/* ptyxis-docker-provider.c
 *
 * Copyright 2026 Eduard Tolosa <tolosaeduard@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Listing takes two steps. `docker ps --format json` prints one object per
 * line and joins Labels and Names with commas, which cannot be split back
 * apart because a label value may itself contain commas. So we ask for
 * identifiers first and then run `docker container inspect`, which returns a
 * JSON array with Labels as a real object.
 *
 * Changes come from `docker events` instead of the file monitors the podman
 * provider uses, because docker keeps its state in /var/lib/docker where only
 * root can read it.
 */

#include "config.h"

#include <signal.h>

#include <json-glib/json-glib.h>

#include "ptyxis-agent-compat.h"
#include "ptyxis-docker-container.h"
#include "ptyxis-docker-provider-private.h"
#include "ptyxis-run-context.h"

#define DOCKER_RELOAD_DELAY_SECONDS       3
#define DOCKER_EVENTS_BACKOFF_MIN_SECONDS 1
#define DOCKER_EVENTS_BACKOFF_MAX_SECONDS 60
#define DOCKER_EVENTS_HEALTHY_USEC        (G_GINT64_CONSTANT (30) * G_USEC_PER_SEC)

struct _PtyxisDockerProvider
{
  PtyxisContainerProvider parent_instance;

  GCancellable *cancellable;

  GSubprocess *events_subprocess;
  GDataInputStream *events_stream;
  gint64 events_started_at;

  /* Only set while ptyxis_docker_provider_update_sync() is waiting */
  GMainLoop *startup_loop;

  guint startup_source;
  guint startup_timeout_source;
  guint queued_update;
  guint events_restart_source;
  guint events_backoff;

  guint is_updating : 1;
  guint needs_update : 1;
  guint available : 1;
};

G_DEFINE_TYPE (PtyxisDockerProvider, ptyxis_docker_provider, PTYXIS_TYPE_CONTAINER_PROVIDER)

static void ptyxis_docker_provider_start_events_monitor    (PtyxisDockerProvider *self);
static void ptyxis_docker_provider_schedule_events_restart (PtyxisDockerProvider *self);
static void ptyxis_docker_provider_queue_update_full       (PtyxisDockerProvider *self,
                                                            guint                 delay_seconds);

static void
ptyxis_docker_provider_startup_done (PtyxisDockerProvider *self)
{
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (self->startup_loop != NULL)
    {
      GMainLoop *loop = self->startup_loop;

      self->startup_loop = NULL;
      g_main_loop_quit (loop);
    }
}

static void
ptyxis_docker_provider_update_finished (PtyxisDockerProvider *self)
{
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  self->is_updating = FALSE;

  ptyxis_docker_provider_startup_done (self);

  /* Don't drop an event that arrived while we were busy */
  if (self->needs_update)
    {
      self->needs_update = FALSE;
      ptyxis_docker_provider_queue_update_full (self, DOCKER_RELOAD_DELAY_SECONDS);
    }
}

char **
_ptyxis_docker_provider_parse_ids (const char *stdout_buf)
{
  g_auto(GStrv) lines = NULL;
  GPtrArray *ids;

  ids = g_ptr_array_new ();

  if (stdout_buf != NULL)
    {
      lines = g_strsplit (stdout_buf, "\n", -1);

      for (guint i = 0; lines[i]; i++)
        {
          char *line = g_strstrip (lines[i]);

          if (line[0] != 0)
            g_ptr_array_add (ids, g_strdup (line));
        }
    }

  g_ptr_array_add (ids, NULL);

  return (char **)g_ptr_array_free (ids, FALSE);
}

static PtyxisDockerContainer *
ptyxis_docker_provider_deserialize (JsonObject *object)
{
  g_autoptr(PtyxisDockerContainer) container = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (object != NULL);

  container = g_object_new (PTYXIS_TYPE_DOCKER_CONTAINER, NULL);

  /* Inspect output varies between docker versions, so skip an entry we cannot
   * read instead of failing the whole refresh.
   */
  if (!ptyxis_docker_container_deserialize (container, object, &error))
    {
      g_debug ("Failed to deserialize container JSON: %s", error->message);
      return NULL;
    }

  return g_steal_pointer (&container);
}

gboolean
_ptyxis_docker_provider_parse_json (PtyxisDockerProvider  *self,
                                    const char            *json,
                                    GError               **error)
{
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GPtrArray) containers = NULL;
  JsonArray *root_array;
  JsonNode *root;

  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));
  g_assert (json != NULL);

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, json, -1, error))
    return FALSE;

  containers = g_ptr_array_new_with_free_func (g_object_unref);

  if ((root = json_parser_get_root (parser)) &&
      JSON_NODE_HOLDS_ARRAY (root) &&
      (root_array = json_node_get_array (root)))
    {
      guint n_elements = json_array_get_length (root_array);

      for (guint i = 0; i < n_elements; i++)
        {
          g_autoptr(PtyxisDockerContainer) container = NULL;
          JsonNode *element = json_array_get_element (root_array, i);
          JsonObject *element_object;

          if (JSON_NODE_HOLDS_OBJECT (element) &&
              (element_object = json_node_get_object (element)) &&
              (container = ptyxis_docker_provider_deserialize (element_object)))
            g_ptr_array_add (containers, g_steal_pointer (&container));
        }
    }

  ptyxis_container_provider_merge (PTYXIS_CONTAINER_PROVIDER (self), containers);

  return TRUE;
}

static void
ptyxis_docker_provider_inspect_cb (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  GSubprocess *subprocess = (GSubprocess *)object;
  g_autoptr(PtyxisDockerProvider) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *stdout_buf = NULL;
  g_autofree char *stderr_buf = NULL;

  g_assert (G_IS_SUBPROCESS (subprocess));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (!g_subprocess_communicate_utf8_finish (subprocess, result, &stdout_buf, &stderr_buf, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to run docker container inspect: %s", error->message);
      ptyxis_docker_provider_update_finished (self);
      return;
    }

  /* Check stdout rather than the exit status. Inspect exits 1 when one of the
   * identifiers no longer exists, which happens whenever a container is removed
   * between listing and inspecting, but it still prints valid JSON for the rest.
   */
  if (stdout_buf == NULL || g_strstrip (stdout_buf)[0] == 0)
    {
      g_debug ("docker container inspect produced no output%s%s",
               stderr_buf && stderr_buf[0] ? ": " : "",
               stderr_buf ? stderr_buf : "");
      ptyxis_docker_provider_update_finished (self);
      return;
    }

  if (!_ptyxis_docker_provider_parse_json (self, stdout_buf, &error))
    g_debug ("Failed to load docker JSON: %s", error->message);

  ptyxis_docker_provider_update_finished (self);
}

static void
ptyxis_docker_provider_list_ids_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  GSubprocess *subprocess = (GSubprocess *)object;
  g_autoptr(PtyxisDockerProvider) self = user_data;
  g_autoptr(PtyxisRunContext) run_context = NULL;
  g_autoptr(GSubprocess) inspect = NULL;
  g_autoptr(GPtrArray) containers = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) ids = NULL;
  g_autofree char *stdout_buf = NULL;
  g_autofree char *stderr_buf = NULL;
  GCancellable *cancellable;
  guint n_ids;

  g_assert (G_IS_SUBPROCESS (subprocess));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (!g_subprocess_communicate_utf8_finish (subprocess, result, &stdout_buf, &stderr_buf, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to run docker ps: %s", error->message);
      ptyxis_docker_provider_update_finished (self);
      return;
    }

  /* A failed listing says nothing about which containers exist, so keep the
   * previous list instead of emptying the menu.
   */
  if (!g_subprocess_get_if_exited (subprocess) ||
      g_subprocess_get_exit_status (subprocess) != 0)
    {
      g_debug ("docker ps failed%s%s",
               stderr_buf && stderr_buf[0] ? ": " : "",
               stderr_buf ? g_strstrip (stderr_buf) : "");
      ptyxis_docker_provider_update_finished (self);
      return;
    }

  ids = _ptyxis_docker_provider_parse_ids (stdout_buf);
  n_ids = g_strv_length (ids);

  /* Inspect with no arguments exits 1 with a usage message. This is also the
   * only case where merging an empty list is correct.
   */
  if (n_ids == 0)
    {
      containers = g_ptr_array_new_with_free_func (g_object_unref);
      ptyxis_container_provider_merge (PTYXIS_CONTAINER_PROVIDER (self), containers);
      ptyxis_docker_provider_update_finished (self);
      return;
    }

  run_context = ptyxis_run_context_new ();

  ptyxis_run_context_push_host (run_context);

  ptyxis_run_context_append_argv (run_context, "docker");
  ptyxis_run_context_append_argv (run_context, "container");
  ptyxis_run_context_append_argv (run_context, "inspect");
  ptyxis_run_context_append_args (run_context, (const char * const *)ids);

  if (!(inspect = ptyxis_run_context_spawn_with_flags (run_context,
                                                       (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                        G_SUBPROCESS_FLAGS_STDERR_PIPE),
                                                       &error)))
    {
      g_debug ("Failed to spawn docker container inspect: %s", error->message);
      ptyxis_docker_provider_update_finished (self);
      return;
    }

  /* Read this first. Argument evaluation order is unspecified and
   * g_steal_pointer() below clears @self.
   */
  cancellable = self->cancellable;

  g_subprocess_communicate_utf8_async (inspect,
                                       NULL,
                                       cancellable,
                                       ptyxis_docker_provider_inspect_cb,
                                       g_steal_pointer (&self));
}

static gboolean
ptyxis_docker_provider_update_source_func (gpointer user_data)
{
  PtyxisDockerProvider *self = user_data;
  g_autoptr(PtyxisRunContext) run_context = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  self->queued_update = 0;
  self->needs_update = FALSE;

  run_context = ptyxis_run_context_new ();

  ptyxis_run_context_push_host (run_context);

  ptyxis_run_context_append_argv (run_context, "docker");
  ptyxis_run_context_append_argv (run_context, "ps");
  ptyxis_run_context_append_argv (run_context, "--all");
  ptyxis_run_context_append_argv (run_context, "--quiet");
  ptyxis_run_context_append_argv (run_context, "--no-trunc");

  if (!(subprocess = ptyxis_run_context_spawn_with_flags (run_context,
                                                          (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                           G_SUBPROCESS_FLAGS_STDERR_PIPE),
                                                          &error)))
    {
      g_debug ("Failed to spawn docker ps: %s", error->message);
      ptyxis_docker_provider_update_finished (self);
      return G_SOURCE_REMOVE;
    }

  self->is_updating = TRUE;

  g_subprocess_communicate_utf8_async (subprocess,
                                       NULL,
                                       self->cancellable,
                                       ptyxis_docker_provider_list_ids_cb,
                                       g_object_ref (self));

  return G_SOURCE_REMOVE;
}

static void
ptyxis_docker_provider_queue_update_full (PtyxisDockerProvider *self,
                                          guint                 delay_seconds)
{
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (g_cancellable_is_cancelled (self->cancellable))
    return;

  if (self->is_updating)
    {
      self->needs_update = TRUE;
      return;
    }

  if (self->queued_update == 0)
    self->queued_update = g_timeout_add_seconds_full (G_PRIORITY_LOW,
                                                      delay_seconds,
                                                      ptyxis_docker_provider_update_source_func,
                                                      self, NULL);
}

static void
ptyxis_docker_provider_queue_update (PtyxisDockerProvider *self)
{
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (!self->available)
    return;

  ptyxis_docker_provider_queue_update_full (self, DOCKER_RELOAD_DELAY_SECONDS);
}

static void
ptyxis_docker_provider_stop_events_monitor (PtyxisDockerProvider *self)
{
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (self->events_subprocess != NULL)
    {
      /* When sandboxed our direct child is flatpak-spawn, which forwards
       * SIGTERM to the host process but cannot forward SIGKILL.
       */
      g_subprocess_send_signal (self->events_subprocess, SIGTERM);
      g_subprocess_force_exit (self->events_subprocess);
      g_clear_object (&self->events_subprocess);
    }

  g_clear_object (&self->events_stream);

  self->events_started_at = 0;
}

static gboolean
ptyxis_docker_provider_events_restart_source_func (gpointer user_data)
{
  PtyxisDockerProvider *self = user_data;

  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  self->events_restart_source = 0;

  ptyxis_docker_provider_start_events_monitor (self);

  /* Events sent while the stream was down were missed, so list again. Skip it
   * when the monitor did not come back, or each retry would run two failing
   * commands instead of one.
   */
  if (self->events_subprocess != NULL)
    ptyxis_docker_provider_queue_update (self);

  return G_SOURCE_REMOVE;
}

static void
ptyxis_docker_provider_schedule_events_restart (PtyxisDockerProvider *self)
{
  gboolean was_healthy;

  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  /* Read this before stopping, which clears the start time. A stream that ran
   * for a while was working, so treat whatever ended it as a one-off and retry
   * quickly.
   */
  was_healthy = self->events_started_at != 0 &&
                g_get_monotonic_time () - self->events_started_at >= DOCKER_EVENTS_HEALTHY_USEC;

  ptyxis_docker_provider_stop_events_monitor (self);

  if (!self->available || g_cancellable_is_cancelled (self->cancellable))
    return;

  if (was_healthy)
    self->events_backoff = 0;

  if (self->events_backoff == 0)
    self->events_backoff = DOCKER_EVENTS_BACKOFF_MIN_SECONDS;
  else
    self->events_backoff = MIN (self->events_backoff * 2, DOCKER_EVENTS_BACKOFF_MAX_SECONDS);

  g_clear_handle_id (&self->events_restart_source, g_source_remove);
  self->events_restart_source =
    g_timeout_add_seconds_full (G_PRIORITY_LOW,
                                self->events_backoff,
                                ptyxis_docker_provider_events_restart_source_func,
                                self, NULL);
}

static void
ptyxis_docker_provider_events_read_line_cb (GObject      *object,
                                            GAsyncResult *result,
                                            gpointer      user_data)
{
  GDataInputStream *stream = (GDataInputStream *)object;
  g_autoptr(PtyxisDockerProvider) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *line = NULL;

  g_assert (G_IS_DATA_INPUT_STREAM (stream));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  line = g_data_input_stream_read_line_finish_utf8 (stream, result, NULL, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  /* The monitor may have been restarted while this read was pending */
  if (stream != self->events_stream)
    return;

  if (error != NULL)
    {
      g_debug ("Failed to read from docker events: %s", error->message);
      ptyxis_docker_provider_schedule_events_restart (self);
      return;
    }

  if (line == NULL)
    {
      g_debug ("docker events stream closed");
      ptyxis_docker_provider_schedule_events_restart (self);
      return;
    }

  /* Any line means the stream works. We use it only as a wakeup. */
  self->events_backoff = 0;

  ptyxis_docker_provider_queue_update (self);

  g_data_input_stream_read_line_async (self->events_stream,
                                       G_PRIORITY_LOW,
                                       self->cancellable,
                                       ptyxis_docker_provider_events_read_line_cb,
                                       g_object_ref (self));
}

static void
ptyxis_docker_provider_start_events_monitor (PtyxisDockerProvider *self)
{
  g_autoptr(PtyxisRunContext) run_context = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (!self->available || g_cancellable_is_cancelled (self->cancellable))
    return;

  ptyxis_docker_provider_stop_events_monitor (self);

  run_context = ptyxis_run_context_new ();

  ptyxis_run_context_push_host (run_context);

  ptyxis_run_context_append_argv (run_context, "docker");
  ptyxis_run_context_append_argv (run_context, "events");

  /* type=container on its own also reports exec_create, exec_start and
   * exec_die, which health checks produce constantly and which would each cost
   * a full re-listing. Repeated values for one filter key are ORed together.
   * Do not add --since, it replays past events on every restart.
   */
  ptyxis_run_context_append_argv (run_context, "--filter=type=container");
  ptyxis_run_context_append_argv (run_context, "--filter=event=create");
  ptyxis_run_context_append_argv (run_context, "--filter=event=start");
  ptyxis_run_context_append_argv (run_context, "--filter=event=die");
  ptyxis_run_context_append_argv (run_context, "--filter=event=destroy");
  ptyxis_run_context_append_argv (run_context, "--filter=event=rename");

  /* The default format embeds every label of the container in every event */
  ptyxis_run_context_append_argv (run_context, "--format={{.Action}} {{.Actor.ID}}");

  if (!(subprocess = ptyxis_run_context_spawn_with_flags (run_context,
                                                          (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                           G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                                          &error)))
    {
      g_debug ("Failed to spawn docker events: %s", error->message);
      ptyxis_docker_provider_schedule_events_restart (self);
      return;
    }

  self->events_subprocess = g_steal_pointer (&subprocess);
  self->events_started_at = g_get_monotonic_time ();

  self->events_stream = g_data_input_stream_new (g_subprocess_get_stdout_pipe (self->events_subprocess));
  g_data_input_stream_set_newline_type (self->events_stream, G_DATA_STREAM_NEWLINE_TYPE_LF);
  g_buffered_input_stream_set_buffer_size (G_BUFFERED_INPUT_STREAM (self->events_stream), 4096);

  g_data_input_stream_read_line_async (self->events_stream,
                                       G_PRIORITY_LOW,
                                       self->cancellable,
                                       ptyxis_docker_provider_events_read_line_cb,
                                       g_object_ref (self));
}

static void
ptyxis_docker_provider_version_cb (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  GSubprocess *subprocess = (GSubprocess *)object;
  g_autoptr(PtyxisDockerProvider) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *stdout_buf = NULL;

  g_assert (G_IS_SUBPROCESS (subprocess));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  if (!g_subprocess_communicate_utf8_finish (subprocess, result, &stdout_buf, NULL, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to run docker --version: %s", error->message);
      ptyxis_docker_provider_startup_done (self);
      return;
    }

  /* Some distributions ship a podman-docker package that installs
   * /usr/bin/docker as a wrapper around podman. Without this check every podman
   * container would be listed twice, with the same identifier, by two providers.
   */
  if (stdout_buf == NULL ||
      !g_str_has_prefix (g_strstrip (stdout_buf), "Docker version "))
    {
      g_debug ("`docker --version` did not identify as Docker (got \"%s\"), "
               "assuming the CLI is missing or is a podman shim",
               stdout_buf != NULL ? stdout_buf : "");
      ptyxis_docker_provider_startup_done (self);
      return;
    }

  self->available = TRUE;

  ptyxis_docker_provider_queue_update_full (self, 0);
  ptyxis_docker_provider_start_events_monitor (self);
}

static gboolean
ptyxis_docker_provider_startup_source_func (gpointer user_data)
{
  PtyxisDockerProvider *self = user_data;
  g_autoptr(PtyxisRunContext) run_context = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  self->startup_source = 0;

  run_context = ptyxis_run_context_new ();

  ptyxis_run_context_push_host (run_context);

  /* `docker --version` is answered by the client and never contacts the daemon,
   * so it cannot hang the way `docker version` can. stderr is silenced because
   * the podman-docker wrapper prints a notice there.
   */
  ptyxis_run_context_append_argv (run_context, "docker");
  ptyxis_run_context_append_argv (run_context, "--version");

  if (!(subprocess = ptyxis_run_context_spawn_with_flags (run_context,
                                                          (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                           G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                                          &error)))
    {
      g_debug ("Docker does not appear to be available: %s", error->message);
      ptyxis_docker_provider_startup_done (self);
      return G_SOURCE_REMOVE;
    }

  g_subprocess_communicate_utf8_async (subprocess,
                                       NULL,
                                       self->cancellable,
                                       ptyxis_docker_provider_version_cb,
                                       g_object_ref (self));

  return G_SOURCE_REMOVE;
}

static gboolean
ptyxis_docker_provider_startup_timeout_cb (gpointer user_data)
{
  PtyxisDockerProvider *self = user_data;

  g_assert (PTYXIS_IS_DOCKER_PROVIDER (self));

  self->startup_timeout_source = 0;

  g_debug ("Timed out waiting for the initial docker listing");
  ptyxis_docker_provider_startup_done (self);

  return G_SOURCE_REMOVE;
}

/**
 * ptyxis_docker_provider_update_sync:
 * @timeout_msec: how long to wait before giving up
 *
 * Looks for docker and performs the first listing, returning when that finishes
 * or @timeout_msec has passed. Runs the asynchronous path from a nested main
 * loop rather than repeating it. Anything unfinished by the deadline arrives
 * later over ContainersChanged.
 */
void
ptyxis_docker_provider_update_sync (PtyxisDockerProvider *self,
                                    guint                 timeout_msec)
{
  g_autoptr(GMainLoop) loop = NULL;

  g_return_if_fail (PTYXIS_IS_DOCKER_PROVIDER (self));
  g_return_if_fail (self->startup_loop == NULL);

  loop = g_main_loop_new (NULL, FALSE);
  self->startup_loop = loop;

  self->startup_timeout_source =
    g_timeout_add_full (G_PRIORITY_HIGH,
                        timeout_msec,
                        ptyxis_docker_provider_startup_timeout_cb,
                        self, NULL);

  /* Probe now rather than waiting for the idle armed in constructed() */
  g_clear_handle_id (&self->startup_source, g_source_remove);
  ptyxis_docker_provider_startup_source_func (self);

  if (self->startup_loop != NULL)
    g_main_loop_run (loop);

  self->startup_loop = NULL;
  g_clear_handle_id (&self->startup_timeout_source, g_source_remove);
}

static void
ptyxis_docker_provider_constructed (GObject *object)
{
  PtyxisDockerProvider *self = (PtyxisDockerProvider *)object;

  G_OBJECT_CLASS (ptyxis_docker_provider_parent_class)->constructed (object);

  /* Deferring the probe keeps construction from spawning anything, which is
   * what lets the testsuite create a provider.
   */
  self->startup_source = g_timeout_add_full (G_PRIORITY_LOW,
                                             0,
                                             ptyxis_docker_provider_startup_source_func,
                                             self, NULL);
}

static void
ptyxis_docker_provider_dispose (GObject *object)
{
  PtyxisDockerProvider *self = (PtyxisDockerProvider *)object;

  /* Cancel first so pending callbacks return early instead of using state we
   * are about to free.
   */
  g_cancellable_cancel (self->cancellable);

  ptyxis_docker_provider_startup_done (self);

  g_clear_handle_id (&self->startup_source, g_source_remove);
  g_clear_handle_id (&self->startup_timeout_source, g_source_remove);
  g_clear_handle_id (&self->queued_update, g_source_remove);
  g_clear_handle_id (&self->events_restart_source, g_source_remove);

  ptyxis_docker_provider_stop_events_monitor (self);

  G_OBJECT_CLASS (ptyxis_docker_provider_parent_class)->dispose (object);
}

static void
ptyxis_docker_provider_finalize (GObject *object)
{
  PtyxisDockerProvider *self = (PtyxisDockerProvider *)object;

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (ptyxis_docker_provider_parent_class)->finalize (object);
}

static void
ptyxis_docker_provider_class_init (PtyxisDockerProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ptyxis_docker_provider_constructed;
  object_class->dispose = ptyxis_docker_provider_dispose;
  object_class->finalize = ptyxis_docker_provider_finalize;
}

static void
ptyxis_docker_provider_init (PtyxisDockerProvider *self)
{
  self->cancellable = g_cancellable_new ();
}

PtyxisContainerProvider *
ptyxis_docker_provider_new (void)
{
  return g_object_new (PTYXIS_TYPE_DOCKER_PROVIDER, NULL);
}

/* ptyxis-docker-container.c
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

/* This mirrors ptyxis-podman-container.c. The spawn and find-program code is
 * kept almost identical so a fix to one can be copied to the other.
 */

#include "config.h"

#include <unistd.h>

#include <glib/gstdio.h>

#include "ptyxis-agent-util.h"
#include "ptyxis-docker-container.h"
#include "ptyxis-process-impl.h"
#include "ptyxis-run-context.h"

struct _PtyxisDockerContainer
{
  PtyxisIpcContainerSkeleton parent_instance;

  GHashTable *labels;
  gboolean has_started;
  gboolean has_unshared_groups;
};

static void container_iface_init (PtyxisIpcContainerIface *iface);

G_DEFINE_TYPE_WITH_CODE (PtyxisDockerContainer, ptyxis_docker_container, PTYXIS_IPC_TYPE_CONTAINER_SKELETON,
                         G_IMPLEMENT_INTERFACE (PTYXIS_IPC_TYPE_CONTAINER, container_iface_init))

/* distrobox can use docker as its backend, and such a container has to be
 * entered through distrobox rather than with a plain exec.
 *
 * This mirrors distrobox's own IsDistrobox(). The manager label is the normal
 * case, but tools layered on top of distrobox, such as apx, replace it with
 * their own name. Those containers still carry the labels distrobox sets for
 * itself, so the key prefix catches them.
 */
static gboolean
container_is_distrobox (PtyxisDockerContainer *self)
{
  GHashTableIter iter;
  const char *key;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));

  if (g_strcmp0 (ptyxis_docker_container_lookup_label (self, "manager"), "distrobox") == 0)
    return TRUE;

  g_hash_table_iter_init (&iter, self->labels);

  while (g_hash_table_iter_next (&iter, (gpointer *)&key, NULL))
    {
      if (g_str_has_prefix (key, "distrobox."))
        return TRUE;
    }

  return FALSE;
}

static void
maybe_start_cb (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
  GSubprocess *subprocess = (GSubprocess *)object;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_SUBPROCESS (subprocess));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  if (!g_subprocess_wait_check_finish (subprocess, result, &error))
    g_task_return_error (task, g_steal_pointer (&error));
  else
    g_task_return_boolean (task, TRUE);
}

static void
maybe_start (PtyxisDockerContainer *self,
             GCancellable          *cancellable,
             GAsyncReadyCallback    callback,
             gpointer               user_data)
{
  g_autoptr(PtyxisRunContext) run_context = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  const char *id;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, maybe_start);

  id = ptyxis_ipc_container_get_id (PTYXIS_IPC_CONTAINER (self));
  g_assert (id != NULL && id[0] != 0);

  if (self->has_started)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  self->has_started = TRUE;

  /* distrobox starts the container itself, and starting it here races with
   * that. See https://gitlab.gnome.org/GNOME/ptyxis/-/issues/31
   */
  if (container_is_distrobox (self))
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  /* The container may have been stopped since it was discovered, so always
   * start it. Docker exits 0 if it is already running.
   */
  run_context = ptyxis_run_context_new ();

  /* In case we're sandboxed */
  ptyxis_run_context_push_host (run_context);

  ptyxis_run_context_append_argv (run_context, "docker");
  ptyxis_run_context_append_argv (run_context, "start");
  ptyxis_run_context_append_argv (run_context, id);

  /* Wait so that we don't try to run before the container has started */
  if ((subprocess = ptyxis_run_context_spawn (run_context, &error)))
    g_subprocess_wait_check_async (subprocess,
                                   cancellable,
                                   maybe_start_cb,
                                   g_steal_pointer (&task));
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

static gboolean
maybe_start_finish (PtyxisDockerContainer  *self,
                    GAsyncResult           *result,
                    GError                **error)
{
  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (G_IS_TASK (result));

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
ptyxis_docker_container_deserialize_labels (PtyxisDockerContainer *self,
                                            JsonObject            *labels)
{
  JsonObjectIter iter;
  const char *key;
  JsonNode *value;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (labels != NULL);

  json_object_iter_init (&iter, labels);

  while (json_object_iter_next (&iter, &key, &value))
    {
      if (JSON_NODE_HOLDS_VALUE (value) &&
          json_node_get_value_type (value) == G_TYPE_STRING)
        {
          const char *value_str = json_node_get_string (value);

          g_hash_table_insert (self->labels, g_strdup (key), g_strdup (value_str));
        }
    }
}

static void
ptyxis_docker_container_deserialize_name (PtyxisDockerContainer *self,
                                          const char            *name,
                                          const char            *id)
{
  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (id != NULL);

  /* Inspect reports names as "/my-container" */
  if (name != NULL && name[0] == '/')
    name++;

  if (name != NULL && name[0] != 0)
    {
      ptyxis_ipc_container_set_display_name (PTYXIS_IPC_CONTAINER (self), name);
    }
  else
    {
      g_autofree char *short_id = g_strndup (id, 12);

      ptyxis_ipc_container_set_display_name (PTYXIS_IPC_CONTAINER (self), short_id);
    }
}

static JsonObject *
get_member_object (JsonObject *object,
                   const char *member)
{
  JsonNode *node;

  g_assert (object != NULL);
  g_assert (member != NULL);

  /* Docker uses JSON null for a container without labels */
  if (json_object_has_member (object, member) &&
      (node = json_object_get_member (object, member)) &&
      JSON_NODE_HOLDS_OBJECT (node))
    return json_node_get_object (node);

  return NULL;
}

gboolean
ptyxis_docker_container_deserialize (PtyxisDockerContainer  *self,
                                     JsonObject             *object,
                                     GError                **error)
{
  JsonObject *config_object;
  JsonObject *labels_object;
  JsonNode *name;
  JsonNode *id;
  const char *id_str;

  g_return_val_if_fail (PTYXIS_IS_DOCKER_CONTAINER (self), FALSE);
  g_return_val_if_fail (object != NULL, FALSE);

  if (!(json_object_has_member (object, "Id") &&
        (id = json_object_get_member (object, "Id")) &&
        JSON_NODE_HOLDS_VALUE (id) &&
        json_node_get_value_type (id) == G_TYPE_STRING &&
        (id_str = json_node_get_string (id)) &&
        id_str[0] != 0))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Failed to locate Id in docker container description");
      return FALSE;
    }

  ptyxis_ipc_container_set_id (PTYXIS_IPC_CONTAINER (self), id_str);

  if ((config_object = get_member_object (object, "Config")) &&
      (labels_object = get_member_object (config_object, "Labels")))
    ptyxis_docker_container_deserialize_labels (self, labels_object);

  /* A distrobox created with --init or --unshare-groups does not want our
   * --tty workaround. See #477 #545
   */
  self->has_unshared_groups =
    g_strcmp0 (ptyxis_docker_container_lookup_label (self, "distrobox.unshare_groups"), "1") == 0;

  if (json_object_has_member (object, "Name") &&
      (name = json_object_get_member (object, "Name")) &&
      JSON_NODE_HOLDS_VALUE (name) &&
      json_node_get_value_type (name) == G_TYPE_STRING)
    ptyxis_docker_container_deserialize_name (self, json_node_get_string (name), id_str);
  else
    ptyxis_docker_container_deserialize_name (self, NULL, id_str);

  return TRUE;
}

const char *
ptyxis_docker_container_lookup_label (PtyxisDockerContainer *self,
                                      const char            *key)
{
  g_return_val_if_fail (PTYXIS_IS_DOCKER_CONTAINER (self), NULL);
  g_return_val_if_fail (self->labels != NULL, NULL);

  return g_hash_table_lookup (self->labels, key);
}

static void
append_distrobox_enter (PtyxisDockerContainer *self,
                        PtyxisRunContext      *run_context,
                        const char * const    *argv,
                        const char * const    *env,
                        const char            *cwd)
{
  const char *name = ptyxis_ipc_container_get_display_name (PTYXIS_IPC_CONTAINER (self));

  ptyxis_run_context_append_argv (run_context, "distrobox");
  ptyxis_run_context_append_argv (run_context, "enter");

  if (!self->has_unshared_groups)
    ptyxis_run_context_append_argv (run_context, "--no-tty");

  ptyxis_run_context_append_argv (run_context, name);

  /* Podman also passes --preserve-fds through here, but `docker exec` has no
   * such option and Ptyxis never maps a descriptor above stderr.
   */
  if (!self->has_unshared_groups)
    {
      ptyxis_run_context_append_argv (run_context, "--additional-flags");
      ptyxis_run_context_append_argv (run_context, "--tty");
    }

  ptyxis_run_context_append_argv (run_context, "--");
  ptyxis_run_context_append_argv (run_context, "env");

  if (cwd != NULL && cwd[0] != 0)
    {
      if (g_file_test (cwd, G_FILE_TEST_EXISTS))
        ptyxis_run_context_set_cwd (run_context, cwd);
      else
        ptyxis_run_context_append_formatted (run_context, "--chdir=%s", cwd);
    }

  if (env != NULL)
    ptyxis_run_context_append_args (run_context, env);

  ptyxis_run_context_append_args (run_context, argv);
}

/* The user and working directory are left to the image: a plain docker image
 * has no account for the user running Ptyxis, and the host directory does not
 * exist inside the container.
 */
static void
append_docker_exec (PtyxisDockerContainer *self,
                    PtyxisRunContext      *run_context,
                    const char * const    *argv,
                    const char * const    *env,
                    gboolean               has_tty)
{
  ptyxis_run_context_append_argv (run_context, "docker");
  ptyxis_run_context_append_argv (run_context, "exec");
  ptyxis_run_context_append_argv (run_context, "--privileged");
  ptyxis_run_context_append_argv (run_context, "--interactive");

  /* Make sure that we request TTY ioctls if necessary */
  if (has_tty)
    ptyxis_run_context_append_argv (run_context, "--tty");

  /* Keep docker from stealing ctrl+p. Unlike podman this needs no version
   * check, --detach-keys has been in `docker exec` since 1.9.
   */
  ptyxis_run_context_append_argv (run_context, "--detach-keys=");

  /* Append --env=FOO=BAR environment variables */
  for (guint i = 0; env[i]; i++)
    ptyxis_run_context_append_formatted (run_context, "--env=%s", env[i]);

  /* Now specify our runtime identifier */
  ptyxis_run_context_append_argv (run_context,
                                  ptyxis_ipc_container_get_id (PTYXIS_IPC_CONTAINER (self)));

  /* Finally, propagate the upper layer's command arguments */
  ptyxis_run_context_append_args (run_context, argv);
}

static gboolean
ptyxis_docker_container_run_context_cb (PtyxisRunContext    *run_context,
                                        const char * const  *argv,
                                        const char * const  *env,
                                        const char          *cwd,
                                        PtyxisUnixFDMap     *unix_fd_map,
                                        gpointer             user_data,
                                        GError             **error)
{
  PtyxisDockerContainer *self = user_data;
  gboolean has_tty = FALSE;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (PTYXIS_IS_RUN_CONTEXT (run_context));
  g_assert (argv != NULL);
  g_assert (env != NULL);
  g_assert (PTYXIS_IS_UNIX_FD_MAP (unix_fd_map));

  /* Make sure that we request TTY ioctls if necessary */
  if (ptyxis_unix_fd_map_stdin_isatty (unix_fd_map) ||
      ptyxis_unix_fd_map_stdout_isatty (unix_fd_map) ||
      ptyxis_unix_fd_map_stderr_isatty (unix_fd_map))
    has_tty = TRUE;

  /* Make sure we can pass the FDs down */
  if (!ptyxis_run_context_merge_unix_fd_map (run_context, unix_fd_map, error))
    return FALSE;

  if (container_is_distrobox (self))
    append_distrobox_enter (self, run_context, argv, env, cwd);
  else
    append_docker_exec (self, run_context, argv, env, has_tty);

  return TRUE;
}

static void
ptyxis_docker_container_prepare_run_context (PtyxisDockerContainer *self,
                                             PtyxisRunContext      *run_context)
{
  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (PTYXIS_IS_RUN_CONTEXT (run_context));

  /* These seem to be needed for distrobox-enter */
  if (container_is_distrobox (self))
    {
      ptyxis_run_context_setenv (run_context, "HOME", g_get_home_dir ());
      ptyxis_run_context_setenv (run_context, "USER", g_get_user_name ());
    }

  ptyxis_run_context_push_host (run_context);

  ptyxis_run_context_push (run_context,
                           ptyxis_docker_container_run_context_cb,
                           g_object_ref (self),
                           g_object_unref);

  /* Give access to some minimal state in the environment from
   * our host system.
   */
  ptyxis_run_context_add_minimal_environment (run_context);

  /* We don't want HOME propagated because the host home directory
   * generally does not exist inside the container.
   */
  ptyxis_run_context_setenv (run_context, "HOME", NULL);
}

static void
ptyxis_docker_container_dispose (GObject *object)
{
  PtyxisDockerContainer *self = (PtyxisDockerContainer *)object;

  g_hash_table_remove_all (self->labels);

  G_OBJECT_CLASS (ptyxis_docker_container_parent_class)->dispose (object);
}

static void
ptyxis_docker_container_finalize (GObject *object)
{
  PtyxisDockerContainer *self = (PtyxisDockerContainer *)object;

  g_clear_pointer (&self->labels, g_hash_table_unref);

  G_OBJECT_CLASS (ptyxis_docker_container_parent_class)->finalize (object);
}

static void
ptyxis_docker_container_class_init (PtyxisDockerContainerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ptyxis_docker_container_dispose;
  object_class->finalize = ptyxis_docker_container_finalize;
}

static void
ptyxis_docker_container_init (PtyxisDockerContainer *self)
{
  self->labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  ptyxis_ipc_container_set_icon_name (PTYXIS_IPC_CONTAINER (self), "container-generic-symbolic");
  ptyxis_ipc_container_set_provider (PTYXIS_IPC_CONTAINER (self), "docker");
}

static void
ptyxis_docker_container_handle_spawn_cb (GObject      *object,
                                         GAsyncResult *result,
                                         gpointer      user_data)
{
  PtyxisDockerContainer *self = (PtyxisDockerContainer *)object;
  g_autoptr(PtyxisRunContext) run_context = user_data;
  g_autoptr(PtyxisIpcProcess) process = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *object_path = NULL;
  g_autofree char *guid = NULL;
  GDBusMethodInvocation *invocation;
  GDBusConnection *connection;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_RUN_CONTEXT (run_context));

  invocation = g_object_get_data (G_OBJECT (run_context), "INVOCATION");
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  connection = g_dbus_method_invocation_get_connection (invocation);
  g_assert (G_IS_DBUS_CONNECTION (connection));

  out_fd_list = g_unix_fd_list_new ();
  guid = g_dbus_generate_guid ();
  object_path = g_strdup_printf ("/org/gnome/Ptyxis/Process/%s", guid);

  if (!maybe_start_finish (self, result, &error) ||
      !(subprocess = ptyxis_run_context_spawn (run_context, &error)) ||
      !(process = ptyxis_process_impl_new (connection, subprocess, object_path, &error)))
    {
      g_dbus_method_invocation_return_gerror (g_object_ref (invocation), error);
      return;
    }

  ptyxis_ipc_container_complete_spawn (PTYXIS_IPC_CONTAINER (self),
                                       g_object_ref (invocation),
                                       out_fd_list,
                                       object_path);
}

static gboolean
ptyxis_docker_container_handle_spawn (PtyxisIpcContainer    *container,
                                      GDBusMethodInvocation *invocation,
                                      GUnixFDList           *in_fd_list,
                                      const char            *cwd,
                                      const char * const    *argv,
                                      GVariant              *in_fds,
                                      GVariant              *in_env)
{
  PtyxisDockerContainer *self = (PtyxisDockerContainer *)container;
  g_autoptr(PtyxisRunContext) run_context = NULL;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (G_IS_UNIX_FD_LIST (in_fd_list));
  g_assert (cwd != NULL);
  g_assert (argv != NULL);
  g_assert (in_fds != NULL);
  g_assert (in_env != NULL);

  run_context = ptyxis_run_context_new ();

  ptyxis_docker_container_prepare_run_context (self, run_context);

  /* Now do our normal handling of the layer requested by the user. */
  ptyxis_agent_push_spawn (run_context, in_fd_list, cwd, argv, in_fds, in_env);

  g_object_set_data_full (G_OBJECT (run_context),
                          "INVOCATION",
                          g_steal_pointer (&invocation),
                          g_object_unref);

  maybe_start (self,
               NULL,
               ptyxis_docker_container_handle_spawn_cb,
               g_steal_pointer (&run_context));

  return TRUE;
}

typedef struct
{
  GDBusMethodInvocation *invocation;
  PtyxisDockerContainer *container;
  char *id;
  char *program;
} FindContainerInPath;

static void
find_container_in_path_free (FindContainerInPath *state)
{
  g_clear_object (&state->container);
  g_clear_object (&state->invocation);
  g_clear_pointer (&state->id, g_free);
  g_clear_pointer (&state->program, g_free);
  g_free (state);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FindContainerInPath, find_container_in_path_free)

static void
ptyxis_docker_container_which_cb (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  GSubprocess *subprocess = (GSubprocess *)object;
  g_autoptr(FindContainerInPath) state = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *stdout_buf = NULL;

  g_assert (G_IS_SUBPROCESS (subprocess));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (state != NULL);
  g_assert (PTYXIS_IS_DOCKER_CONTAINER (state->container));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (state->invocation));
  g_assert (state->id != NULL);
  g_assert (state->program != NULL);

  if (!g_subprocess_communicate_utf8_finish (subprocess, result, &stdout_buf, NULL, &error))
    g_dbus_method_invocation_return_gerror (g_steal_pointer (&state->invocation),
                                            g_steal_pointer (&error));
  else
    ptyxis_ipc_container_complete_find_program_in_path (PTYXIS_IPC_CONTAINER (state->container),
                                                        g_steal_pointer (&state->invocation),
                                                        g_strstrip (stdout_buf));
}

static void
ptyxis_docker_container_find_program_in_path_start_cb (GObject      *object,
                                                       GAsyncResult *result,
                                                       gpointer      user_data)
{
  PtyxisDockerContainer *self = (PtyxisDockerContainer *)object;
  g_autoptr(PtyxisRunContext) run_context = NULL;
  g_autoptr(FindContainerInPath) state = user_data;
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (state != NULL);
  g_assert (PTYXIS_IS_DOCKER_CONTAINER (state->container));
  g_assert (state->container == self);
  g_assert (G_IS_DBUS_METHOD_INVOCATION (state->invocation));
  g_assert (state->id != NULL);
  g_assert (state->program != NULL);

  if (!maybe_start_finish (self, result, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&state->invocation),
                                              g_steal_pointer (&error));
      return;
    }

  run_context = ptyxis_run_context_new ();

  /* In case we're sandboxed */
  ptyxis_run_context_push_host (run_context);
  ptyxis_run_context_append_argv (run_context, "docker");
  ptyxis_run_context_append_argv (run_context, "exec");
  ptyxis_run_context_append_argv (run_context, state->id);
  ptyxis_run_context_append_argv (run_context, "which");
  ptyxis_run_context_append_argv (run_context, state->program);

  if (!(subprocess = ptyxis_run_context_spawn_with_flags (run_context, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error)))
    g_dbus_method_invocation_return_gerror (g_steal_pointer (&state->invocation),
                                            g_steal_pointer (&error));
  else
    g_subprocess_communicate_utf8_async (subprocess,
                                         NULL,
                                         NULL,
                                         ptyxis_docker_container_which_cb,
                                         g_steal_pointer (&state));
}

static gboolean
ptyxis_docker_container_handle_find_program_in_path (PtyxisIpcContainer    *container,
                                                     GDBusMethodInvocation *invocation,
                                                     const char            *program)
{
  FindContainerInPath *state;
  const char *id;

  g_assert (PTYXIS_IS_DOCKER_CONTAINER (container));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  id = ptyxis_ipc_container_get_id (container);

  g_assert (id != NULL);
  g_assert (id[0] != 0);

  state = g_new0 (FindContainerInPath, 1);
  state->invocation = g_steal_pointer (&invocation);
  state->id = g_strdup (id);
  state->program = g_strdup (program);
  state->container = g_object_ref (PTYXIS_DOCKER_CONTAINER (container));

  maybe_start (PTYXIS_DOCKER_CONTAINER (container),
               NULL,
               ptyxis_docker_container_find_program_in_path_start_cb,
               state);

  return TRUE;
}

static void
container_iface_init (PtyxisIpcContainerIface *iface)
{
  iface->handle_spawn = ptyxis_docker_container_handle_spawn;
  iface->handle_find_program_in_path = ptyxis_docker_container_handle_find_program_in_path;
}

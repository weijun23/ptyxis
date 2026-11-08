/*
 * ptyxis-terminal-surface.c
 *
 * Copyright 2023-2024 Christian Hergert <christian@sourceandstack.com>
 * Copyright 2024 Sebastian Wick
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

#include "config.h"

#include <glib/gi18n.h>

#ifdef __linux__
# include <libportal/portal.h>
# include <libportal-gtk4/portal-gtk4.h>
#endif

#include "ptyxis-agent-ipc.h"
#include "ptyxis-application.h"
#include "ptyxis-enums.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-terminal.h"
#include "ptyxis-util.h"

struct _PtyxisTerminalSurface
{
  GObject parent_instance;

  PtyxisTerminal          *terminal;           /* (owned) (not nullable) */
  GtkScrolledWindow       *scrolled_window;    /* (owned) (not nullable) */
  PtyxisProfile           *profile;            /* (owned) (not nullable) */
  GBindingGroup           *profile_bindings;   /* (owned) (not nullable) */
  GSignalGroup            *profile_signals;    /* (owned) (not nullable) */
  PtyxisIpcProcess        *process;            /* (owned) (nullable) */
  PtyxisIpcContainer      *container_at_creation; /* (owned) (nullable) */
  char                   **command;            /* (owned) (nullable) */
  char                   **environment;        /* (owned) (nullable) */
  char                    *initial_working_directory_uri; /* (owned) (nullable) */
  char                    *previous_working_directory_uri; /* (owned) (nullable) */
  char                    *initial_title;      /* (owned) (nullable) */
  char                    *title_prefix;       /* (owned) (nullable) */
  char                    *command_line;        /* (owned) (nullable) */
  char                    *program_name;        /* (owned) (nullable) */

  PtyxisTerminalSurfaceState state;
  GPid                     pid;
  int64_t                  respawn_time;

  PtyxisZoomLevel          zoom : 5;
  PtyxisProcessLeaderKind  leader_kind : 3;
  unsigned int             has_foreground_process : 1;
  unsigned int             forced_exit : 1;
  unsigned int             ignore_osc_title : 1;
  unsigned int             disable_dynamic_title : 1;
  unsigned int             keep_terminal_open : 1;
  unsigned int             has_pressed_key : 1;
};

enum {
  PROP_0,
  PROP_COMMAND_LINE,
  PROP_DISABLE_DYNAMIC_TITLE,
  PROP_ICON,
  PROP_IGNORE_OSC_TITLE,
  PROP_INITIAL_TITLE,
  PROP_PROCESS_LEADER_KIND,
  PROP_PROFILE,
  PROP_PROGRESS,
  PROP_PROGRESS_FRACTION,
  PROP_STATE,
  PROP_SUBTITLE,
  PROP_TERMINAL,
  PROP_TITLE,
  PROP_TITLE_PREFIX,
  PROP_ZOOM,
  PROP_ZOOM_LABEL,
  N_PROPS
};

enum {
  BELL,
  COMMIT,
  NOTIFY_USER,
  REQUEST_CLOSE,
  N_SIGNALS
};

static void ptyxis_terminal_surface_do_respawn (PtyxisTerminalSurface *self);
static void ptyxis_terminal_surface_profile_signals_bind_cb (PtyxisTerminalSurface *self,
                                                             PtyxisProfile         *profile,
                                                             GSignalGroup          *group);

G_DEFINE_FINAL_TYPE (PtyxisTerminalSurface,
                     ptyxis_terminal_surface,
                     G_TYPE_OBJECT)

#ifdef __linux__
static XdpPortal *portal;
#endif

static GParamSpec *properties[N_PROPS];
static unsigned int signals[N_SIGNALS];
static double zoom_font_scales[] = {
  0,

  /* MINUS_14 through MINUS_1: each step is 1.2^(1/2) ≈ 1.095445 */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2),                     /* MINUS_14: 1.2^(-7) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332, /* MINUS_13: 1.2^(-6.5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2),                           /* MINUS_12: 1.2^(-6) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332,       /* MINUS_11: 1.2^(-5.5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2),                                 /* MINUS_10: 1.2^(-5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332,             /* MINUS_9: 1.2^(-4.5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2),                                       /* MINUS_8: 1.2^(-4) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332,                   /* MINUS_7: 1.2^(-3.5) */
  1.0 / (1.2 * 1.2 * 1.2),                                             /* MINUS_6: 1.2^(-3) */
  1.0 / (1.2 * 1.2 * 1.2) * 1.095445115010332,                         /* MINUS_5: 1.2^(-2.5) */
  1.0 / (1.2 * 1.2),                                                   /* MINUS_4: 1.2^(-2) */
  1.0 / (1.2 * 1.2) * 1.095445115010332,                               /* MINUS_3: 1.2^(-1.5) */
  1.0 / (1.2),                                                         /* MINUS_2: 1.2^(-1) */
  1.0 / (1.2) * 1.095445115010332,                                     /* MINUS_1: 1.2^(-0.5) */
  1.0,                                                                 /* DEFAULT: 1.2^0 */

  /* PLUS_1 through PLUS_14: each step is 1.2^(1/2) ≈ 1.095445 */
  1.0 * 1.095445115010332,                                             /* PLUS_1: 1.2^0.5 */
  1.0 * 1.2,                                                           /* PLUS_2: 1.2^1 */
  1.0 * 1.2 * 1.095445115010332,                                       /* PLUS_3: 1.2^1.5 */
  1.0 * 1.2 * 1.2,                                                     /* PLUS_4: 1.2^2 */
  1.0 * 1.2 * 1.2 * 1.095445115010332,                                 /* PLUS_5: 1.2^2.5 */
  1.0 * 1.2 * 1.2 * 1.2,                                               /* PLUS_6: 1.2^3 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.095445115010332,                           /* PLUS_7: 1.2^3.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2,                                         /* PLUS_8: 1.2^4 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.095445115010332,                     /* PLUS_9: 1.2^4.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2,                                   /* PLUS_10: 1.2^5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.095445115010332,               /* PLUS_11: 1.2^5.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2,                             /* PLUS_12: 1.2^6 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.095445115010332,         /* PLUS_13: 1.2^6.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2,                       /* PLUS_14: 1.2^7 */
};

static void
ptyxis_terminal_surface_emit_notify_user (PtyxisTerminalSurface *self,
                                          const char            *title,
                                          const char            *button_label,
                                          const char            *action_name,
                                          GVariant              *action_target)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  g_signal_emit (self, signals[NOTIFY_USER], 0,
                 title, button_label, action_name, action_target);
}

static void
ptyxis_terminal_surface_send_signal (PtyxisTerminalSurface *self,
                                     int                    signum)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (self->process == NULL)
    return;

  ptyxis_ipc_process_call_send_signal (self->process, signum, NULL, NULL, NULL);
}

static void
ptyxis_terminal_surface_apply_zoom (PtyxisTerminalSurface *self)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  vte_terminal_set_font_scale (VTE_TERMINAL (self->terminal),
                               zoom_font_scales[self->zoom]);
}

static void
ptyxis_terminal_surface_update_scrollback_lines (PtyxisTerminalSurface *self)
{
  long scrollback_lines = -1;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (ptyxis_profile_get_limit_scrollback (self->profile))
    scrollback_lines = ptyxis_profile_get_scrollback_lines (self->profile);

  vte_terminal_set_scrollback_lines (VTE_TERMINAL (self->terminal), scrollback_lines);
}

static void
ptyxis_terminal_surface_update_cell_height_scale (PtyxisTerminalSurface *self)
{
  double cell_height_scale = 1.0;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (ptyxis_profile_get_cell_height_scale (self->profile))
    cell_height_scale = ptyxis_profile_get_cell_height_scale (self->profile);

  vte_terminal_set_cell_height_scale (VTE_TERMINAL (self->terminal), cell_height_scale);
}

static void
ptyxis_terminal_surface_update_cell_width_scale (PtyxisTerminalSurface *self)
{
  double cell_width_scale = 1.0;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (ptyxis_profile_get_cell_width_scale (self->profile))
    cell_width_scale = ptyxis_profile_get_cell_width_scale (self->profile);

  vte_terminal_set_cell_width_scale (VTE_TERMINAL (self->terminal), cell_width_scale);
}

static void
ptyxis_terminal_surface_update_custom_links (PtyxisTerminalSurface *self)
{
  g_autoptr(GListModel) custom_links_list = NULL;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  custom_links_list = ptyxis_profile_list_custom_links (self->profile);
  ptyxis_terminal_update_custom_links_list (self->terminal, custom_links_list);
}

static void
ptyxis_terminal_surface_update_scrollbar_policy (PtyxisTerminalSurface *self)
{
  PtyxisSettings *settings;
  PtyxisScrollbarPolicy policy;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  policy = ptyxis_settings_get_scrollbar_policy (settings);

  switch (policy)
    {
    case PTYXIS_SCROLLBAR_POLICY_NEVER:
      gtk_scrolled_window_set_overlay_scrolling (self->scrolled_window, FALSE);
      gtk_scrolled_window_set_policy (self->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
      break;

    case PTYXIS_SCROLLBAR_POLICY_ALWAYS:
      gtk_scrolled_window_set_overlay_scrolling (self->scrolled_window, FALSE);
      gtk_scrolled_window_set_policy (self->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
      break;

    case PTYXIS_SCROLLBAR_POLICY_SYSTEM:
      if (ptyxis_application_get_overlay_scrollbars (PTYXIS_APPLICATION_DEFAULT))
        {
          gtk_scrolled_window_set_overlay_scrolling (self->scrolled_window, TRUE);
          gtk_scrolled_window_set_policy (self->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        }
      else
        {
          gtk_scrolled_window_set_overlay_scrolling (self->scrolled_window, FALSE);
          gtk_scrolled_window_set_policy (self->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
ptyxis_terminal_surface_update_padding_cb (PtyxisTerminalSurface *self,
                                           GParamSpec            *pspec,
                                           PtyxisSettings        *settings)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (PTYXIS_IS_SETTINGS (settings));

  if (ptyxis_settings_get_disable_padding (settings))
    gtk_widget_remove_css_class (GTK_WIDGET (self->terminal), "padded");
  else
    gtk_widget_add_css_class (GTK_WIDGET (self->terminal), "padded");
}

static void
ptyxis_terminal_surface_update_word_char_exceptions (PtyxisTerminalSurface *self,
                                                     GParamSpec            *pspec,
                                                     PtyxisSettings        *settings)
{
  g_autofree char *word_char_exceptions = NULL;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (PTYXIS_IS_SETTINGS (settings));

  word_char_exceptions = ptyxis_settings_dup_word_char_exceptions (settings);
  vte_terminal_set_word_char_exceptions (VTE_TERMINAL (self->terminal), word_char_exceptions);
}

static PtyxisIpcContainer *
ptyxis_terminal_surface_discover_container (PtyxisTerminalSurface *self)
{
  const char *current_container_name;
  const char *current_container_runtime;

  current_container_name = ptyxis_terminal_get_current_container_name (self->terminal);
  current_container_runtime = ptyxis_terminal_get_current_container_runtime (self->terminal);

  return ptyxis_application_find_container_by_name (PTYXIS_APPLICATION_DEFAULT,
                                                    current_container_runtime,
                                                    current_container_name);
}

static void
ptyxis_terminal_surface_wait_cb (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  PtyxisApplication *app = (PtyxisApplication *)object;
  g_autoptr(PtyxisTerminalSurface) self = user_data;
  g_autoptr(GError) error = NULL;
  PtyxisExitAction exit_action;
  int exit_code;

  g_assert (PTYXIS_IS_APPLICATION (app));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (self->state == PTYXIS_TERMINAL_SURFACE_STATE_RUNNING);

  g_clear_object (&self->process);

  exit_code = ptyxis_application_wait_finish (app, result, &error);

  g_debug ("Process completed with exit-code 0x%x %s",
           exit_code,
           error ? error->message : "");

  if (error == NULL && WIFEXITED (exit_code) && WEXITSTATUS (exit_code) == 0)
    self->state = PTYXIS_TERMINAL_SURFACE_STATE_EXITED;
  else
    self->state = PTYXIS_TERMINAL_SURFACE_STATE_FAILED;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);

  if (self->forced_exit)
    return;

  if (WIFSIGNALED (exit_code))
    {
      g_autofree char *title = NULL;

      title = g_strdup_printf (_("Process Exited from Signal %d"), WTERMSIG (exit_code));

      ptyxis_terminal_surface_emit_notify_user (self,
                                                title,
                                                _("_Restart"),
                                                "surface.respawn",
                                                NULL);
      return;
    }

  exit_action = ptyxis_profile_get_exit_action (self->profile);

  if (self->command != NULL)
    exit_action = PTYXIS_EXIT_ACTION_CLOSE;

  if (self->keep_terminal_open)
    exit_action = PTYXIS_EXIT_ACTION_NONE;

  if ((self->command == NULL || self->state == PTYXIS_TERMINAL_SURFACE_STATE_FAILED) &&
      (g_get_monotonic_time () - self->respawn_time) < (G_USEC_PER_SEC / 2) &&
      !self->has_pressed_key)
    exit_action = PTYXIS_EXIT_ACTION_NONE;

  switch (exit_action)
    {
    case PTYXIS_EXIT_ACTION_RESTART:
      ptyxis_terminal_surface_do_respawn (self);
      break;

    case PTYXIS_EXIT_ACTION_CLOSE:
      g_signal_emit (self, signals[REQUEST_CLOSE], 0);
      break;

    case PTYXIS_EXIT_ACTION_NONE:
      ptyxis_terminal_surface_emit_notify_user (self,
                                                _("Process Exited"),
                                                _("_Restart"),
                                                "surface.respawn",
                                                NULL);
      break;

    default:
      g_assert_not_reached ();
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static void
ptyxis_terminal_surface_spawn_cb (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  PtyxisApplication *app = (PtyxisApplication *)object;
  g_autoptr(PtyxisIpcProcess) process = NULL;
  g_autoptr(PtyxisTerminalSurface) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (PTYXIS_IS_APPLICATION (app));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (self->state == PTYXIS_TERMINAL_SURFACE_STATE_SPAWNING);

  if (!(process = ptyxis_application_spawn_finish (app, result, &error)))
    {
      const char *profile_uuid = ptyxis_profile_get_uuid (self->profile);

      self->state = PTYXIS_TERMINAL_SURFACE_STATE_FAILED;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);

      vte_terminal_feed (VTE_TERMINAL (self->terminal), error->message, -1);
      vte_terminal_feed (VTE_TERMINAL (self->terminal), "\r\n", -1);

      ptyxis_terminal_surface_emit_notify_user (self,
                                                _("Failed to launch terminal"),
                                                _("Edit Profile"),
                                                "app.edit-profile",
                                                g_variant_new_string (profile_uuid));
      return;
    }

  self->state = PTYXIS_TERMINAL_SURFACE_STATE_RUNNING;
  self->respawn_time = g_get_monotonic_time ();
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);

  g_set_object (&self->process, process);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ICON]);

  ptyxis_application_wait_async (app,
                                 process,
                                 NULL,
                                 ptyxis_terminal_surface_wait_cb,
                                 g_object_ref (self));
}

static void
ptyxis_terminal_surface_do_respawn (PtyxisTerminalSurface *self)
{
  g_autofree char *default_container = NULL;
  g_autoptr(PtyxisIpcContainer) container = NULL;
  g_autoptr(VtePty) new_pty = NULL;
  PtyxisApplication *app;
  const char *profile_uuid;
  const char *cwd_uri;
  VtePty *pty;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (self->state == PTYXIS_TERMINAL_SURFACE_STATE_INITIAL ||
            self->state == PTYXIS_TERMINAL_SURFACE_STATE_EXITED ||
            self->state == PTYXIS_TERMINAL_SURFACE_STATE_FAILED);

  ptyxis_terminal_surface_emit_notify_user (self, NULL, NULL, NULL, NULL);

  app = PTYXIS_APPLICATION_DEFAULT;
  profile_uuid = ptyxis_profile_get_uuid (self->profile);
  default_container = ptyxis_profile_dup_default_container (self->profile);

  if (self->container_at_creation != NULL)
    container = g_object_ref (self->container_at_creation);
  else
    container = ptyxis_application_lookup_container (app, default_container);

  if (container == NULL)
    {
      g_autofree char *title = NULL;

      self->state = PTYXIS_TERMINAL_SURFACE_STATE_FAILED;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);

      title = g_strdup_printf (_("Cannot locate container “%s”"), default_container);
      ptyxis_terminal_surface_emit_notify_user (self,
                                                title,
                                                _("Edit Profile"),
                                                "app.edit-profile",
                                                g_variant_new_string (profile_uuid));
      return;
    }

  self->state = PTYXIS_TERMINAL_SURFACE_STATE_SPAWNING;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);

  pty = vte_terminal_get_pty (VTE_TERMINAL (self->terminal));

  if (pty == NULL)
    {
      g_autoptr(GError) error = NULL;

      new_pty = ptyxis_application_create_pty (PTYXIS_APPLICATION_DEFAULT, &error);

      if (new_pty == NULL)
        {
          self->state = PTYXIS_TERMINAL_SURFACE_STATE_FAILED;
          g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);

          ptyxis_terminal_surface_emit_notify_user (self,
                                                    _("Failed to create pseudo terminal device"),
                                                    NULL,
                                                    NULL,
                                                    NULL);
          return;
        }

      vte_terminal_set_pty (VTE_TERMINAL (self->terminal), new_pty);
      pty = new_pty;
    }

  cwd_uri = self->previous_working_directory_uri;
  if (self->initial_working_directory_uri)
    cwd_uri = self->initial_working_directory_uri;

  ptyxis_application_spawn_async (PTYXIS_APPLICATION_DEFAULT,
                                  container,
                                  self->profile,
                                  cwd_uri,
                                  pty,
                                  (const char * const *)self->command,
                                  (const char * const *)self->environment,
                                  NULL,
                                  ptyxis_terminal_surface_spawn_cb,
                                  g_object_ref (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static gboolean
on_scroll_scrolled_cb (GtkEventControllerScroll *scroll,
                       double                    dx,
                       double                    dy,
                       PtyxisTerminalSurface    *self)
{
  GdkModifierType mods;

  g_assert (GTK_IS_EVENT_CONTROLLER_SCROLL (scroll));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  mods = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (scroll));

  if ((mods & GDK_CONTROL_MASK) != 0)
    {
      PtyxisSettings *settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

      if (ptyxis_settings_get_enable_zoom_scroll_ctrl (settings))
        {
          if (dy < 0)
            ptyxis_terminal_surface_zoom_in (self);
          else if (dy > 0)
            ptyxis_terminal_surface_zoom_out (self);
        }

      return TRUE;
    }

  return FALSE;
}

static void
on_scroll_begin_cb (GtkEventControllerScroll *scroll,
                    PtyxisTerminalSurface    *self)
{
  GdkModifierType state;

  g_assert (GTK_IS_EVENT_CONTROLLER_SCROLL (scroll));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (scroll));

  if ((state & GDK_CONTROL_MASK) != 0)
    gtk_event_controller_scroll_set_flags (scroll,
                                           GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                           GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
}

static gboolean
ptyxis_terminal_surface_key_pressed_cb (PtyxisTerminalSurface *self,
                                        unsigned int           keyval,
                                        unsigned int           keycode,
                                        GdkModifierType        state,
                                        GtkEventControllerKey *controller)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  self->has_pressed_key = TRUE;

  return FALSE;
}

static void
on_scroll_end_cb (GtkEventControllerScroll *scroll,
                  PtyxisTerminalSurface    *self)
{
  g_assert (GTK_IS_EVENT_CONTROLLER_SCROLL (scroll));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  gtk_event_controller_scroll_set_flags (scroll, GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
}

static void
ptyxis_terminal_surface_notify_window_title_cb (PtyxisTerminalSurface *self,
                                                GParamSpec            *pspec,
                                                PtyxisTerminal        *terminal)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static void
ptyxis_terminal_surface_notify_window_subtitle_cb (PtyxisTerminalSurface *self,
                                                   PtyxisTerminal        *terminal)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUBTITLE]);
}

static void
ptyxis_terminal_surface_increase_font_size_cb (PtyxisTerminalSurface *self,
                                               PtyxisTerminal        *terminal)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  ptyxis_terminal_surface_zoom_in (self);
}

static void
ptyxis_terminal_surface_decrease_font_size_cb (PtyxisTerminalSurface *self,
                                               PtyxisTerminal        *terminal)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  ptyxis_terminal_surface_zoom_out (self);
}

static void
ptyxis_terminal_surface_bell_cb (PtyxisTerminalSurface *self,
                                 PtyxisTerminal        *terminal)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  g_signal_emit (self, signals[BELL], 0);
}

static void
ptyxis_terminal_surface_commit_cb (PtyxisTerminalSurface *self,
                                   const char            *str,
                                   unsigned int           length,
                                   PtyxisTerminal        *terminal)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  g_signal_emit (self, signals[COMMIT], 0, str);
}

static void
ptyxis_terminal_surface_invalidate_icon (PtyxisTerminalSurface *self)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ICON]);
}

static void
ptyxis_terminal_surface_invalidate_progress (PtyxisTerminalSurface *self)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROGRESS]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROGRESS_FRACTION]);
}

static gboolean
ptyxis_terminal_surface_match_clicked_cb (PtyxisTerminalSurface *self,
                                          double                 x,
                                          double                 y,
                                          int                    button,
                                          GdkModifierType        state,
                                          const char            *match,
                                          PtyxisTerminal        *terminal)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (match != NULL);

  if (!ptyxis_str_empty0 (match))
    {
      ptyxis_terminal_surface_open_uri (self, match);
      return TRUE;
    }

  return FALSE;
}

static void
ptyxis_terminal_surface_scrolled_window_map_cb (GtkWidget             *widget,
                                                PtyxisTerminalSurface *self)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (self->state == PTYXIS_TERMINAL_SURFACE_STATE_INITIAL)
    ptyxis_terminal_surface_do_respawn (self);
}

static void
ptyxis_terminal_surface_profile_signals_bind_cb (PtyxisTerminalSurface *self,
                                                 PtyxisProfile         *profile,
                                                 GSignalGroup          *group)
{
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_assert (PTYXIS_IS_PROFILE (profile));
  g_assert (G_IS_SIGNAL_GROUP (group));

  ptyxis_terminal_surface_update_scrollback_lines (self);
  ptyxis_terminal_surface_update_cell_height_scale (self);
  ptyxis_terminal_surface_update_cell_width_scale (self);
  ptyxis_terminal_surface_update_custom_links (self);
}

static char *
ptyxis_terminal_surface_collapse_uri (const char *uri)
{
  g_autoptr(GFile) file = NULL;

  if (uri == NULL)
    return NULL;

  file = g_file_new_for_uri (uri);
  if (file == NULL)
    return NULL;

  if (g_file_is_native (file))
    return ptyxis_path_collapse (g_file_peek_path (file));

  return strdup (uri);
}

static void
ptyxis_terminal_surface_constructed (GObject *object)
{
  PtyxisTerminalSurface *self = (PtyxisTerminalSurface *)object;
  PtyxisSettings *settings;
  GtkEventController *controller;

  G_OBJECT_CLASS (ptyxis_terminal_surface_parent_class)->constructed (object);

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  /* Create terminal */
  self->terminal = g_object_new (PTYXIS_TYPE_TERMINAL,
                                 "enable-fallback-scrolling", FALSE,
                                 "scroll-unit-is-pixels", TRUE,
                                 NULL);
  g_object_ref_sink (self->terminal);
  ptyxis_terminal_set_surface (self->terminal, self);

  /* Create scrolled window containing terminal */
  self->scrolled_window = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_object_ref_sink (self->scrolled_window);
  gtk_scrolled_window_set_propagate_natural_width (self->scrolled_window, TRUE);
  gtk_scrolled_window_set_propagate_natural_height (self->scrolled_window, TRUE);
  gtk_scrolled_window_set_policy (self->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scrolled_window), TRUE);
  gtk_scrolled_window_set_child (self->scrolled_window, GTK_WIDGET (self->terminal));

  /* Connect terminal signals */
  g_signal_connect_object (self->terminal, "commit",
                           G_CALLBACK (ptyxis_terminal_surface_commit_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "notify::window-title",
                           G_CALLBACK (ptyxis_terminal_surface_notify_window_title_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "current-file-uri-changed",
                           G_CALLBACK (ptyxis_terminal_surface_notify_window_subtitle_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "current-directory-uri-changed",
                           G_CALLBACK (ptyxis_terminal_surface_notify_window_subtitle_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "decrease-font-size",
                           G_CALLBACK (ptyxis_terminal_surface_decrease_font_size_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "increase-font-size",
                           G_CALLBACK (ptyxis_terminal_surface_increase_font_size_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "bell",
                           G_CALLBACK (ptyxis_terminal_surface_bell_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "termprop-changed::vte.container.name",
                           G_CALLBACK (ptyxis_terminal_surface_invalidate_icon),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "termprop-changed::vte.container.runtime",
                           G_CALLBACK (ptyxis_terminal_surface_invalidate_icon),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "termprop-changed::vte.progress.hint",
                           G_CALLBACK (ptyxis_terminal_surface_invalidate_progress),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "termprop-changed::vte.progress.value",
                           G_CALLBACK (ptyxis_terminal_surface_invalidate_progress),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->terminal, "match-clicked",
                           G_CALLBACK (ptyxis_terminal_surface_match_clicked_cb),
                           self, G_CONNECT_SWAPPED);

  /* Bind settings to terminal */
  g_object_bind_property (settings, "audible-bell",
                          self->terminal, "audible-bell",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "cursor-shape",
                          self->terminal, "cursor-shape",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "cursor-blink-mode",
                          self->terminal, "cursor-blink-mode",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "enable-a11y",
                          self->terminal, "enable-a11y",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "font-desc",
                          self->terminal, "font-desc",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "text-blink-mode",
                          self->terminal, "text-blink-mode",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "ignore-osc-title",
                          self, "ignore-osc-title",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "disable-dynamic-title",
                          self, "disable-dynamic-title",
                          G_BINDING_SYNC_CREATE);

  /* Bind profile properties to terminal via GBindingGroup */
  self->profile_bindings = g_binding_group_new ();
  g_binding_group_bind (self->profile_bindings, "palette",
                        self->terminal, "palette",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->profile_bindings, "scroll-on-keystroke",
                        self->terminal, "scroll-on-keystroke",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->profile_bindings, "scroll-on-output",
                        self->terminal, "scroll-on-output",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->profile_bindings, "backspace-binding",
                        self->terminal, "backspace-binding",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->profile_bindings, "delete-binding",
                        self->terminal, "delete-binding",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->profile_bindings, "cjk-ambiguous-width",
                        self->terminal, "cjk-ambiguous-width",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->profile_bindings, "bold-is-bright",
                        self->terminal, "bold-is-bright",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_set_source (self->profile_bindings, self->profile);

  /* Padding */
  g_signal_connect_object (settings,
                           "notify::disable-padding",
                           G_CALLBACK (ptyxis_terminal_surface_update_padding_cb),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_terminal_surface_update_padding_cb (self, NULL, settings);

  /* Scrollbar policy */
  g_signal_connect_object (PTYXIS_APPLICATION_DEFAULT,
                           "notify::overlay-scrollbars",
                           G_CALLBACK (ptyxis_terminal_surface_update_scrollbar_policy),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (settings,
                           "notify::scrollbar-policy",
                           G_CALLBACK (ptyxis_terminal_surface_update_scrollbar_policy),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_terminal_surface_update_scrollbar_policy (self);

  /* Profile signal group */
  self->profile_signals = g_signal_group_new (PTYXIS_TYPE_PROFILE);
  g_signal_connect_object (self->profile_signals,
                           "bind",
                           G_CALLBACK (ptyxis_terminal_surface_profile_signals_bind_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::limit-scrollback",
                                 G_CALLBACK (ptyxis_terminal_surface_update_scrollback_lines),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::scrollback-lines",
                                 G_CALLBACK (ptyxis_terminal_surface_update_scrollback_lines),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::cell-height-scale",
                                 G_CALLBACK (ptyxis_terminal_surface_update_cell_height_scale),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::cell-width-scale",
                                 G_CALLBACK (ptyxis_terminal_surface_update_cell_width_scale),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "custom-links-changed",
                                 G_CALLBACK (ptyxis_terminal_surface_update_custom_links),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_set_target (self->profile_signals, self->profile);

  /* Word char exceptions */
  g_signal_connect_object (settings,
                           "notify::word-char-exceptions",
                           G_CALLBACK (ptyxis_terminal_surface_update_word_char_exceptions),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_terminal_surface_update_word_char_exceptions (self, NULL, settings);

  /* Scroll event controller for ctrl+scroll zoom */
  controller = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "scroll",
                    G_CALLBACK (on_scroll_scrolled_cb), self);
  g_signal_connect (controller, "scroll-begin",
                    G_CALLBACK (on_scroll_begin_cb), self);
  g_signal_connect (controller, "scroll-end",
                    G_CALLBACK (on_scroll_end_cb), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled_window), controller);

  /* Key press tracking for fast-exit detection */
  controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect_object (controller, "key-pressed",
                           G_CALLBACK (ptyxis_terminal_surface_key_pressed_cb),
                           self, G_CONNECT_SWAPPED);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled_window), controller);

  /* Spawn on map */
  g_signal_connect (self->scrolled_window, "map",
                    G_CALLBACK (ptyxis_terminal_surface_scrolled_window_map_cb),
                    self);
}

static void
ptyxis_terminal_surface_dispose (GObject *object)
{
  PtyxisTerminalSurface *self = (PtyxisTerminalSurface *)object;

  ptyxis_terminal_surface_force_quit (self);

  g_clear_object (&self->profile_bindings);
  g_clear_object (&self->profile_signals);
  g_clear_object (&self->profile);
  g_clear_object (&self->process);
  g_clear_object (&self->container_at_creation);

  if (self->scrolled_window != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->scrolled_window, self);
      g_clear_object (&self->scrolled_window);
    }

  g_clear_object (&self->terminal);

  g_clear_pointer (&self->initial_working_directory_uri, g_free);
  g_clear_pointer (&self->previous_working_directory_uri, g_free);
  g_clear_pointer (&self->initial_title, g_free);
  g_clear_pointer (&self->title_prefix, g_free);
  g_clear_pointer (&self->command, g_strfreev);
  g_clear_pointer (&self->environment, g_strfreev);
  g_clear_pointer (&self->command_line, g_free);
  g_clear_pointer (&self->program_name, g_free);

  G_OBJECT_CLASS (ptyxis_terminal_surface_parent_class)->dispose (object);
}

static void
ptyxis_terminal_surface_get_property (GObject    *object,
                                      unsigned int prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  PtyxisTerminalSurface *self = PTYXIS_TERMINAL_SURFACE (object);

  switch (prop_id)
    {
    case PROP_COMMAND_LINE:
      g_value_set_string (value, self->command_line);
      break;

    case PROP_DISABLE_DYNAMIC_TITLE:
      g_value_set_boolean (value, self->disable_dynamic_title);
      break;

    case PROP_ICON:
      g_value_take_object (value, ptyxis_terminal_surface_dup_icon (self));
      break;

    case PROP_IGNORE_OSC_TITLE:
      g_value_set_boolean (value, self->ignore_osc_title);
      break;

    case PROP_INITIAL_TITLE:
      g_value_set_string (value, self->initial_title);
      break;

    case PROP_PROCESS_LEADER_KIND:
      g_value_set_enum (value, self->leader_kind);
      break;

    case PROP_PROFILE:
      g_value_set_object (value, self->profile);
      break;

    case PROP_PROGRESS:
      g_value_set_enum (value, ptyxis_terminal_surface_get_progress (self));
      break;

    case PROP_PROGRESS_FRACTION:
      g_value_set_double (value, ptyxis_terminal_surface_get_progress_fraction (self));
      break;

    case PROP_STATE:
      g_value_set_int (value, self->state);
      break;

    case PROP_SUBTITLE:
      g_value_take_string (value, ptyxis_terminal_surface_dup_subtitle (self));
      break;

    case PROP_TERMINAL:
      g_value_set_object (value, self->terminal);
      break;

    case PROP_TITLE:
      g_value_take_string (value, ptyxis_terminal_surface_dup_title (self));
      break;

    case PROP_TITLE_PREFIX:
      g_value_set_string (value, ptyxis_terminal_surface_get_title_prefix (self));
      break;

    case PROP_ZOOM:
      g_value_set_enum (value, self->zoom);
      break;

    case PROP_ZOOM_LABEL:
      g_value_take_string (value, ptyxis_terminal_surface_dup_zoom_label (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_terminal_surface_set_property (GObject      *object,
                                      unsigned int  prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  PtyxisTerminalSurface *self = PTYXIS_TERMINAL_SURFACE (object);

  switch (prop_id)
    {
    case PROP_DISABLE_DYNAMIC_TITLE:
      if (self->disable_dynamic_title != !!g_value_get_boolean (value))
        {
          self->disable_dynamic_title = !!g_value_get_boolean (value);
          g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DISABLE_DYNAMIC_TITLE]);
          g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
        }
      break;

    case PROP_IGNORE_OSC_TITLE:
      ptyxis_terminal_surface_set_ignore_osc_title (self, g_value_get_boolean (value));
      break;

    case PROP_INITIAL_TITLE:
      ptyxis_terminal_surface_set_initial_title (self, g_value_get_string (value));
      break;

    case PROP_PROFILE:
      self->profile = g_value_dup_object (value);
      break;

    case PROP_TITLE_PREFIX:
      ptyxis_terminal_surface_set_title_prefix (self, g_value_get_string (value));
      break;

    case PROP_ZOOM:
      ptyxis_terminal_surface_set_zoom (self, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_terminal_surface_class_init (PtyxisTerminalSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ptyxis_terminal_surface_constructed;
  object_class->dispose = ptyxis_terminal_surface_dispose;
  object_class->get_property = ptyxis_terminal_surface_get_property;
  object_class->set_property = ptyxis_terminal_surface_set_property;

  properties[PROP_COMMAND_LINE] =
    g_param_spec_string ("command-line", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_DISABLE_DYNAMIC_TITLE] =
    g_param_spec_boolean ("disable-dynamic-title", NULL, NULL,
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_EXPLICIT_NOTIFY |
                           G_PARAM_STATIC_STRINGS));

  properties[PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL,
                         G_TYPE_ICON,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_IGNORE_OSC_TITLE] =
    g_param_spec_boolean ("ignore-osc-title", NULL, NULL,
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_EXPLICIT_NOTIFY |
                           G_PARAM_STATIC_STRINGS));

  properties[PROP_INITIAL_TITLE] =
    g_param_spec_string ("initial-title", NULL, NULL,
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_PROCESS_LEADER_KIND] =
    g_param_spec_enum ("process-leader-kind", NULL, NULL,
                       PTYXIS_TYPE_PROCESS_LEADER_KIND,
                       PTYXIS_PROCESS_LEADER_KIND_UNKNOWN,
                       (G_PARAM_READABLE |
                        G_PARAM_STATIC_STRINGS));

  properties[PROP_PROFILE] =
    g_param_spec_object ("profile", NULL, NULL,
                         PTYXIS_TYPE_PROFILE,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_PROGRESS] =
    g_param_spec_enum ("progress", NULL, NULL,
                       PTYXIS_TYPE_TAB_PROGRESS,
                       PTYXIS_TAB_PROGRESS_INDETERMINATE,
                       (G_PARAM_READABLE |
                        G_PARAM_STATIC_STRINGS));

  properties[PROP_PROGRESS_FRACTION] =
    g_param_spec_double ("progress-fraction", NULL, NULL,
                         0, 1, 0,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_STATE] =
    g_param_spec_int ("state", NULL, NULL,
                      PTYXIS_TERMINAL_SURFACE_STATE_INITIAL,
                      PTYXIS_TERMINAL_SURFACE_STATE_FAILED,
                      PTYXIS_TERMINAL_SURFACE_STATE_INITIAL,
                      (G_PARAM_READABLE |
                       G_PARAM_STATIC_STRINGS));

  properties[PROP_SUBTITLE] =
    g_param_spec_string ("subtitle", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_TERMINAL] =
    g_param_spec_object ("terminal", NULL, NULL,
                         PTYXIS_TYPE_TERMINAL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_TITLE_PREFIX] =
    g_param_spec_string ("title-prefix", NULL, NULL,
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_ZOOM] =
    g_param_spec_enum ("zoom", NULL, NULL,
                       PTYXIS_TYPE_ZOOM_LEVEL,
                       PTYXIS_ZOOM_LEVEL_DEFAULT,
                       (G_PARAM_READWRITE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS));

  properties[PROP_ZOOM_LABEL] =
    g_param_spec_string ("zoom-label", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[BELL] =
    g_signal_new_class_handler ("bell",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                NULL,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 0);

  signals[COMMIT] =
    g_signal_new_class_handler ("commit",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                NULL,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE,
                                1,
                                G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);

  signals[NOTIFY_USER] =
    g_signal_new_class_handler ("notify-user",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                NULL,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE,
                                4,
                                G_TYPE_STRING,
                                G_TYPE_STRING,
                                G_TYPE_STRING,
                                G_TYPE_VARIANT);

  signals[REQUEST_CLOSE] =
    g_signal_new_class_handler ("request-close",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                NULL,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 0);

  g_type_ensure (PTYXIS_TYPE_TERMINAL);
}

static void
ptyxis_terminal_surface_init (PtyxisTerminalSurface *self)
{
  self->state = PTYXIS_TERMINAL_SURFACE_STATE_INITIAL;
  self->zoom = PTYXIS_ZOOM_LEVEL_DEFAULT;
}

PtyxisTerminalSurface *
ptyxis_terminal_surface_new (PtyxisProfile *profile)
{
  g_return_val_if_fail (PTYXIS_IS_PROFILE (profile), NULL);

  return g_object_new (PTYXIS_TYPE_TERMINAL_SURFACE,
                       "profile", profile,
                       NULL);
}

PtyxisTerminal *
ptyxis_terminal_surface_get_terminal (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return self->terminal;
}

GtkScrolledWindow *
ptyxis_terminal_surface_get_scrolled_window (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return self->scrolled_window;
}

PtyxisProfile *
ptyxis_terminal_surface_get_profile (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return self->profile;
}

void
ptyxis_terminal_surface_apply_profile (PtyxisTerminalSurface *self,
                                       PtyxisProfile         *new_profile)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_return_if_fail (PTYXIS_IS_PROFILE (new_profile));

  if (self->profile == new_profile)
    return;

  g_clear_object (&self->profile);
  self->profile = g_object_ref (new_profile);

  g_binding_group_set_source (self->profile_bindings, self->profile);
  g_signal_group_set_target (self->profile_signals, self->profile);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROFILE]);
}

PtyxisZoomLevel
ptyxis_terminal_surface_get_zoom (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), 0);

  return self->zoom;
}

void
ptyxis_terminal_surface_set_zoom (PtyxisTerminalSurface *self,
                                  PtyxisZoomLevel        zoom)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_return_if_fail (zoom >= PTYXIS_ZOOM_LEVEL_MINUS_14 &&
                    zoom <= PTYXIS_ZOOM_LEVEL_PLUS_14);

  if (zoom != self->zoom)
    {
      self->zoom = zoom;
      ptyxis_terminal_surface_apply_zoom (self);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM_LABEL]);
    }
}

void
ptyxis_terminal_surface_zoom_in (PtyxisTerminalSurface *self)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (self->zoom < PTYXIS_ZOOM_LEVEL_PLUS_14)
    ptyxis_terminal_surface_set_zoom (self, self->zoom + 1);
}

void
ptyxis_terminal_surface_zoom_out (PtyxisTerminalSurface *self)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (self->zoom > PTYXIS_ZOOM_LEVEL_MINUS_14)
    ptyxis_terminal_surface_set_zoom (self, self->zoom - 1);
}

char *
ptyxis_terminal_surface_dup_zoom_label (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  if (self->zoom == PTYXIS_ZOOM_LEVEL_DEFAULT)
    return g_strdup ("100%");

  return g_strdup_printf ("%.0lf%%", zoom_font_scales[self->zoom] * 100.0);
}

PtyxisTerminalSurfaceState
ptyxis_terminal_surface_get_state (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), 0);

  return self->state;
}

typedef struct _PollAgentSync
{
  GMainContext *context;
  gboolean completed;
  gboolean success;
} PollAgentSync;

static void
ptyxis_terminal_surface_poll_agent_sync_cb (GObject      *object,
                                            GAsyncResult *result,
                                            gpointer      user_data)
{
  PtyxisTerminalSurface *self = (PtyxisTerminalSurface *)object;
  PollAgentSync *sync_data = user_data;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  sync_data->completed = TRUE;
  sync_data->success = ptyxis_terminal_surface_poll_agent_finish (self, result, NULL);
  g_main_context_wakeup (sync_data->context);
}

static gboolean
ptyxis_terminal_surface_poll_agent_sync_impl (PtyxisTerminalSurface *self)
{
  PollAgentSync sync_data;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), FALSE);

  sync_data.context = g_main_context_get_thread_default ();
  sync_data.completed = FALSE;
  sync_data.success = FALSE;

  ptyxis_terminal_surface_poll_agent_async (self, NULL,
                                            ptyxis_terminal_surface_poll_agent_sync_cb,
                                            &sync_data);

  while (!sync_data.completed)
    g_main_context_iteration (sync_data.context, TRUE);

  return sync_data.success;
}

gboolean
ptyxis_terminal_surface_is_running (PtyxisTerminalSurface  *self,
                                    char                  **cmdline)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), FALSE);

  ptyxis_terminal_surface_poll_agent_sync_impl (self);

  if (cmdline != NULL)
    *cmdline = g_strdup (self->command_line);

  if (self->has_foreground_process && self->program_name != NULL)
    return !ptyxis_is_shell (self->program_name);

  return FALSE;
}

gboolean
ptyxis_terminal_surface_has_foreground_process (PtyxisTerminalSurface  *self,
                                                GPid                   *pid,
                                                char                  **cmdline)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), FALSE);

  ptyxis_terminal_surface_poll_agent_sync_impl (self);

  if (pid != NULL)
    *pid = self->pid;

  if (cmdline != NULL)
    *cmdline = g_strdup (self->command_line);

  return self->has_foreground_process;
}

static gboolean
ptyxis_terminal_surface_force_quit_in_idle (gpointer data)
{
  PtyxisTerminalSurface *self = data;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (self->process != NULL)
    ptyxis_terminal_surface_send_signal (self, SIGKILL);

  return G_SOURCE_REMOVE;
}

void
ptyxis_terminal_surface_force_quit (PtyxisTerminalSurface *self)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  self->forced_exit = TRUE;

  if (self->process == NULL)
    return;

  ptyxis_terminal_surface_send_signal (self, SIGHUP);

  g_timeout_add_full (G_PRIORITY_HIGH,
                      50,
                      ptyxis_terminal_surface_force_quit_in_idle,
                      g_object_ref (self),
                      g_object_unref);
}

PtyxisIpcProcess *
ptyxis_terminal_surface_get_process (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return self->process;
}

const char *
ptyxis_terminal_surface_get_command_line (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return self->command_line;
}

void
ptyxis_terminal_surface_respawn (PtyxisTerminalSurface *self)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (self->state == PTYXIS_TERMINAL_SURFACE_STATE_FAILED ||
      self->state == PTYXIS_TERMINAL_SURFACE_STATE_EXITED)
    ptyxis_terminal_surface_do_respawn (self);
}

PtyxisIpcContainer *
ptyxis_terminal_surface_dup_container (PtyxisTerminalSurface *self)
{
  g_autoptr(PtyxisIpcContainer) container = NULL;
  const char *runtime;
  const char *name;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  if ((runtime = ptyxis_terminal_get_current_container_runtime (self->terminal)) &&
      (name = ptyxis_terminal_get_current_container_name (self->terminal)))
    container = ptyxis_application_find_container_by_name (PTYXIS_APPLICATION_DEFAULT, runtime, name);

  if (container == NULL)
    g_set_object (&container, self->container_at_creation);

  return g_steal_pointer (&container);
}

void
ptyxis_terminal_surface_set_container (PtyxisTerminalSurface *self,
                                       PtyxisIpcContainer    *container)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_return_if_fail (!container || PTYXIS_IPC_IS_CONTAINER (container));

  g_set_object (&self->container_at_creation, container);
}

char *
ptyxis_terminal_surface_dup_current_directory_uri (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return ptyxis_terminal_dup_current_directory_uri (self->terminal);
}

void
ptyxis_terminal_surface_set_initial_working_directory_uri (PtyxisTerminalSurface *self,
                                                           const char            *uri)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  g_set_str (&self->initial_working_directory_uri, uri);
}

char *
ptyxis_terminal_surface_dup_previous_working_directory_uri (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return g_strdup (self->previous_working_directory_uri);
}

void
ptyxis_terminal_surface_set_previous_working_directory_uri (PtyxisTerminalSurface *self,
                                                            const char            *uri)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  g_set_str (&self->previous_working_directory_uri, uri);
}

void
ptyxis_terminal_surface_set_command (PtyxisTerminalSurface  *self,
                                     const char * const     *command)
{
  char **copy;

  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (command != NULL && command[0] == NULL)
    command = NULL;

  copy = g_strdupv ((char **)command);
  g_strfreev (self->command);
  self->command = copy;
}

void
ptyxis_terminal_surface_set_environment (PtyxisTerminalSurface  *self,
                                         const char * const     *environment)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (environment != NULL && environment[0] == NULL)
    environment = NULL;

  g_set_strv (&self->environment, environment);
}

char *
ptyxis_terminal_surface_dup_title (PtyxisTerminalSurface *self)
{
  GString *gstr;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  gstr = g_string_new (NULL);

  if (self->title_prefix != NULL && self->title_prefix[0] != '\0')
    g_string_append (gstr, self->title_prefix);

  if (self->initial_title != NULL && self->initial_title[0] != '\0')
    g_string_append (gstr, self->initial_title);

  if (!self->ignore_osc_title)
    {
      const char *window_title;

      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        window_title = vte_terminal_get_window_title (VTE_TERMINAL (self->terminal));
      G_GNUC_END_IGNORE_DEPRECATIONS

      if (window_title && window_title[0])
        g_string_append (gstr, window_title);
      else if (self->command != NULL && self->command[0] != NULL)
        g_string_append (gstr, self->command[0]);
    }

  if (gstr->len == 0)
    g_string_append (gstr, _("Terminal"));

  if (self->state == PTYXIS_TERMINAL_SURFACE_STATE_EXITED)
    g_string_append_printf (gstr, " (%s)", _("Exited"));
  else if (self->state == PTYXIS_TERMINAL_SURFACE_STATE_FAILED)
    g_string_append_printf (gstr, " (%s)", _("Failed"));
  else if (!self->disable_dynamic_title &&
           self->has_foreground_process &&
           !ptyxis_str_empty0 (self->command_line) &&
           !ptyxis_str_empty0 (self->program_name) &&
           !ptyxis_is_shell (self->program_name))
    g_string_append_printf (gstr, " — %s", self->command_line);

  return g_string_free (gstr, FALSE);
}

char *
ptyxis_terminal_surface_dup_subtitle (PtyxisTerminalSurface *self)
{
  g_autofree char *current_directory_uri = NULL;
  g_autofree char *current_file_uri = NULL;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  current_file_uri = ptyxis_terminal_dup_current_file_uri (self->terminal);
  if (current_file_uri != NULL && current_file_uri[0] != 0)
    return ptyxis_terminal_surface_collapse_uri (current_file_uri);

  current_directory_uri = ptyxis_terminal_dup_current_directory_uri (self->terminal);
  if (current_directory_uri != NULL && current_directory_uri[0] != 0)
    return ptyxis_terminal_surface_collapse_uri (current_directory_uri);

  return g_strdup ("");
}

GIcon *
ptyxis_terminal_surface_dup_icon (PtyxisTerminalSurface *self)
{
  PtyxisProcessLeaderKind kind;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  kind = self->leader_kind;

  switch (kind)
    {
    default:
    case PTYXIS_PROCESS_LEADER_KIND_REMOTE:
      return g_themed_icon_new ("process-remote-symbolic");

    case PTYXIS_PROCESS_LEADER_KIND_SUPERUSER:
      return g_themed_icon_new ("process-superuser-symbolic");

    case PTYXIS_PROCESS_LEADER_KIND_CONTAINER:
    case PTYXIS_PROCESS_LEADER_KIND_UNKNOWN:
      {
        g_autoptr(PtyxisIpcContainer) container = NULL;
        const char *icon_name;

        container = ptyxis_terminal_surface_discover_container (self);

        if (container == NULL)
          {
            if (!g_set_object (&container, self->container_at_creation))
              {
                if (self->profile != NULL)
                  {
                    g_autofree char *profile_uuid = ptyxis_profile_dup_default_container (self->profile);

                    container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, profile_uuid);
                  }
              }
          }

        if (container != NULL &&
            (icon_name = ptyxis_ipc_container_get_icon_name (container)) &&
            icon_name[0] != 0)
          return g_themed_icon_new (icon_name);
      }
      return NULL;
    }
}

PtyxisProcessLeaderKind
ptyxis_terminal_surface_get_process_leader_kind (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), PTYXIS_PROCESS_LEADER_KIND_UNKNOWN);

  return self->leader_kind;
}

const char *
ptyxis_terminal_surface_get_initial_title (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return self->initial_title;
}

void
ptyxis_terminal_surface_set_initial_title (PtyxisTerminalSurface *self,
                                           const char            *initial_title)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (g_set_str (&self->initial_title, initial_title))
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_INITIAL_TITLE]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
}

const char *
ptyxis_terminal_surface_get_title_prefix (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  return self->title_prefix ? self->title_prefix : "";
}

void
ptyxis_terminal_surface_set_title_prefix (PtyxisTerminalSurface *self,
                                          const char            *title_prefix)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (ptyxis_str_empty0 (title_prefix))
    title_prefix = NULL;

  if (g_set_str (&self->title_prefix, title_prefix))
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE_PREFIX]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
}

gboolean
ptyxis_terminal_surface_get_ignore_osc_title (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), FALSE);

  return self->ignore_osc_title;
}

void
ptyxis_terminal_surface_set_ignore_osc_title (PtyxisTerminalSurface *self,
                                              gboolean               ignore_osc_title)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  ignore_osc_title = !!ignore_osc_title;

  if (ignore_osc_title != self->ignore_osc_title)
    {
      self->ignore_osc_title = ignore_osc_title;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_IGNORE_OSC_TITLE]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
}

PtyxisTabProgress
ptyxis_terminal_surface_get_progress (PtyxisTerminalSurface *self)
{
  int64_t state;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), 0);

  if (vte_terminal_get_termprop_int_by_id (VTE_TERMINAL (self->terminal),
                                           VTE_PROPERTY_ID_PROGRESS_HINT,
                                           &state))
    {
      switch (state)
        {
        case VTE_PROGRESS_HINT_ACTIVE:
          return PTYXIS_TAB_PROGRESS_ACTIVE;

        case VTE_PROGRESS_HINT_ERROR:
          return PTYXIS_TAB_PROGRESS_ERROR;

        case VTE_PROGRESS_HINT_PAUSED:
        case VTE_PROGRESS_HINT_INDETERMINATE:
        default:
          return PTYXIS_TAB_PROGRESS_INDETERMINATE;
        }
    }

  return PTYXIS_TAB_PROGRESS_INDETERMINATE;
}

double
ptyxis_terminal_surface_get_progress_fraction (PtyxisTerminalSurface *self)
{
  uint64_t value;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), .0);

  if (ptyxis_terminal_surface_get_progress (self) != PTYXIS_TAB_PROGRESS_ACTIVE ||
      !vte_terminal_get_termprop_uint_by_id (VTE_TERMINAL (self->terminal),
                                             VTE_PROPERTY_ID_PROGRESS_VALUE,
                                             &value))
    return .0;

  return MIN (value, 100) / 100.0;
}

static void
ptyxis_terminal_surface_poll_agent_cb (GObject      *object,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
  PtyxisIpcProcess *process = (PtyxisIpcProcess *)object;
  g_autoptr(GTask) task = user_data;
  g_autofree char *the_cmdline = NULL;
  g_autofree char *the_leader_kind = NULL;
  PtyxisProcessLeaderKind leader_kind;
  gboolean has_foreground_process;
  gboolean changed = FALSE;
  PtyxisTerminalSurface *self;
  GPid the_pid;

  g_assert (PTYXIS_IPC_IS_PROCESS (process));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  ptyxis_ipc_process_call_has_foreground_process_finish (process,
                                                         &has_foreground_process,
                                                         &the_pid,
                                                         &the_cmdline,
                                                         &the_leader_kind,
                                                         NULL,
                                                         result,
                                                         NULL);

  if (self->pid != the_pid)
    {
      changed = TRUE;
      self->pid = the_pid;
    }

  if (self->has_foreground_process != has_foreground_process)
    {
      changed = TRUE;
      self->has_foreground_process = has_foreground_process;
    }

  if (g_strcmp0 (the_leader_kind, "superuser") == 0)
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_SUPERUSER;
  else if (g_strcmp0 (the_leader_kind, "container") == 0)
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_CONTAINER;
  else if (g_strcmp0 (the_leader_kind, "remote") == 0)
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_REMOTE;
  else
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_UNKNOWN;

  if (self->leader_kind != leader_kind)
    {
      changed = TRUE;
      self->leader_kind = leader_kind;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROCESS_LEADER_KIND]);
    }

  if (g_set_str (&self->command_line, the_cmdline))
    {
      g_autofree char *program_name = NULL;
      const char *space;

      changed = TRUE;

      if (the_cmdline != NULL && (space = strchr (the_cmdline, ' ')))
        program_name = g_strndup (the_cmdline, space - the_cmdline);

      g_set_str (&self->program_name, program_name);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMMAND_LINE]);
    }

  if (changed)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);

  g_task_return_boolean (task, changed);
}

void
ptyxis_terminal_surface_poll_agent_async (PtyxisTerminalSurface *self,
                                          GCancellable          *cancellable,
                                          GAsyncReadyCallback    callback,
                                          gpointer               user_data)
{
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GTask) task = NULL;
  VtePty *pty;
  int handle;
  int pty_fd;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ptyxis_terminal_surface_poll_agent_async);

  if (self->process == NULL)
    {
      self->has_foreground_process = FALSE;
      self->pid = -1;

      if (g_set_str (&self->command_line, NULL))
        g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMMAND_LINE]);

      if (self->leader_kind != PTYXIS_PROCESS_LEADER_KIND_UNKNOWN)
        {
          self->leader_kind = PTYXIS_PROCESS_LEADER_KIND_UNKNOWN;
          g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROCESS_LEADER_KIND]);
        }

      g_task_return_boolean (task, FALSE);
      return;
    }

  pty = vte_terminal_get_pty (VTE_TERMINAL (self->terminal));
  pty_fd = vte_pty_get_fd (pty);
  fd_list = g_unix_fd_list_new ();
  handle = g_unix_fd_list_append (fd_list, pty_fd, NULL);

  ptyxis_ipc_process_call_has_foreground_process (self->process,
                                                  g_variant_new_handle (handle),
                                                  fd_list,
                                                  cancellable,
                                                  ptyxis_terminal_surface_poll_agent_cb,
                                                  g_steal_pointer (&task));
}

gboolean
ptyxis_terminal_surface_poll_agent_finish (PtyxisTerminalSurface  *self,
                                           GAsyncResult           *result,
                                           GError                **error)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
ptyxis_terminal_surface_grab_focus (PtyxisTerminalSurface *self)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  gtk_widget_grab_focus (GTK_WIDGET (self->terminal));
}

#ifdef __linux__
static void
ptyxis_terminal_surface_toast (PtyxisTerminalSurface *self,
                               int                    timeout,
                               const char            *title)
{
  GtkWidget *overlay;
  AdwToast *toast;

  overlay = gtk_widget_get_ancestor (GTK_WIDGET (self->scrolled_window), ADW_TYPE_TOAST_OVERLAY);
  if (overlay == NULL)
    return;

  toast = g_object_new (ADW_TYPE_TOAST,
                        "title", title,
                        "timeout", timeout,
                        NULL);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), toast);
}

static void
ptyxis_terminal_surface_open_uri_cb (GObject      *object,
                                     GAsyncResult *result,
                                     gpointer      user_data)
{
  g_autoptr(PtyxisTerminalSurface) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (XDP_IS_PORTAL (object));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (self));

  if (!xdp_portal_open_uri_finish (XDP_PORTAL (object), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    ptyxis_terminal_surface_toast (self, 3, _("Failed to open link"));
}

void
ptyxis_terminal_surface_open_uri (PtyxisTerminalSurface *self,
                                  const char            *uri)
{
  g_autofree char *translated = NULL;
  GtkWindow *window;
  XdpParent *parent;

  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_return_if_fail (uri != NULL);

  window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self->scrolled_window)));

  if (g_str_has_prefix (uri, "file://"))
    {
      g_autoptr(PtyxisIpcContainer) container = ptyxis_terminal_surface_dup_container (self);
      g_autoptr(GUri) guri = NULL;

      if (container == NULL)
        {
          g_autofree char *default_container = ptyxis_profile_dup_default_container (self->profile);
          container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, default_container);
        }

      if (container != NULL)
        {
          if (ptyxis_ipc_container_call_translate_uri_sync (container, uri, &translated, NULL, NULL))
            uri = translated;
        }

      if (ptyxis_get_process_kind () == PTYXIS_PROCESS_KIND_FLATPAK &&
          (guri = g_uri_parse (uri, 0, NULL)) &&
          !g_str_has_prefix (g_uri_get_path (guri), g_get_home_dir ()))
        {
          const char *path = g_uri_get_path (guri);
          g_autofree char *new_path = g_build_filename ("/var/run/host", path, NULL);
          g_autoptr(GUri) rewritten = NULL;

          rewritten = g_uri_build (0,
                                   "file",
                                   g_uri_get_userinfo (guri),
                                   g_uri_get_host (guri),
                                   g_uri_get_port (guri),
                                   new_path,
                                   g_uri_get_query (guri),
                                   g_uri_get_fragment (guri));

          g_clear_pointer (&translated, g_free);
          uri = translated = g_uri_to_string (rewritten);
        }
    }
  else if (!g_utf8_strchr (uri, -1, ':') && g_utf8_strchr (uri, -1, '@'))
    {
      uri = translated = g_strconcat ("mailto:", uri, NULL);
    }

  if (portal == NULL)
    portal = xdp_portal_new ();

  parent = xdp_parent_new_gtk (window);
  xdp_portal_open_uri (portal,
                       parent,
                       uri,
                       XDP_OPEN_URI_FLAG_NONE,
                       NULL,
                       ptyxis_terminal_surface_open_uri_cb,
                       g_object_ref (self));
  xdp_parent_free (parent);
}
#else
void
ptyxis_terminal_surface_open_uri (PtyxisTerminalSurface *self,
                                  const char            *uri)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_show_uri (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self->scrolled_window))), uri, 0);
  G_GNUC_END_IGNORE_DEPRECATIONS
}
#endif

char *
ptyxis_terminal_surface_query_working_directory_from_agent (PtyxisTerminalSurface *self)
{
  g_autofree char *path = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  VtePty *pty;
  int pty_fd;
  int handle;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), NULL);

  if (self->process == NULL)
    return NULL;

  pty = vte_terminal_get_pty (VTE_TERMINAL (self->terminal));
  pty_fd = vte_pty_get_fd (pty);
  fd_list = g_unix_fd_list_new ();
  handle = g_unix_fd_list_append (fd_list, pty_fd, NULL);

  if (ptyxis_ipc_process_call_get_working_directory_sync (self->process,
                                                          g_variant_new_handle (handle),
                                                          fd_list,
                                                          &path,
                                                          NULL, NULL, NULL))
    return g_steal_pointer (&path);

  return NULL;
}

void
ptyxis_terminal_surface_set_keep_terminal_open (PtyxisTerminalSurface *self,
                                                gboolean               keep_terminal_open)
{
  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));

  self->keep_terminal_open = !!keep_terminal_open;
}

gboolean
ptyxis_terminal_surface_get_has_pressed_key (PtyxisTerminalSurface *self)
{
  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self), FALSE);

  return self->has_pressed_key;
}

void
ptyxis_terminal_surface_serialize (PtyxisTerminalSurface *self,
                                   GVariantBuilder       *builder)
{
  g_autoptr(PtyxisIpcContainer) container = NULL;
  g_autofree char *cwd = NULL;
  PtyxisProfile *profile;
  PtyxisTerminal *terminal;
  const char *uuid;
  const char *window_title;
  PtyxisZoomLevel zoom;
  unsigned int columns;
  unsigned int rows;

  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_return_if_fail (builder != NULL);

  profile = ptyxis_terminal_surface_get_profile (self);
  terminal = ptyxis_terminal_surface_get_terminal (self);
  uuid = ptyxis_profile_get_uuid (profile);
  container = ptyxis_terminal_surface_dup_container (self);
  cwd = ptyxis_terminal_surface_dup_current_directory_uri (self);
  zoom = ptyxis_terminal_surface_get_zoom (self);
  columns = vte_terminal_get_column_count (VTE_TERMINAL (terminal));
  rows = vte_terminal_get_row_count (VTE_TERMINAL (terminal));

  if (cwd == NULL)
    cwd = ptyxis_terminal_surface_dup_previous_working_directory_uri (self);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  window_title = vte_terminal_get_window_title (VTE_TERMINAL (terminal));
  G_GNUC_END_IGNORE_DEPRECATIONS

  if (ptyxis_str_empty0 (window_title))
    window_title = ptyxis_terminal_surface_get_initial_title (self);

  g_variant_builder_add_parsed (builder, "{'profile', <%s>}", uuid);
  g_variant_builder_add_parsed (builder, "{'size', <(%u,%u)>}", columns, rows);

  if (zoom != PTYXIS_ZOOM_LEVEL_DEFAULT)
    g_variant_builder_add_parsed (builder, "{'zoom', <%u>}", zoom);
  if (!ptyxis_str_empty0 (cwd))
    g_variant_builder_add_parsed (builder, "{'cwd', <%s>}", cwd);
  if (!ptyxis_str_empty0 (window_title))
    g_variant_builder_add_parsed (builder, "{'window-title', <%s>}", window_title);
  if (!ptyxis_str_empty0 (self->title_prefix))
    g_variant_builder_add_parsed (builder, "{'title-prefix', <%s>}", self->title_prefix);

  if (container != NULL)
    {
      g_autofree char *default_container = NULL;
      const char *container_id;

      container_id = ptyxis_ipc_container_get_id (container);
      default_container = ptyxis_profile_dup_default_container (profile);

      if (container_id != NULL &&
          g_strcmp0 (default_container, container_id) != 0)
        g_variant_builder_add_parsed (builder, "{'container', <%s>}", container_id);
    }
}

void
ptyxis_terminal_surface_deserialize (PtyxisTerminalSurface *self,
                                     GVariant              *variant)
{
  g_autoptr(PtyxisIpcContainer) container = NULL;
  PtyxisApplication *app = PTYXIS_APPLICATION_DEFAULT;
  PtyxisTerminal *terminal;
  const char *container_id;
  const char *title_prefix;
  const char *cwd;
  const char *window_title;
  uint32_t zoom;
  uint32_t columns;
  uint32_t rows;

  g_return_if_fail (PTYXIS_IS_TERMINAL_SURFACE (self));
  g_return_if_fail (variant != NULL);

  if (g_variant_lookup (variant, "container", "&s", &container_id) &&
      !ptyxis_str_empty0 (container_id))
    {
      container = ptyxis_application_lookup_container (app, container_id);
      if (container != NULL)
        ptyxis_terminal_surface_set_container (self, container);
    }

  if (g_variant_lookup (variant, "cwd", "&s", &cwd) &&
      !ptyxis_str_empty0 (cwd))
    ptyxis_terminal_surface_set_previous_working_directory_uri (self, cwd);

  if (g_variant_lookup (variant, "zoom", "u", &zoom) &&
      zoom < PTYXIS_ZOOM_LEVEL_LAST &&
      zoom != PTYXIS_ZOOM_LEVEL_DEFAULT)
    ptyxis_terminal_surface_set_zoom (self, zoom);

  if (g_variant_lookup (variant, "window-title", "&s", &window_title) &&
      !ptyxis_str_empty0 (window_title))
    ptyxis_terminal_surface_set_initial_title (self, window_title);

  if (g_variant_lookup (variant, "title-prefix", "&s", &title_prefix) &&
      !ptyxis_str_empty0 (title_prefix))
    ptyxis_terminal_surface_set_title_prefix (self, title_prefix);

  terminal = ptyxis_terminal_surface_get_terminal (self);

  if (g_variant_lookup (variant, "size", "(uu)", &columns, &rows) &&
      columns > 0 && rows > 0)
    vte_terminal_set_size (VTE_TERMINAL (terminal), columns, rows);
}

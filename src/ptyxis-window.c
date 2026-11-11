/* ptyxis-window.c
 *
 * Copyright 2023 Christian Hergert
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

#include "ptyxis-application.h"
#include "ptyxis-close-dialog.h"
#include "ptyxis-find-bar.h"
#include "ptyxis-fullscreen-box.h"
#include "ptyxis-parking-lot.h"
#include "ptyxis-preferences-window.h"
#include "ptyxis-settings.h"
#include "ptyxis-shrinker.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-surface-monitor.h"
#include "ptyxis-theme-selector.h"
#include "ptyxis-title-dialog.h"
#include "ptyxis-profile-dialog.h"
#include "ptyxis-util.h"
#include "ptyxis-window.h"
#include "ptyxis-window-private.h"
#include "ptyxis-window-dressing.h"

#ifdef GDK_WINDOWING_X11
# include <gdk/x11/gdkx.h>
#endif

G_DEFINE_TYPE_WITH_PRIVATE (PtyxisWindow, ptyxis_window, ADW_TYPE_APPLICATION_WINDOW)

#define GET_PRIV(self) ((PtyxisWindowPrivate *)ptyxis_window_get_instance_private (self))

PtyxisWindowPrivate *
_ptyxis_window_get_priv (PtyxisWindow *self)
{
  return GET_PRIV (self);
}

enum {
  PROP_0,
  PROP_SHORTCUTS,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

PtyxisTerminalSurface *
ptyxis_window_get_active_surface (PtyxisWindow *self)
{
  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), NULL);

  return PTYXIS_WINDOW_GET_CLASS (self)->get_active_surface (self);
}

void
ptyxis_window_save_size (PtyxisWindow *self)
{
  PtyxisTerminalSurface *surface;

  g_assert (PTYXIS_IS_WINDOW (self));

  if ((surface = ptyxis_window_get_active_surface (self)))
    {
      PtyxisSettings *settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
      PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (surface);
      guint columns = vte_terminal_get_column_count (VTE_TERMINAL (terminal));
      guint rows = vte_terminal_get_row_count (VTE_TERMINAL (terminal));

      ptyxis_settings_set_window_size (settings, columns, rows);
    }
}

gboolean
_ptyxis_window_bind_title_cb (GBinding     *binding,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data)
{
  const char *str = g_value_get_string (from_value);

  if (ptyxis_str_empty0 (str))
    g_value_set_static_string (to_value, ptyxis_app_name ());
  else
    g_value_set_string (to_value, str);

  return TRUE;
}

static PtyxisProfile *
ptyxis_window_dup_profile_for_param (PtyxisWindow *self,
                                     const char   *profile_uuid)
{
  g_autoptr(PtyxisProfile) profile = NULL;
  PtyxisApplication *app;
  PtyxisProfile *active_profile;

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (profile_uuid != NULL);

  app = PTYXIS_APPLICATION_DEFAULT;
  active_profile = ptyxis_window_get_active_profile (self);

  if (profile_uuid[0] == 0 && active_profile != NULL)
    profile = g_object_ref (active_profile);
  else if (profile_uuid[0] == 0 || g_str_equal (profile_uuid, "default"))
    profile = ptyxis_application_dup_default_profile (app);
  else
    profile = ptyxis_application_dup_profile (app, profile_uuid);

  return g_steal_pointer (&profile);
}

void
_ptyxis_window_apply_current_settings_to_surface (PtyxisWindow          *self,
                                                  PtyxisTerminalSurface *surface,
                                                  PtyxisTerminalSurface *source)
{
  PtyxisApplication *app;
  PtyxisProfile *profile;

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));

  app = PTYXIS_APPLICATION_DEFAULT;
  profile = ptyxis_terminal_surface_get_profile (surface);

  if (source == NULL)
    source = ptyxis_window_get_active_surface (self);

  if (source != NULL && source != surface)
    {
      PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (source);
      g_autofree char *current_directory_uri = ptyxis_terminal_surface_dup_current_directory_uri (source);
      const char *current_container_name = ptyxis_terminal_get_current_container_name (terminal);
      const char *current_container_runtime = ptyxis_terminal_get_current_container_runtime (terminal);
      PtyxisZoomLevel zoom = ptyxis_terminal_surface_get_zoom (source);
      g_autoptr(PtyxisIpcContainer) current_container = NULL;

      if (ptyxis_profile_get_preserve_container (profile) != PTYXIS_PRESERVE_CONTAINER_NEVER)
        {
          if ((current_container = ptyxis_application_find_container_by_name (app,
                                                                              current_container_runtime,
                                                                              current_container_name)) ||
              (current_container = ptyxis_terminal_surface_dup_container (source)))
            ptyxis_terminal_surface_set_container (surface, current_container);
        }

      if (current_directory_uri != NULL)
        ptyxis_terminal_surface_set_previous_working_directory_uri (surface, current_directory_uri);

      ptyxis_terminal_surface_set_zoom (surface, zoom);

      vte_terminal_set_size (VTE_TERMINAL (ptyxis_terminal_surface_get_terminal (surface)),
                             vte_terminal_get_column_count (VTE_TERMINAL (terminal)),
                             vte_terminal_get_row_count (VTE_TERMINAL (terminal)));
    }
}

static void
ptyxis_window_new_tab_action (GtkWidget  *widget,
                              const char *action_name,
                              GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  g_autoptr(PtyxisProfile) profile = NULL;
  PtyxisTerminalSurface *prev_surface;
  PtyxisTerminalSurface *surface;
  const char *profile_uuid = "";
  const char *container_id = "";

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE ("(ss)")));

  g_variant_get (param, "(&s&s)", &profile_uuid, &container_id);
  profile = ptyxis_window_dup_profile_for_param (self, profile_uuid);

  prev_surface = ptyxis_window_get_active_surface (self);
  surface = ptyxis_window_add_terminal (self, profile);
  _ptyxis_window_apply_current_settings_to_surface (self, surface, prev_surface);

  if (!ptyxis_str_empty0 (container_id))
    {
      g_autoptr(PtyxisIpcContainer) container = NULL;

      if ((container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, container_id)))
        ptyxis_terminal_surface_set_container (surface, container);
    }

  ptyxis_terminal_surface_grab_focus (surface);
}

static void
ptyxis_window_new_window_action (GtkWidget  *widget,
                                 const char *action_name,
                                 GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  g_autoptr(PtyxisProfile) profile = NULL;
  const char *profile_uuid = "";
  const char *container_id = "";

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE ("(ss)")));

  g_variant_get (param, "(&s&s)", &profile_uuid, &container_id);
  profile = ptyxis_window_dup_profile_for_param (self, profile_uuid);

  PTYXIS_WINDOW_GET_CLASS (self)->open_new_window (self, profile, container_id);
}

static void
ptyxis_window_new_terminal_action (GtkWidget  *widget,
                                   const char *action_name,
                                   GVariant   *param)
{
  g_assert (PTYXIS_IS_WINDOW (widget));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE ("(ss)")));

  if (ptyxis_application_control_is_pressed (PTYXIS_APPLICATION_DEFAULT))
    ptyxis_window_new_window_action (widget, NULL, param);
  else
    ptyxis_window_new_tab_action (widget, NULL, param);
}

static void
ptyxis_window_maybe_resize_for_zoom (PtyxisWindow *self,
                                     GVariant     *param)
{
  if (g_variant_get_boolean (param) &&
      PTYXIS_WINDOW_GET_CLASS (self)->resize_for_zoom &&
      PTYXIS_WINDOW_GET_CLASS (self)->resize_for_zoom (self))
    gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);
}

static void
ptyxis_window_zoom_in_action (GtkWidget  *widget,
                              const char *action_name,
                              GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  PtyxisTerminalSurface *surface;

  g_assert (PTYXIS_IS_WINDOW (self));

  if ((surface = ptyxis_window_get_active_surface (self)))
    {
      ptyxis_terminal_surface_zoom_in (surface);
      ptyxis_window_maybe_resize_for_zoom (self, param);
    }
}

static void
ptyxis_window_zoom_out_action (GtkWidget  *widget,
                               const char *action_name,
                               GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  PtyxisTerminalSurface *surface;

  g_assert (PTYXIS_IS_WINDOW (self));

  if ((surface = ptyxis_window_get_active_surface (self)))
    {
      ptyxis_terminal_surface_zoom_out (surface);
      ptyxis_window_maybe_resize_for_zoom (self, param);
    }
}

static void
ptyxis_window_zoom_one_action (GtkWidget  *widget,
                               const char *action_name,
                               GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  PtyxisTerminalSurface *surface;

  g_assert (PTYXIS_IS_WINDOW (self));

  if ((surface = ptyxis_window_get_active_surface (self)))
    {
      ptyxis_terminal_surface_set_zoom (surface, PTYXIS_ZOOM_LEVEL_DEFAULT);
      ptyxis_window_maybe_resize_for_zoom (self, param);
    }
}

static void
ptyxis_window_active_surface_bell_cb (PtyxisWindow          *self,
                                      PtyxisTerminalSurface *surface)
{
  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));

  ptyxis_window_visual_bell (self);
}

static void
ptyxis_window_active_surface_commit_cb (PtyxisWindow          *self,
                                        const char            *str,
                                        PtyxisTerminalSurface *surface)
{
  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));

  if (GET_PRIV (self)->is_fullscreen)
    ptyxis_fullscreen_box_unreveal (GET_PRIV (self)->fullscreen_box);
}

static void
ptyxis_window_fullscreen_action (GtkWidget  *widget,
                                 const char *action_name,
                                 GVariant   *param)
{
  gtk_window_fullscreen (GTK_WINDOW (widget));
}

static void
ptyxis_window_unfullscreen_action (GtkWidget  *widget,
                                   const char *action_name,
                                   GVariant   *param)
{
  gtk_window_unfullscreen (GTK_WINDOW (widget));
}

static void
ptyxis_window_toggle_fullscreen (GtkWidget  *widget,
                                 const char *action_name,
                                 GVariant   *param)
{
  if (gtk_window_is_fullscreen (GTK_WINDOW (widget)))
    gtk_window_unfullscreen (GTK_WINDOW (widget));
  else
    gtk_window_fullscreen (GTK_WINDOW (widget));
}

static void
ptyxis_window_notify_is_active_cb (PtyxisWindow *self,
                                   GParamSpec   *pspec)
{
  g_assert (PTYXIS_IS_WINDOW (self));

  if (gtk_window_is_active (GTK_WINDOW (self)))
    {
#ifdef GDK_WINDOWING_X11
      {
        GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (self));

        if (GDK_IS_X11_SURFACE (surface))
          gdk_x11_surface_set_urgency_hint (surface, FALSE);
      }
#endif
    }
}

static void
ptyxis_window_toplevel_state_changed_cb (PtyxisWindow *self,
                                         GParamSpec   *pspec,
                                         GdkToplevel  *toplevel)
{
  GdkToplevelState state;
  gboolean is_fullscreen;
  gboolean is_maximized;

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (GDK_IS_TOPLEVEL (toplevel));

  state = gdk_toplevel_get_state (toplevel);

  is_fullscreen = !!(state & GDK_TOPLEVEL_STATE_FULLSCREEN);
  is_maximized = !!(state & GDK_TOPLEVEL_STATE_MAXIMIZED);

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.fullscreen", !is_fullscreen);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.unfullscreen", is_fullscreen);

  ptyxis_fullscreen_box_set_fullscreen (GET_PRIV (self)->fullscreen_box, is_fullscreen);

  if (!GET_PRIV (self)->is_fullscreen && is_fullscreen)
    ptyxis_fullscreen_box_reveal (GET_PRIV (self)->fullscreen_box);

  if (is_fullscreen)
    gtk_widget_add_css_class (GTK_WIDGET (self), "fullscreen");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "fullscreen");

  GET_PRIV (self)->is_fullscreen = is_fullscreen;
  GET_PRIV (self)->is_maximized = is_maximized;
}

static void
ptyxis_window_realize (GtkWidget *widget)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  GdkToplevel *toplevel;

  g_assert (PTYXIS_IS_WINDOW (self));

  GTK_WIDGET_CLASS (ptyxis_window_parent_class)->realize (widget);

  toplevel = GDK_TOPLEVEL (gtk_native_get_surface (GTK_NATIVE (self)));

  g_signal_connect_object (toplevel,
                           "notify::state",
                           G_CALLBACK (ptyxis_window_toplevel_state_changed_cb),
                           self,
                           G_CONNECT_SWAPPED);
}

static void
ptyxis_window_shortcuts_notify_cb (PtyxisWindow    *self,
                                   GParamSpec      *pspec,
                                   PtyxisShortcuts *shortcuts)
{
  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (PTYXIS_IS_SHORTCUTS (shortcuts));

  ptyxis_shortcuts_update_menu (shortcuts, GET_PRIV (self)->primary_menu);
}

static void
ptyxis_window_set_title_action (GtkWidget  *widget,
                                const char *action_name,
                                GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  PtyxisTerminalSurface *surface;
  AdwDialog *dialog;

  g_assert (PTYXIS_IS_WINDOW (self));

  if (!(surface = ptyxis_window_get_active_surface (self)))
    return;

  dialog = g_object_new (PTYXIS_TYPE_TITLE_DIALOG,
                         "surface", surface,
                         "title", _("Set Title"),
                         NULL);

  adw_dialog_set_presentation_mode (dialog, ADW_DIALOG_FLOATING);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
ptyxis_window_set_profile_action (GtkWidget  *widget,
                                  const char *action_name,
                                  GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  PtyxisTerminalSurface *surface;
  AdwDialog *dialog;

  g_assert (PTYXIS_IS_WINDOW (self));

  if (!(surface = ptyxis_window_get_active_surface (self)))
    return;

  dialog = g_object_new (PTYXIS_TYPE_PROFILE_DIALOG,
                         "surface", surface,
                         NULL);

  adw_dialog_set_presentation_mode (dialog, ADW_DIALOG_FLOATING);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
ptyxis_window_search_action (GtkWidget  *widget,
                             const char *action_name,
                             GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;

  g_assert (PTYXIS_IS_WINDOW (self));

  gtk_revealer_set_reveal_child (GET_PRIV (self)->find_bar_revealer, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (GET_PRIV (self)->find_bar));
}

static void
ptyxis_window_tab_reset_action (GtkWidget  *widget,
                                const char *action_name,
                                GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  PtyxisTerminalSurface *surface;
  PtyxisTerminal *terminal;
  gboolean clear;

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE_BOOLEAN));

  clear = g_variant_get_boolean (param);

  if (!(surface = ptyxis_window_get_active_surface (self)))
    return;

  terminal = ptyxis_terminal_surface_get_terminal (surface);
  vte_terminal_reset (VTE_TERMINAL (terminal), TRUE, clear);
}

static void
ptyxis_window_preferences_action (GtkWidget  *widget,
                                  const char *action_name,
                                  GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;
  const GList *windows;
  GtkWindow *window;

  g_assert (PTYXIS_IS_WINDOW (self));

  windows = gtk_application_get_windows (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT));

  for (const GList *iter = windows; iter != NULL; iter = iter->next)
    {
      if (PTYXIS_IS_PREFERENCES_WINDOW (iter->data))
        {
          gtk_window_present (GTK_WINDOW (iter->data));
          return;
        }
    }

  window = PTYXIS_WINDOW_GET_CLASS (self)->create_preferences_window (self);
  gtk_application_add_window (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT), window);
  gtk_window_set_transient_for (window, GTK_WINDOW (self));
  gtk_window_set_modal (window, FALSE);
  gtk_window_present (window);
}

static void
ptyxis_window_show_keyboard_shortcuts_action (GtkWidget  *widget,
                                              const char *action_name,
                                              GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;

  g_assert (PTYXIS_IS_WINDOW (self));

  ptyxis_window_preferences_action (widget, action_name, param);

  for (const GList *iter = gtk_application_get_windows (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT));
       iter != NULL;
       iter = iter->next)
    {
      if (PTYXIS_IS_PREFERENCES_WINDOW (iter->data))
        {
          ptyxis_preferences_window_edit_shortcuts (PTYXIS_PREFERENCES_WINDOW (iter->data));
          return;
        }
    }
}

static void
ptyxis_window_primary_menu_action (GtkWidget  *widget,
                                   const char *action_name,
                                   GVariant   *param)
{
  PtyxisWindow *self = (PtyxisWindow *)widget;

  g_assert (PTYXIS_IS_WINDOW (self));

  gtk_menu_button_popup (GET_PRIV (self)->primary_menu_button);
}

static void
ptyxis_window_close_request_cb (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  g_autoptr(PtyxisWindow) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_WINDOW (self));

  if (_ptyxis_close_dialog_run_finish (result, &error))
    gtk_window_destroy (GTK_WINDOW (self));

  GET_PRIV (self)->in_close_request = FALSE;
}

static gboolean
is_last_window (PtyxisWindow *self)
{
  const GList *windows = gtk_application_get_windows (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT));

  for (const GList *iter = windows;
       iter;
       iter = iter->next)
    {
      if (PTYXIS_IS_WINDOW (iter->data) && iter->data != (gpointer)self)
        return FALSE;
    }

  return TRUE;
}

static gboolean
ptyxis_window_close_request (GtkWindow *window)
{
  PtyxisWindow *self = (PtyxisWindow *)window;
  g_autoptr(GPtrArray) surfaces = NULL;
  PtyxisSettings *settings;

  g_assert (PTYXIS_IS_WINDOW (self));

  ptyxis_window_save_size (self);

  if (!GET_PRIV (self)->single_terminal_mode && is_last_window (self))
    ptyxis_application_save_session (PTYXIS_APPLICATION_DEFAULT);

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  if (!ptyxis_settings_get_prompt_on_close (settings))
    return GDK_EVENT_PROPAGATE;

  surfaces = g_ptr_array_new_with_free_func (g_object_unref);

  GET_PRIV (self)->in_close_request = TRUE;

  PTYXIS_WINDOW_GET_CLASS (self)->collect_surfaces (self, surfaces);

  if (surfaces->len > 0)
    _ptyxis_close_dialog_run_async (GTK_WINDOW (self),
                                    surfaces,
                                    TRUE,
                                    NULL,
                                    ptyxis_window_close_request_cb,
                                    g_object_ref (self));
  else
    GET_PRIV (self)->in_close_request = FALSE;

  return surfaces->len > 0;
}

static void
ptyxis_window_notify_process_leader_kind_cb (PtyxisWindow          *self,
                                             GParamSpec            *pspec,
                                             PtyxisTerminalSurface *surface)
{
  PtyxisProcessLeaderKind kind;

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));

  kind = ptyxis_terminal_surface_get_process_leader_kind (surface);

  gtk_widget_remove_css_class (GTK_WIDGET (self), "container");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "remote");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "superuser");

  if (kind == PTYXIS_PROCESS_LEADER_KIND_SUPERUSER)
    gtk_widget_add_css_class (GTK_WIDGET (self), "superuser");
  else if (kind == PTYXIS_PROCESS_LEADER_KIND_REMOTE)
    gtk_widget_add_css_class (GTK_WIDGET (self), "remote");
  else if (kind == PTYXIS_PROCESS_LEADER_KIND_CONTAINER)
    gtk_widget_add_css_class (GTK_WIDGET (self), "container");
}

static void
ptyxis_window_notify_zoom_cb (PtyxisWindow          *self,
                              GParamSpec            *pspec,
                              PtyxisTerminalSurface *surface)
{
  PtyxisZoomLevel zoom;
  gboolean can_zoom_in;
  gboolean can_zoom_out;
  gboolean can_zoom_one;

  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));

  zoom = ptyxis_terminal_surface_get_zoom (surface);

  can_zoom_in = zoom != PTYXIS_ZOOM_LEVEL_PLUS_14;
  can_zoom_one = zoom != PTYXIS_ZOOM_LEVEL_DEFAULT;
  can_zoom_out = zoom != PTYXIS_ZOOM_LEVEL_MINUS_14;

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.zoom-in", can_zoom_in);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.zoom-one", can_zoom_one);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.zoom-out", can_zoom_out);
}

static void
ptyxis_window_active_surface_bind_cb (PtyxisWindow          *self,
                                      PtyxisTerminalSurface *surface,
                                      GSignalGroup          *signals)
{
  g_assert (PTYXIS_IS_WINDOW (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));
  g_assert (G_IS_SIGNAL_GROUP (signals));

  ptyxis_window_notify_process_leader_kind_cb (self, NULL, surface);
  ptyxis_window_notify_zoom_cb (self, NULL, surface);
}

static void
ptyxis_window_add_theme_controls (PtyxisWindow *self)
{
  g_autoptr(GPropertyAction) interface_style = NULL;
  PtyxisSettings *settings;
  GtkPopover *popover;
  GtkWidget *selector;

  g_assert (PTYXIS_IS_WINDOW (self));

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  GET_PRIV (self)->interface_style_action = g_property_action_new ("interface-style", settings, "interface-style");
  g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (GET_PRIV (self)->interface_style_action));

  popover = gtk_menu_button_get_popover (GET_PRIV (self)->primary_menu_button);
  selector = g_object_new (PTYXIS_TYPE_THEME_SELECTOR,
                           "action-name", "win.interface-style",
                           NULL);
  gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), selector, "interface-style");
}

static void
ptyxis_window_my_computer_action (GtkWidget  *widget,
                                  const char *action_name,
                                  GVariant   *param)
{
  g_assert (PTYXIS_IS_WINDOW (widget));

  gtk_widget_activate_action (widget, "win.new-terminal", "(ss)", "", "session");
}

static void
ptyxis_window_constructed (GObject *object)
{
  PtyxisWindow *self = (PtyxisWindow *)object;

  G_OBJECT_CLASS (ptyxis_window_parent_class)->constructed (object);

  GET_PRIV (self)->dressing = ptyxis_window_dressing_new (self);
  g_binding_group_bind (GET_PRIV (self)->profile_bindings, "palette",
                        GET_PRIV (self)->dressing, "palette",
                        G_BINDING_SYNC_CREATE);
  g_binding_group_bind (GET_PRIV (self)->profile_bindings, "opacity",
                        GET_PRIV (self)->dressing, "opacity",
                        G_BINDING_SYNC_CREATE);

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.unfullscreen", FALSE);

  ptyxis_window_add_theme_controls (self);

  g_signal_connect (self,
                    "notify::is-active",
                    G_CALLBACK (ptyxis_window_notify_is_active_cb),
                    NULL);
}

static void
ptyxis_window_dispose (GObject *object)
{
  PtyxisWindow *self = (PtyxisWindow *)object;

  GET_PRIV (self)->disposed = TRUE;

  g_action_map_remove_action (G_ACTION_MAP (self), "interface-style");

  g_signal_group_set_target (GET_PRIV (self)->active_surface_signals, NULL);
  g_binding_group_set_source (GET_PRIV (self)->active_surface_bindings, NULL);
  g_binding_group_set_source (GET_PRIV (self)->profile_bindings, NULL);
  g_clear_object (&GET_PRIV (self)->parking_lot);
  g_clear_object (&GET_PRIV (self)->interface_style_action);

  G_OBJECT_CLASS (ptyxis_window_parent_class)->dispose (object);
}

static void
ptyxis_window_finalize (GObject *object)
{
  PtyxisWindow *self = (PtyxisWindow *)object;

  g_clear_object (&GET_PRIV (self)->active_surface_bindings);
  g_clear_object (&GET_PRIV (self)->active_surface_signals);
  g_clear_object (&GET_PRIV (self)->profile_bindings);

  G_OBJECT_CLASS (ptyxis_window_parent_class)->finalize (object);
}

static void
ptyxis_window_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  PtyxisWindow *self = PTYXIS_WINDOW (object);

  switch (prop_id)
    {
    case PROP_SHORTCUTS:
      g_value_set_object (value, GET_PRIV (self)->shortcuts);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_window_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_window_class_init (PtyxisWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  object_class->constructed = ptyxis_window_constructed;
  object_class->dispose = ptyxis_window_dispose;
  object_class->finalize = ptyxis_window_finalize;
  object_class->get_property = ptyxis_window_get_property;
  object_class->set_property = ptyxis_window_set_property;

  widget_class->realize = ptyxis_window_realize;

  window_class->close_request = ptyxis_window_close_request;

  properties[PROP_SHORTCUTS] =
    g_param_spec_object ("shortcuts", NULL, NULL,
                         PTYXIS_TYPE_SHORTCUTS,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_install_action (widget_class, "win.primary-menu", NULL, ptyxis_window_primary_menu_action);
  gtk_widget_class_install_action (widget_class, "win.new-tab", "(ss)", ptyxis_window_new_tab_action);
  gtk_widget_class_install_action (widget_class, "win.new-window", "(ss)", ptyxis_window_new_window_action);
  gtk_widget_class_install_action (widget_class, "win.new-terminal", "(ss)", ptyxis_window_new_terminal_action);
  gtk_widget_class_install_action (widget_class, "win.fullscreen", NULL, ptyxis_window_fullscreen_action);
  gtk_widget_class_install_action (widget_class, "win.unfullscreen", NULL, ptyxis_window_unfullscreen_action);
  gtk_widget_class_install_action (widget_class, "win.toggle-fullscreen", NULL, ptyxis_window_toggle_fullscreen);
  gtk_widget_class_install_action (widget_class, "win.zoom-in", "b", ptyxis_window_zoom_in_action);
  gtk_widget_class_install_action (widget_class, "win.zoom-out", "b", ptyxis_window_zoom_out_action);
  gtk_widget_class_install_action (widget_class, "win.zoom-one", "b", ptyxis_window_zoom_one_action);
  gtk_widget_class_install_action (widget_class, "win.set-title", NULL, ptyxis_window_set_title_action);
  gtk_widget_class_install_action (widget_class, "win.set-profile", NULL, ptyxis_window_set_profile_action);
  gtk_widget_class_install_action (widget_class, "win.search", NULL, ptyxis_window_search_action);
  gtk_widget_class_install_action (widget_class, "my-computer", NULL, ptyxis_window_my_computer_action);
  gtk_widget_class_install_action (widget_class, "win.preferences", NULL, ptyxis_window_preferences_action);
  gtk_widget_class_install_action (widget_class, "win.show-keyboard-shortcuts", NULL, ptyxis_window_show_keyboard_shortcuts_action);
  gtk_widget_class_install_action (widget_class, "tab.reset", "b", ptyxis_window_tab_reset_action);

  g_type_ensure (PTYXIS_TYPE_FIND_BAR);
  g_type_ensure (PTYXIS_TYPE_FULLSCREEN_BOX);
  g_type_ensure (PTYXIS_TYPE_SHRINKER);
}

static void
ptyxis_window_init (PtyxisWindow *self)
{
  GET_PRIV (self)->profile_bindings = g_binding_group_new ();

  GET_PRIV (self)->active_surface_bindings = g_binding_group_new ();
  GET_PRIV (self)->active_surface_signals = g_signal_group_new (PTYXIS_TYPE_TERMINAL_SURFACE);
  g_signal_connect_object (GET_PRIV (self)->active_surface_signals,
                           "bind",
                           G_CALLBACK (ptyxis_window_active_surface_bind_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_group_connect_object (GET_PRIV (self)->active_surface_signals,
                                 "bell",
                                 G_CALLBACK (ptyxis_window_active_surface_bell_cb),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (GET_PRIV (self)->active_surface_signals,
                                 "commit",
                                 G_CALLBACK (ptyxis_window_active_surface_commit_cb),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (GET_PRIV (self)->active_surface_signals,
                                 "notify::process-leader-kind",
                                 G_CALLBACK (ptyxis_window_notify_process_leader_kind_cb),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (GET_PRIV (self)->active_surface_signals,
                                 "notify::zoom",
                                 G_CALLBACK (ptyxis_window_notify_zoom_cb),
                                 self,
                                 G_CONNECT_SWAPPED);

  g_binding_group_bind (GET_PRIV (self)->active_surface_bindings, "profile",
                        GET_PRIV (self)->profile_bindings, "source",
                        G_BINDING_SYNC_CREATE);

  GET_PRIV (self)->parking_lot = ptyxis_parking_lot_new ();

  GET_PRIV (self)->shortcuts = g_object_ref (ptyxis_application_get_shortcuts (PTYXIS_APPLICATION_DEFAULT));
}

void
_ptyxis_window_init_after_template (PtyxisWindow *self)
{
#if DEVELOPMENT_BUILD
  gtk_widget_add_css_class (GTK_WIDGET (self), "devel");
#endif

  g_signal_connect_object (GET_PRIV (self)->shortcuts,
                           "notify",
                           G_CALLBACK (ptyxis_window_shortcuts_notify_cb),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_window_shortcuts_notify_cb (self, NULL, GET_PRIV (self)->shortcuts);
}

void
_ptyxis_window_class_bind_shared_template (GtkWidgetClass *widget_class)
{
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, find_bar);
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, find_bar_revealer);
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, fullscreen_box);
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, header_bar);
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, primary_menu);
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, primary_menu_button);
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, toasts);
  gtk_widget_class_bind_template_child_private (widget_class, PtyxisWindow, visual_bell);
}

PtyxisTerminalSurface *
ptyxis_window_add_terminal (PtyxisWindow  *self,
                            PtyxisProfile *profile)
{
  g_autoptr(PtyxisProfile) default_profile = NULL;

  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), NULL);

  if (profile == NULL)
    {
      default_profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);
      profile = default_profile;
    }

  return PTYXIS_WINDOW_GET_CLASS (self)->add_terminal (self, profile);
}

PtyxisWindow *
ptyxis_window_new (void)
{
  return ptyxis_application_new_window (PTYXIS_APPLICATION_DEFAULT);
}

PtyxisWindow *
ptyxis_window_new_empty (void)
{
  return ptyxis_application_new_window_empty (PTYXIS_APPLICATION_DEFAULT);
}

PtyxisWindow *
ptyxis_window_new_for_profile (PtyxisProfile *profile)
{
  return ptyxis_application_new_window_for_profile (PTYXIS_APPLICATION_DEFAULT, profile);
}

PtyxisWindow *
ptyxis_window_new_for_command (PtyxisProfile      *profile,
                               const char * const *argv,
                               const char         *cwd_uri)
{
  return ptyxis_application_new_window_for_command (PTYXIS_APPLICATION_DEFAULT, profile, argv, cwd_uri);
}

static gboolean
ptyxis_window_remove_visual_bell (gpointer data)
{
  PtyxisWindow *self = data;

  g_assert (PTYXIS_IS_WINDOW (self));

  GET_PRIV (self)->visual_bell_source = 0;

  gtk_widget_remove_css_class (GTK_WIDGET (GET_PRIV (self)->visual_bell), "visual-bell");

  return G_SOURCE_REMOVE;
}

void
ptyxis_window_visual_bell (PtyxisWindow *self)
{
  PtyxisSettings *settings;

  g_return_if_fail (PTYXIS_IS_WINDOW (self));

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  if (!ptyxis_settings_get_visual_bell (settings))
    return;

  gtk_widget_add_css_class (GTK_WIDGET (GET_PRIV (self)->visual_bell), "visual-bell");

  g_clear_handle_id (&GET_PRIV (self)->visual_bell_source, g_source_remove);

  GET_PRIV (self)->visual_bell_source = g_timeout_add_full (G_PRIORITY_HIGH_IDLE,
                                                 500,
                                                 ptyxis_window_remove_visual_bell,
                                                 g_object_ref (self),
                                                 g_object_unref);
}

PtyxisProfile *
ptyxis_window_get_active_profile (PtyxisWindow *self)
{
  PtyxisTerminalSurface *surface;

  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), NULL);

  if ((surface = ptyxis_window_get_active_surface (self)))
    return ptyxis_terminal_surface_get_profile (surface);

  return NULL;
}

gboolean
ptyxis_window_focus_by_uuid (PtyxisWindow *self,
                             const char   *uuid)
{
  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  return PTYXIS_WINDOW_GET_CLASS (self)->focus_by_uuid (self, uuid);
}

guint
ptyxis_window_get_n_pages (PtyxisWindow *self)
{
  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), 0);

  return PTYXIS_WINDOW_GET_CLASS (self)->get_n_pages (self);
}

GVariant *
ptyxis_window_serialize (PtyxisWindow *self)
{
  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), NULL);

  return PTYXIS_WINDOW_GET_CLASS (self)->serialize (self);
}

gboolean
ptyxis_window_deserialize (PtyxisWindow *self,
                           GVariant     *state)
{
  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);

  if (PTYXIS_WINDOW_GET_CLASS (self)->deserialize)
    return PTYXIS_WINDOW_GET_CLASS (self)->deserialize (self, state);

  return FALSE;
}

GMenu *
ptyxis_window_create_terminal_menu (PtyxisWindow *self)
{
  g_return_val_if_fail (PTYXIS_IS_WINDOW (self), NULL);

  if (PTYXIS_WINDOW_GET_CLASS (self)->create_terminal_menu)
    return PTYXIS_WINDOW_GET_CLASS (self)->create_terminal_menu (self);

  return NULL;
}

/*
 * ptyxis-notify.h
 *
 * Copyright 2023 Christian Hergert <christian@sourceandstack.com>
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

#pragma once

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
# include <gdk/x11/gdkx.h>
#endif

#include <glib/gi18n.h>

#include "ptyxis-tab.h"
#include "ptyxis-terminal.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-window.h"

#define TIME_WATERMARK_MSEC 250

G_BEGIN_DECLS

typedef struct _PtyxisNotify
{
  GtkWidget                  *widget;
  PtyxisTerminal             *terminal;
  PtyxisTerminalSurface      *surface;
  const char                 *uuid;

  char *current_cmdline;

  guint contents_changed_source;
  guint shell_preexec_source;

  gulong shell_precmd_handler;
  gulong shell_preexec_handler;

  gint64 command_start_time;

  guint between_preexec_and_precmd : 1;
} PtyxisNotify;

static inline void
ptyxis_notify_show_notification (PtyxisNotify *notify,
                                 const char   *cmdline)
{
  GtkRoot *window;
  PtyxisTab *active_tab;

  g_assert (notify != NULL);
  g_assert (GTK_IS_WIDGET (notify->widget));

  window = gtk_widget_get_root (notify->widget);

  if (!PTYXIS_IS_WINDOW (window))
    return;

  if (gtk_window_is_active (GTK_WINDOW (window)))
    {
      active_tab = ptyxis_window_get_active_tab (PTYXIS_WINDOW (window));

      if (active_tab != NULL &&
          ptyxis_tab_get_surface (active_tab) == notify->surface)
        return;
    }
  else
    {
      g_autoptr(GNotification) notification = NULL;
      g_autoptr(GIcon) icon = NULL;
      g_autofree char *cmdline_sanitized = NULL;
#ifdef GDK_WINDOWING_X11
      GdkSurface *surface;

      surface = gtk_native_get_surface (GTK_NATIVE (window));

      if (GDK_IS_X11_SURFACE (surface))
        gdk_x11_surface_set_urgency_hint (surface, TRUE);
#endif

      icon = g_themed_icon_new (APP_ID "-symbolic");
      cmdline_sanitized = g_utf8_make_valid (cmdline, -1);

      notification = g_notification_new (_("Command completed"));
      g_notification_set_body (notification, cmdline_sanitized);
      g_notification_set_icon (notification, icon);
      g_notification_set_default_action_and_target (notification,
                                                    "app.focus-tab-by-uuid",
                                                    "s", notify->uuid);
      g_application_send_notification (G_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                       notify->uuid, notification);
    }

  active_tab = PTYXIS_TAB (gtk_widget_get_ancestor (notify->widget, PTYXIS_TYPE_TAB));

  if (active_tab != NULL)
    ptyxis_tab_set_needs_attention (active_tab, TRUE);
}

static inline void
ptyxis_notify_shell_precmd_cb (PtyxisTerminal *terminal,
                               PtyxisNotify   *notify)
{
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  notify->between_preexec_and_precmd = FALSE;

  g_clear_handle_id (&notify->contents_changed_source, g_source_remove);
  g_clear_handle_id (&notify->shell_preexec_source, g_source_remove);

  if (notify->current_cmdline != NULL)
    {
      gint64 elapsed_time = g_get_monotonic_time () - notify->command_start_time;

      if (elapsed_time >= TIME_WATERMARK_MSEC * 1000)
        ptyxis_notify_show_notification (notify, notify->current_cmdline);

      g_clear_pointer (&notify->current_cmdline, g_free);
    }
}

static void
ptyxis_notify_shell_preexec_poll_cb (GObject      *object,
                                     GAsyncResult *result,
                                     gpointer      user_data)
{
  PtyxisTerminalSurface *surface = (PtyxisTerminalSurface *)object;
  PtyxisNotify *notify = user_data;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (notify != NULL);

  ptyxis_terminal_surface_poll_agent_finish (surface, result, NULL);

  g_set_str (&notify->current_cmdline,
             ptyxis_terminal_surface_get_command_line (surface));
}

static inline void
ptyxis_notify_shell_preexec_cb (PtyxisTerminal *terminal,
                                PtyxisNotify   *notify)
{
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  notify->between_preexec_and_precmd = TRUE;
  notify->command_start_time = g_get_monotonic_time ();

  g_set_str (&notify->current_cmdline, NULL);

  ptyxis_terminal_surface_poll_agent_async (notify->surface,
                                            NULL,
                                            ptyxis_notify_shell_preexec_poll_cb,
                                            notify);
}

static inline void
ptyxis_notify_init (PtyxisNotify          *notify,
                    GtkWidget             *widget,
                    PtyxisTerminal        *terminal,
                    PtyxisTerminalSurface *surface,
                    const char            *uuid)
{
  notify->widget = widget;
  notify->terminal = terminal;
  notify->surface = surface;
  notify->uuid = uuid;

  notify->contents_changed_source = 0;
  notify->shell_preexec_source = 0;
  notify->between_preexec_and_precmd = FALSE;
  notify->current_cmdline = NULL;
  notify->command_start_time = 0;

  notify->shell_precmd_handler =
    g_signal_connect (terminal,
                      "shell-precmd",
                      G_CALLBACK (ptyxis_notify_shell_precmd_cb),
                      notify);

  notify->shell_preexec_handler =
    g_signal_connect (terminal,
                      "shell-preexec",
                      G_CALLBACK (ptyxis_notify_shell_preexec_cb),
                      notify);
}

static inline void
ptyxis_notify_destroy (PtyxisNotify *notify)
{
  if (notify->widget == NULL)
    return;

  g_clear_handle_id (&notify->contents_changed_source, g_source_remove);
  g_clear_handle_id (&notify->shell_preexec_source, g_source_remove);

  g_clear_signal_handler (&notify->shell_precmd_handler, notify->terminal);
  g_clear_signal_handler (&notify->shell_preexec_handler, notify->terminal);

  g_clear_pointer (&notify->current_cmdline, g_free);

  notify->widget = NULL;
}

G_END_DECLS

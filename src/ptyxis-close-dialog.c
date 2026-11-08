/* ptyxis-close-dialog.c
 *
 * Copyright 2021 Christian Hergert <christian@sourceandstack.com>
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

#include "ptyxis-close-dialog.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-util.h"

typedef struct _CloseRequest
{
  PtyxisTerminalSurface *surface; /* (owned) (not nullable) */
  AdwAlertDialog        *dialog;  /* (owned) (not nullable) */
} CloseRequest;

static void
close_request_clear (gpointer data)
{
  CloseRequest *cr = data;

  g_clear_object (&cr->surface);
  g_clear_object (&cr->dialog);
}

static void
ptyxis_close_dialog_confirm (AdwAlertDialog *dialog,
                             GArray         *requests)
{
  g_assert (ADW_IS_ALERT_DIALOG (dialog));
  g_assert (requests != NULL);
  g_assert (requests->len > 0);

  gtk_widget_set_sensitive (GTK_WIDGET (dialog), FALSE);

  for (unsigned int i = 0; i < requests->len; i++)
    {
      const CloseRequest *cr = &g_array_index (requests, CloseRequest, i);

      ptyxis_terminal_surface_force_quit (cr->surface);
    }
}

static void
ptyxis_close_dialog_response (AdwAlertDialog *dialog,
                              const char     *response,
                              GArray         *requests)
{
  GTask *task;

  g_assert (ADW_IS_ALERT_DIALOG (dialog));

  task = g_object_get_data (G_OBJECT (dialog), "G_TASK");

  if (!g_strcmp0 (response, "discard"))
    {
      ptyxis_close_dialog_confirm (dialog, requests);
      g_task_return_boolean (task, TRUE);
    }
  else
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_CANCELLED,
                               "The user cancelled the request");
    }
}

static AdwDialog *
_ptyxis_close_dialog_new (GtkWindow *parent,
                          GPtrArray *surfaces,
                          gboolean   is_window)
{
  g_autoptr(GArray) requests = NULL;
  const char *heading;
  const char *body;
  const char *discard_label;
  AdwDialog *dialog;
  GtkWidget *group;

  g_return_val_if_fail (!parent || GTK_IS_WINDOW (parent), NULL);
  g_return_val_if_fail (surfaces != NULL, NULL);
  g_return_val_if_fail (surfaces->len > 0, NULL);

  requests = g_array_new (/* zero_terminated = */ FALSE,
                          /* clear_ = */ FALSE,
                          sizeof (CloseRequest));
  g_array_set_clear_func (requests, close_request_clear);

  if (is_window)
    {
      heading = _("Close Window?");
      body = _("Some processes are still running.");
      discard_label = g_dngettext (GETTEXT_PACKAGE,
                                   _("_Close"), _("_Close All"),
                                   surfaces->len);
    }
  else
    {
      heading = _("Close Terminal?");
      body = _("A process is still running.");
      discard_label = _("_Close");
    }

  dialog = adw_alert_dialog_new (heading, body);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                    "cancel", _("_Cancel"),
                                    "discard", discard_label,
                                    NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
                                              "discard", ADW_RESPONSE_DESTRUCTIVE);

  group = adw_preferences_group_new ();
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), group);

  for (unsigned int i = 0; i < surfaces->len; i++)
    {
      PtyxisTerminalSurface *surface = g_ptr_array_index (surfaces, i);
      g_autofree char *cmdline = NULL;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      GtkWidget *row;
      CloseRequest cr;
      GPid pid;

      if (ptyxis_terminal_surface_has_foreground_process (surface, &pid, &cmdline))
        {
          g_utf8_make_valid (cmdline, -1);
          title = g_steal_pointer (&cmdline);
          subtitle = g_strdup_printf (_("Process %d"), pid);
        }
      else
        {
          title = ptyxis_terminal_surface_dup_title (surface);
          subtitle = ptyxis_terminal_surface_dup_subtitle (surface);
        }

      if (g_utf8_strlen (title, -1) > 200)
        g_utf8_offset_to_pointer (title, 200)[0] = 0;

      row = adw_action_row_new ();

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);

      cr.surface = g_object_ref (surface);
      cr.dialog = g_object_ref (ADW_ALERT_DIALOG (dialog));

      g_array_append_val (requests, cr);
    }

  g_signal_connect_data (dialog,
                         "response",
                         G_CALLBACK (ptyxis_close_dialog_response),
                         g_steal_pointer (&requests),
                         (GClosureNotify) g_array_unref,
                         0);

  return dialog;
}

void
_ptyxis_close_dialog_run_async (GtkWindow           *parent,
                                GPtrArray           *surfaces,
                                gboolean             is_window,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  AdwDialog *dialog;

  g_return_if_fail (!parent || GTK_IS_WINDOW (parent));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  dialog = _ptyxis_close_dialog_new (parent, surfaces, is_window);
  task = g_task_new (dialog, cancellable, callback, user_data);
  g_task_set_source_tag (task, _ptyxis_close_dialog_run_async);

  if (surfaces == NULL || surfaces->len == 0)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  g_object_set_data_full (G_OBJECT (dialog),
                          "G_TASK",
                          g_steal_pointer (&task),
                          g_object_unref);
  adw_dialog_present (dialog, GTK_WIDGET (parent));
}

gboolean
_ptyxis_close_dialog_run_finish (GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

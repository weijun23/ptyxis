/*
 * ptyxis-session.c
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

#include "config.h"

#include "ptyxis-application.h"
#include "ptyxis-session.h"
#include "ptyxis-window.h"

GVariant *
ptyxis_session_save (PtyxisApplication *app)
{
  GVariantBuilder builder;

  g_return_val_if_fail (PTYXIS_IS_APPLICATION (app), NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add_parsed (&builder, "{'version', <%u>}", 1);
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("{sv}"));
  g_variant_builder_add (&builder, "s", "windows");
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("v"));
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("aa{sv}"));

  for (const GList *list = gtk_application_get_windows (GTK_APPLICATION (app));
       list != NULL;
       list = list->next)
    {
      if (PTYXIS_IS_WINDOW (list->data))
        {
          g_autoptr(GVariant) window_data = NULL;
          PtyxisWindow *window = PTYXIS_WINDOW (list->data);
          GVariantIter data_iter;
          const char *key;
          GVariant *value;

          g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));

          if (gtk_window_is_maximized (GTK_WINDOW (window)))
            g_variant_builder_add_parsed (&builder, "{'maximized', <%b>}", TRUE);

          window_data = ptyxis_window_serialize (window);
          g_variant_iter_init (&data_iter, window_data);
          while (g_variant_iter_loop (&data_iter, "{&sv}", &key, &value))
            g_variant_builder_add (&builder, "{sv}", key, value);

          g_variant_builder_close (&builder);
        }
    }

  g_variant_builder_close (&builder);
  g_variant_builder_close (&builder);
  g_variant_builder_close (&builder);

  return g_variant_take_ref (g_variant_builder_end (&builder));
}

gboolean
ptyxis_session_restore (PtyxisApplication *app,
                        GVariant          *state)
{
  g_autoptr(GVariant) windows = NULL;
  GVariantIter iter;
  GVariant *window;
  gboolean added_window = FALSE;
  guint32 version;

  g_return_val_if_fail (PTYXIS_IS_APPLICATION (app), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (g_variant_is_of_type (state, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (!g_variant_lookup (state, "version", "u", &version))
    return FALSE;

  if (!(windows = g_variant_lookup_value (state, "windows", G_VARIANT_TYPE ("aa{sv}"))))
    return FALSE;

  g_variant_iter_init (&iter, windows);
  while (g_variant_iter_loop (&iter, "@a{sv}", &window))
    {
      PtyxisWindow *the_window;
      gboolean maximized;

      the_window = ptyxis_application_new_window_empty (app);

      if (!ptyxis_window_deserialize (the_window, window))
        {
          gtk_window_destroy (GTK_WINDOW (the_window));
          continue;
        }

      if (!g_variant_lookup (window, "maximized", "b", &maximized))
        maximized = FALSE;

      if (maximized)
        gtk_window_maximize (GTK_WINDOW (the_window));

      gtk_window_present (GTK_WINDOW (the_window));
      added_window = TRUE;
    }

  return added_window;
}

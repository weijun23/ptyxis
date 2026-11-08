/*
 * ptyxis-tiling-preferences-window.c
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

#include "ptyxis-preferences-window-private.h"
#include "ptyxis-shortcut-row.h"
#include "ptyxis-tiling-preferences-window.h"

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

struct _PtyxisTilingPreferencesWindow
{
  PtyxisPreferencesWindow parent_instance;

  AdwPreferencesGroup *tiling_group;
};

G_DEFINE_FINAL_TYPE (PtyxisTilingPreferencesWindow, ptyxis_tiling_preferences_window, PTYXIS_TYPE_PREFERENCES_WINDOW)

static void
ptyxis_tiling_preferences_window_constructed (GObject *object)
{
  PtyxisTilingPreferencesWindow *self = (PtyxisTilingPreferencesWindow *)object;
  PtyxisPreferencesWindowPrivate *priv = _ptyxis_preferences_window_get_priv (PTYXIS_PREFERENCES_WINDOW (self));

  G_OBJECT_CLASS (ptyxis_tiling_preferences_window_parent_class)->constructed (object);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), self->tiling_group);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), priv->zoom_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), priv->app_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), priv->app_group_2);

  ptyxis_preferences_window_bind_shortcut_rows (PTYXIS_PREFERENCES_WINDOW (self),
                                                GTK_WIDGET (self->tiling_group));
}

static void
ptyxis_tiling_preferences_window_dispose (GObject *object)
{
  PtyxisTilingPreferencesWindow *self = (PtyxisTilingPreferencesWindow *)object;

  gtk_widget_dispose_template (GTK_WIDGET (self), PTYXIS_TYPE_TILING_PREFERENCES_WINDOW);

  G_OBJECT_CLASS (ptyxis_tiling_preferences_window_parent_class)->dispose (object);
}

static void
ptyxis_tiling_preferences_window_class_init (PtyxisTilingPreferencesWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = ptyxis_tiling_preferences_window_constructed;
  object_class->dispose = ptyxis_tiling_preferences_window_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Ptyxis/ptyxis-tiling-preferences-window.ui");

  gtk_widget_class_bind_template_child (widget_class, PtyxisTilingPreferencesWindow, tiling_group);
}

static void
ptyxis_tiling_preferences_window_init (PtyxisTilingPreferencesWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

G_GNUC_END_IGNORE_DEPRECATIONS

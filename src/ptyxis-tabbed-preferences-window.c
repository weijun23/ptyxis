/*
 * ptyxis-tabbed-preferences-window.c
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
#include "ptyxis-preferences-window-private.h"
#include "ptyxis-settings.h"
#include "ptyxis-shortcut-row.h"
#include "ptyxis-tabbed-preferences-window.h"

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

struct _PtyxisTabbedPreferencesWindow
{
  PtyxisPreferencesWindow parent_instance;

  AdwComboRow         *tab_position;
  GListModel          *tab_positions;
  AdwPreferencesGroup *tabs_group_1;
  AdwPreferencesGroup *tabs_group_2;
  AdwPreferencesGroup *tabs_group_3;
  AdwPreferencesGroup *tabs_group_4;
  AdwPreferencesGroup *tabs_group_5;
  AdwPreferencesGroup *tabs_group_6;
};

G_DEFINE_FINAL_TYPE (PtyxisTabbedPreferencesWindow, ptyxis_tabbed_preferences_window, PTYXIS_TYPE_PREFERENCES_WINDOW)

static void
ptyxis_tabbed_preferences_window_constructed (GObject *object)
{
  PtyxisTabbedPreferencesWindow *self = (PtyxisTabbedPreferencesWindow *)object;
  PtyxisPreferencesWindowPrivate *priv = _ptyxis_preferences_window_get_priv (PTYXIS_PREFERENCES_WINDOW (self));
  PtyxisSettings *settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  GSettings *gsettings = ptyxis_settings_get_settings (settings);

  G_OBJECT_CLASS (ptyxis_tabbed_preferences_window_parent_class)->constructed (object);

  adw_preferences_group_add (priv->interface_group, GTK_WIDGET (self->tab_position));

  g_settings_bind_with_mapping (gsettings,
                                PTYXIS_SETTING_KEY_NEW_TAB_POSITION,
                                self->tab_position,
                                "selected",
                                G_SETTINGS_BIND_DEFAULT,
                                ptyxis_preferences_window_string_to_index,
                                ptyxis_preferences_window_index_to_string,
                                g_object_ref (self->tab_positions),
                                g_object_unref);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), self->tabs_group_1);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), self->tabs_group_2);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), self->tabs_group_3);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), self->tabs_group_4);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), self->tabs_group_5);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), self->tabs_group_6);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), priv->zoom_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), priv->app_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (priv->shortcuts_page), priv->app_group_2);

  ptyxis_preferences_window_bind_shortcut_rows (PTYXIS_PREFERENCES_WINDOW (self),
                                                GTK_WIDGET (self->tabs_group_1));
  ptyxis_preferences_window_bind_shortcut_rows (PTYXIS_PREFERENCES_WINDOW (self),
                                                GTK_WIDGET (self->tabs_group_2));
  ptyxis_preferences_window_bind_shortcut_rows (PTYXIS_PREFERENCES_WINDOW (self),
                                                GTK_WIDGET (self->tabs_group_3));
  ptyxis_preferences_window_bind_shortcut_rows (PTYXIS_PREFERENCES_WINDOW (self),
                                                GTK_WIDGET (self->tabs_group_4));
  ptyxis_preferences_window_bind_shortcut_rows (PTYXIS_PREFERENCES_WINDOW (self),
                                                GTK_WIDGET (self->tabs_group_5));
  ptyxis_preferences_window_bind_shortcut_rows (PTYXIS_PREFERENCES_WINDOW (self),
                                                GTK_WIDGET (self->tabs_group_6));
}

static void
ptyxis_tabbed_preferences_window_dispose (GObject *object)
{
  PtyxisTabbedPreferencesWindow *self = (PtyxisTabbedPreferencesWindow *)object;

  gtk_widget_dispose_template (GTK_WIDGET (self), PTYXIS_TYPE_TABBED_PREFERENCES_WINDOW);

  G_OBJECT_CLASS (ptyxis_tabbed_preferences_window_parent_class)->dispose (object);
}

static void
ptyxis_tabbed_preferences_window_class_init (PtyxisTabbedPreferencesWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = ptyxis_tabbed_preferences_window_constructed;
  object_class->dispose = ptyxis_tabbed_preferences_window_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Ptyxis/ptyxis-tabbed-preferences-window.ui");

  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tab_position);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tab_positions);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tabs_group_1);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tabs_group_2);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tabs_group_3);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tabs_group_4);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tabs_group_5);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedPreferencesWindow, tabs_group_6);
}

static void
ptyxis_tabbed_preferences_window_init (PtyxisTabbedPreferencesWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

G_GNUC_END_IGNORE_DEPRECATIONS

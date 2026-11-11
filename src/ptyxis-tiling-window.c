/* ptyxis-tiling-window.c
 *
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

#include "ptyxis-application.h"
#include "ptyxis-close-dialog.h"
#include "ptyxis-pane.h"
#include "ptyxis-settings.h"
#include "ptyxis-split-tree.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-tiling-preferences-window.h"
#include "ptyxis-tiling-window.h"
#include "ptyxis-window-private.h"

struct _PtyxisTilingWindow
{
  PtyxisWindow     parent_instance;

  PtyxisSplitTree *split_tree;
};

G_DEFINE_FINAL_TYPE (PtyxisTilingWindow, ptyxis_tiling_window, PTYXIS_TYPE_WINDOW)

static void setup_tiling_window (PtyxisTilingWindow *self,
                                 PtyxisSplitTree    *split_tree);

static PtyxisTerminalSurface *
ptyxis_tiling_window_get_active_surface (PtyxisWindow *window)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);

  if (self->split_tree != NULL)
    {
      PtyxisPane *pane = ptyxis_split_tree_get_focused_pane (self->split_tree);

      if (pane != NULL)
        return ptyxis_pane_get_surface (pane);
    }

  return NULL;
}

static PtyxisTerminalSurface *
ptyxis_tiling_window_add_terminal (PtyxisWindow  *window,
                                   PtyxisProfile *profile)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);

  if (self->split_tree != NULL)
    {
      PtyxisPane *pane;
      PtyxisPane *target;

      pane = g_object_new (PTYXIS_TYPE_PANE, "profile", profile, NULL);
      target = ptyxis_split_tree_get_focused_pane (self->split_tree);

      if (target != NULL)
        ptyxis_split_tree_insert_pane (self->split_tree, pane, target,
                                      GTK_ORIENTATION_HORIZONTAL, TRUE);

      return ptyxis_pane_get_surface (pane);
    }
  else
    {
      setup_tiling_window (self, ptyxis_split_tree_new (profile));

      return ptyxis_pane_get_surface (ptyxis_split_tree_get_focused_pane (self->split_tree));
    }
}

static void
collect_running_pane_surface (gpointer data,
                              gpointer user_data)
{
  PtyxisPane *pane = data;
  GPtrArray *surfaces = user_data;
  PtyxisTerminalSurface *surface;

  surface = ptyxis_pane_get_surface (pane);

  if (ptyxis_terminal_surface_is_running (surface, NULL))
    g_ptr_array_add (surfaces, g_object_ref (surface));
}

static void
ptyxis_tiling_window_collect_surfaces (PtyxisWindow *window,
                                       GPtrArray    *surfaces)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);

  if (self->split_tree != NULL)
    ptyxis_split_tree_foreach_pane (self->split_tree,
                                    collect_running_pane_surface,
                                    surfaces);
}

typedef struct {
  const char *uuid;
  PtyxisPane *found;
} FindPaneByUuid;

static void
find_pane_by_uuid_cb (gpointer data,
                      gpointer user_data)
{
  PtyxisPane *pane = data;
  FindPaneByUuid *find = user_data;

  if (find->found == NULL &&
      g_strcmp0 (find->uuid, ptyxis_pane_get_uuid (pane)) == 0)
    find->found = pane;
}

static gboolean
ptyxis_tiling_window_focus_by_uuid_vfunc (PtyxisWindow *window,
                                          const char   *uuid)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);
  FindPaneByUuid find = { .uuid = uuid, .found = NULL };

  if (self->split_tree == NULL)
    return FALSE;

  ptyxis_split_tree_foreach_pane (self->split_tree,
                                  find_pane_by_uuid_cb,
                                  &find);

  if (find.found == NULL &&
      ptyxis_split_tree_get_maximized (self->split_tree))
    {
      ptyxis_split_tree_toggle_maximize (self->split_tree);

      ptyxis_split_tree_foreach_pane (self->split_tree,
                                      find_pane_by_uuid_cb,
                                      &find);
    }

  if (find.found != NULL)
    {
      gtk_window_present (GTK_WINDOW (self));
      ptyxis_pane_grab_focus (find.found);
      return TRUE;
    }

  return FALSE;
}

static guint
ptyxis_tiling_window_get_n_pages_vfunc (PtyxisWindow *window)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);

  if (self->split_tree == NULL)
    return 0;

  return ptyxis_split_tree_get_n_panes (self->split_tree);
}

static GVariant *
ptyxis_tiling_window_serialize_vfunc (PtyxisWindow *window)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);
  PtyxisSettings *settings;
  GVariantBuilder builder;

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  if (ptyxis_settings_get_restore_session (settings) && self->split_tree != NULL)
    {
      GVariant *tree_variant = ptyxis_split_tree_to_variant (self->split_tree);

      if (tree_variant != NULL)
        {
          int width = gtk_widget_get_width (GTK_WIDGET (self));
          int height = gtk_widget_get_height (GTK_WIDGET (self));

          g_variant_builder_add (&builder, "{sv}", "split-tree", tree_variant);
          if (width > 0 && height > 0)
            g_variant_builder_add_parsed (&builder, "{'window-size', <(%i,%i)>}",
                                          width, height);
        }
    }

  return g_variant_builder_end (&builder);
}

static gboolean
ptyxis_tiling_window_deserialize_vfunc (PtyxisWindow *window,
                                        GVariant     *state)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);
  g_autoptr(GVariant) tree_variant = NULL;
  PtyxisSplitTree *split_tree;
  PtyxisSettings *settings;
  gboolean restore_window_size;
  int width;
  int height;

  tree_variant = g_variant_lookup_value (state, "split-tree",
                                         G_VARIANT_TYPE ("a{sv}"));
  if (tree_variant == NULL)
    return FALSE;

  split_tree = ptyxis_split_tree_new_from_variant (tree_variant);
  if (split_tree == NULL)
    return FALSE;

  setup_tiling_window (self, split_tree);

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  restore_window_size = ptyxis_settings_get_restore_window_size (settings);

  if (restore_window_size &&
      g_variant_lookup (state, "window-size", "(ii)", &width, &height) &&
      width > 0 && height > 0)
    gtk_window_set_default_size (GTK_WINDOW (self), width, height);

  return TRUE;
}

static void
ptyxis_tiling_window_surface_needs_attention_vfunc (PtyxisWindow          *window,
                                                    PtyxisTerminalSurface *surface)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (window);

  if (self->split_tree != NULL &&
      ptyxis_split_tree_get_maximized (self->split_tree))
    ptyxis_split_tree_toggle_maximize (self->split_tree);
}

static GtkWindow *
ptyxis_tiling_window_create_preferences_window_vfunc (PtyxisWindow *window)
{
  return g_object_new (PTYXIS_TYPE_TILING_PREFERENCES_WINDOW, NULL);
}

static void
ptyxis_tiling_window_open_new_window_vfunc (PtyxisWindow  *window,
                                            PtyxisProfile *profile,
                                            const char    *container_id)
{
  PtyxisWindow *new_window;

  new_window = ptyxis_application_new_window_for_profile (PTYXIS_APPLICATION_DEFAULT, profile);
  gtk_window_present (GTK_WINDOW (new_window));
}

static GMenu *
ptyxis_tiling_window_create_terminal_menu_vfunc (PtyxisWindow *window)
{
  g_autoptr(GtkBuilder) builder = NULL;
  GMenu *menu;

  builder = gtk_builder_new_from_resource ("/org/gnome/Ptyxis/ptyxis-tiling-terminal-menu.ui");
  menu = G_MENU (gtk_builder_get_object (builder, "terminal_menu"));

  return g_object_ref (menu);
}

static void
ptyxis_tiling_window_split_tree_focused_pane_cb (PtyxisSplitTree    *split_tree,
                                                 GParamSpec         *pspec,
                                                 PtyxisTilingWindow *self)
{
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  g_autoptr(GListModel) profiles = NULL;
  PtyxisTerminalSurface *surface;
  unsigned int n_profiles;

  g_assert (PTYXIS_IS_TILING_WINDOW (self));
  g_assert (PTYXIS_IS_SPLIT_TREE (split_tree));

  surface = ptyxis_window_get_active_surface (PTYXIS_WINDOW (self));

  g_signal_group_set_target (priv->active_surface_signals, surface);
  g_binding_group_set_source (priv->active_surface_bindings, surface);

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.search", surface != NULL);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.set-title", surface != NULL);

  profiles = ptyxis_application_list_profiles (PTYXIS_APPLICATION_DEFAULT);
  n_profiles = g_list_model_get_n_items (profiles);
  gtk_widget_action_set_enabled (GTK_WIDGET (self),
                                 "win.set-profile",
                                 surface != NULL && n_profiles > 1);
}

static void
ptyxis_tiling_window_split_tree_empty_cb (PtyxisSplitTree    *split_tree,
                                          PtyxisTilingWindow *self)
{
  g_assert (PTYXIS_IS_TILING_WINDOW (self));
  g_assert (PTYXIS_IS_SPLIT_TREE (split_tree));

  if (self->split_tree != split_tree)
    return;

  gtk_window_close (GTK_WINDOW (self));
}

static void
ptyxis_tiling_window_split_tree_single_pane_cb (PtyxisSplitTree    *split_tree,
                                                GParamSpec         *pspec,
                                                PtyxisTilingWindow *self)
{
  gboolean single = ptyxis_split_tree_get_n_panes (split_tree) <= 1;

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.toggle-maximize-pane", !single);
}

static void
ptyxis_tiling_window_split_tree_maximized_cb (PtyxisSplitTree    *split_tree,
                                              GParamSpec         *pspec,
                                              PtyxisTilingWindow *self)
{
  gboolean maximized = ptyxis_split_tree_get_maximized (split_tree);

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.split-down", !maximized);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.split-right", !maximized);
}

static void
ptyxis_tiling_window_split_down_action (GtkWidget  *widget,
                                        const char *action_name,
                                        GVariant   *param)
{
  ptyxis_split_tree_split_down (PTYXIS_TILING_WINDOW (widget)->split_tree);
}

static void
ptyxis_tiling_window_split_right_action (GtkWidget  *widget,
                                         const char *action_name,
                                         GVariant   *param)
{
  ptyxis_split_tree_split_right (PTYXIS_TILING_WINDOW (widget)->split_tree);
}

static void
ptyxis_tiling_window_search_action (GtkWidget  *widget,
                                    const char *action_name,
                                    GVariant   *param)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (widget);
  PtyxisPane *pane;

  pane = ptyxis_split_tree_get_focused_pane (self->split_tree);
  if (pane != NULL)
    ptyxis_pane_show_find_bar (pane);
}

static void
ptyxis_tiling_window_detach_and_park_pane (PtyxisTilingWindow *self,
                                           PtyxisPane         *pane)
{
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));

  g_object_ref (pane);

  if (ptyxis_split_tree_detach_pane (self->split_tree, pane))
    ptyxis_parking_lot_push (priv->parking_lot,
                            GTK_WIDGET (pane), ptyxis_pane_get_surface (pane));

  g_object_unref (pane);
}

static void
ptyxis_tiling_window_close_pane_dialog_cb (GObject      *object,
                                           GAsyncResult *result,
                                           gpointer      user_data)
{
  g_autoptr(PtyxisPane) pane = user_data;
  PtyxisTilingWindow *self;

  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_PANE (pane));

  if (!_ptyxis_close_dialog_run_finish (result, NULL))
    return;

  self = PTYXIS_TILING_WINDOW (gtk_widget_get_root (GTK_WIDGET (pane)));
  if (self == NULL)
    return;

  ptyxis_tiling_window_detach_and_park_pane (self, pane);
}

static void
ptyxis_tiling_window_close_pane_action (GtkWidget  *widget,
                                        const char *action_name,
                                        GVariant   *param)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (widget);
  g_autoptr(GPtrArray) surfaces = NULL;
  PtyxisTerminalSurface *surface;
  PtyxisSettings *settings;
  PtyxisPane *pane;

  pane = ptyxis_split_tree_get_focused_pane (self->split_tree);
  if (pane == NULL)
    return;

  surface = ptyxis_pane_get_surface (pane);
  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  if (!ptyxis_terminal_surface_is_running (surface, NULL) ||
      !ptyxis_settings_get_prompt_on_close (settings))
    {
      ptyxis_tiling_window_detach_and_park_pane (self, pane);
      return;
    }

  surfaces = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (surfaces, g_object_ref (surface));
  _ptyxis_close_dialog_run_async (GTK_WINDOW (self),
                                  surfaces,
                                  FALSE,
                                  NULL,
                                  ptyxis_tiling_window_close_pane_dialog_cb,
                                  g_object_ref (pane));
}

static void
ptyxis_tiling_window_undo_close_pane_action (GtkWidget  *widget,
                                             const char *action_name,
                                             GVariant   *param)
{
  PtyxisTilingWindow *self = PTYXIS_TILING_WINDOW (widget);
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  g_autoptr(GtkWidget) popped = NULL;

  if ((popped = ptyxis_parking_lot_pop (priv->parking_lot)))
    {
      PtyxisPane *pane = PTYXIS_PANE (popped);
      PtyxisPane *focused = ptyxis_split_tree_get_focused_pane (self->split_tree);

      if (focused != NULL)
        ptyxis_split_tree_insert_pane (self->split_tree, pane, focused,
                                      GTK_ORIENTATION_HORIZONTAL, TRUE);
      else
        setup_tiling_window (self, ptyxis_split_tree_new_with_pane (pane));

      ptyxis_pane_grab_focus (pane);
    }
}

static void
ptyxis_tiling_window_focus_pane_up_action (GtkWidget  *widget,
                                           const char *action_name,
                                           GVariant   *param)
{
  ptyxis_split_tree_focus_up (PTYXIS_TILING_WINDOW (widget)->split_tree);
}

static void
ptyxis_tiling_window_focus_pane_down_action (GtkWidget  *widget,
                                             const char *action_name,
                                             GVariant   *param)
{
  ptyxis_split_tree_focus_down (PTYXIS_TILING_WINDOW (widget)->split_tree);
}

static void
ptyxis_tiling_window_focus_pane_left_action (GtkWidget  *widget,
                                             const char *action_name,
                                             GVariant   *param)
{
  ptyxis_split_tree_focus_left (PTYXIS_TILING_WINDOW (widget)->split_tree);
}

static void
ptyxis_tiling_window_focus_pane_right_action (GtkWidget  *widget,
                                              const char *action_name,
                                              GVariant   *param)
{
  ptyxis_split_tree_focus_right (PTYXIS_TILING_WINDOW (widget)->split_tree);
}

static void
ptyxis_tiling_window_toggle_maximize_pane_action (GtkWidget  *widget,
                                                  const char *action_name,
                                                  GVariant   *param)
{
  ptyxis_split_tree_toggle_maximize (PTYXIS_TILING_WINDOW (widget)->split_tree);
}

static void
setup_tiling_window (PtyxisTilingWindow *self,
                     PtyxisSplitTree    *split_tree)
{
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  GtkWidget *title_widget;
  PtyxisPane *pane;

  self->split_tree = split_tree;
  gtk_widget_set_vexpand (GTK_WIDGET (split_tree), TRUE);

  g_signal_connect (split_tree, "empty",
                    G_CALLBACK (ptyxis_tiling_window_split_tree_empty_cb), self);
  g_signal_connect (split_tree, "notify::focused-pane",
                    G_CALLBACK (ptyxis_tiling_window_split_tree_focused_pane_cb), self);
  g_signal_connect (split_tree, "notify::single-pane",
                    G_CALLBACK (ptyxis_tiling_window_split_tree_single_pane_cb), self);
  g_signal_connect (split_tree, "notify::maximized",
                    G_CALLBACK (ptyxis_tiling_window_split_tree_maximized_cb), self);

  adw_toast_overlay_set_child (priv->toasts, GTK_WIDGET (split_tree));

  title_widget = adw_header_bar_get_title_widget (priv->header_bar);

  g_object_bind_property_full (split_tree, "title",
                               title_widget, "title",
                               G_BINDING_SYNC_CREATE,
                               _ptyxis_window_bind_title_cb, NULL,
                               NULL, NULL);
  g_object_bind_property_full (split_tree, "title",
                               self, "title",
                               G_BINDING_SYNC_CREATE,
                               _ptyxis_window_bind_title_cb, NULL,
                               NULL, NULL);

  ptyxis_tiling_window_split_tree_focused_pane_cb (self->split_tree, NULL, self);
  ptyxis_tiling_window_split_tree_single_pane_cb (self->split_tree, NULL, self);
  ptyxis_tiling_window_split_tree_maximized_cb (self->split_tree, NULL, self);

  pane = ptyxis_split_tree_get_focused_pane (self->split_tree);
  if (pane != NULL)
    ptyxis_pane_grab_focus (pane);

  gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);
}

static void
ptyxis_tiling_window_dispose (GObject *object)
{
  gtk_widget_dispose_template (GTK_WIDGET (object), PTYXIS_TYPE_TILING_WINDOW);

  G_OBJECT_CLASS (ptyxis_tiling_window_parent_class)->dispose (object);
}

static void
ptyxis_tiling_window_class_init (PtyxisTilingWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  PtyxisWindowClass *window_class = PTYXIS_WINDOW_CLASS (klass);

  object_class->dispose = ptyxis_tiling_window_dispose;

  window_class->get_active_surface = ptyxis_tiling_window_get_active_surface;
  window_class->add_terminal = ptyxis_tiling_window_add_terminal;
  window_class->collect_surfaces = ptyxis_tiling_window_collect_surfaces;
  window_class->focus_by_uuid = ptyxis_tiling_window_focus_by_uuid_vfunc;
  window_class->get_n_pages = ptyxis_tiling_window_get_n_pages_vfunc;
  window_class->serialize = ptyxis_tiling_window_serialize_vfunc;
  window_class->deserialize = ptyxis_tiling_window_deserialize_vfunc;
  window_class->create_terminal_menu = ptyxis_tiling_window_create_terminal_menu_vfunc;
  window_class->open_new_window = ptyxis_tiling_window_open_new_window_vfunc;
  window_class->surface_needs_attention = ptyxis_tiling_window_surface_needs_attention_vfunc;
  window_class->create_preferences_window = ptyxis_tiling_window_create_preferences_window_vfunc;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Ptyxis/ptyxis-tiling-window.ui");

  _ptyxis_window_class_bind_shared_template (widget_class);

  gtk_widget_class_install_action (widget_class, "win.search", NULL, ptyxis_tiling_window_search_action);
  gtk_widget_class_install_action (widget_class, "win.split-down", NULL, ptyxis_tiling_window_split_down_action);
  gtk_widget_class_install_action (widget_class, "win.split-right", NULL, ptyxis_tiling_window_split_right_action);
  gtk_widget_class_install_action (widget_class, "win.close-pane", NULL, ptyxis_tiling_window_close_pane_action);
  gtk_widget_class_install_action (widget_class, "win.undo-close-pane", NULL, ptyxis_tiling_window_undo_close_pane_action);
  gtk_widget_class_install_action (widget_class, "win.focus-pane-up", NULL, ptyxis_tiling_window_focus_pane_up_action);
  gtk_widget_class_install_action (widget_class, "win.focus-pane-down", NULL, ptyxis_tiling_window_focus_pane_down_action);
  gtk_widget_class_install_action (widget_class, "win.focus-pane-left", NULL, ptyxis_tiling_window_focus_pane_left_action);
  gtk_widget_class_install_action (widget_class, "win.focus-pane-right", NULL, ptyxis_tiling_window_focus_pane_right_action);
  gtk_widget_class_install_action (widget_class, "win.toggle-maximize-pane", NULL, ptyxis_tiling_window_toggle_maximize_pane_action);
}

static void
ptyxis_tiling_window_init (PtyxisTilingWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
  _ptyxis_window_init_after_template (PTYXIS_WINDOW (self));

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.toggle-maximize-pane", FALSE);
}

PtyxisTilingWindow *
ptyxis_tiling_window_new (PtyxisProfile *profile)
{
  g_autoptr(PtyxisProfile) default_profile = NULL;
  PtyxisSettings *settings;
  PtyxisTilingWindow *self;
  PtyxisTerminal *terminal;
  PtyxisPane *pane;
  guint32 columns;
  guint32 rows;

  if (profile == NULL)
    {
      default_profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);
      profile = default_profile;
    }

  g_return_val_if_fail (PTYXIS_IS_PROFILE (profile), NULL);

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  self = g_object_new (PTYXIS_TYPE_TILING_WINDOW,
                       "application", PTYXIS_APPLICATION_DEFAULT,
                       NULL);

  setup_tiling_window (self, ptyxis_split_tree_new (profile));

  ptyxis_settings_get_default_size (settings, &columns, &rows);

  if (ptyxis_settings_get_restore_window_size (settings))
    ptyxis_settings_get_window_size (settings, &columns, &rows);

  if (!columns || !rows)
    ptyxis_settings_get_default_size (settings, &columns, &rows);

  pane = ptyxis_split_tree_get_focused_pane (self->split_tree);
  terminal = ptyxis_pane_get_terminal (pane);
  vte_terminal_set_size (VTE_TERMINAL (terminal), columns, rows);

  gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);

  return self;
}

PtyxisTilingWindow *
ptyxis_tiling_window_new_with_pane (PtyxisPane *pane)
{
  PtyxisTilingWindow *self;

  g_return_val_if_fail (PTYXIS_IS_PANE (pane), NULL);

  self = g_object_new (PTYXIS_TYPE_TILING_WINDOW,
                       "application", PTYXIS_APPLICATION_DEFAULT,
                       NULL);

  setup_tiling_window (self, ptyxis_split_tree_new_with_pane (pane));

  return self;
}

PtyxisTilingWindow *
ptyxis_tiling_window_new_from_split_tree (PtyxisSplitTree *split_tree)
{
  PtyxisTilingWindow *self;

  g_return_val_if_fail (PTYXIS_IS_SPLIT_TREE (split_tree), NULL);

  self = g_object_new (PTYXIS_TYPE_TILING_WINDOW,
                       "application", PTYXIS_APPLICATION_DEFAULT,
                       NULL);

  setup_tiling_window (self, split_tree);

  return self;
}

PtyxisSplitTree *
ptyxis_tiling_window_get_split_tree (PtyxisTilingWindow *self)
{
  g_return_val_if_fail (PTYXIS_IS_TILING_WINDOW (self), NULL);

  return self->split_tree;
}

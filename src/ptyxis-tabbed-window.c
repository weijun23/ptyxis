/* ptyxis-tabbed-window.c
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
#include "ptyxis-gated-list-model.h"
#include "ptyxis-menu-row.h"
#include "ptyxis-parking-lot.h"
#include "ptyxis-settings.h"
#include "ptyxis-shrinker.h"
#include "ptyxis-tab-private.h"
#include "ptyxis-preferences-window.h"
#include "ptyxis-tabbed-window.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-util.h"
#include "ptyxis-window-private.h"

struct _PtyxisTabbedWindow
{
  PtyxisWindow          parent_instance;

  AdwTabView           *tab_view;
  AdwTabBar            *tab_bar;
  AdwTabOverview       *tab_overview;
  GMenu                *tab_menu;
  GtkWidget            *tab_overview_button;
  GtkWidget            *new_tab_box;
  GtkButton            *new_terminal_button;
  GtkMenuButton        *new_terminal_menu_button;
  GtkWidget            *new_terminal_separator;
  GtkListView          *menu_list_view;
  GtkSearchEntry       *menu_search;
  GtkStack             *menu_search_stack;
  GtkFilterListModel   *maybe_containers;
  GtkCustomFilter      *maybe_containers_filter;
  GtkFilterListModel   *not_containers;
  GtkCustomFilter      *not_containers_filter;

  GBindingGroup        *active_tab_bindings;
  GSignalGroup         *selected_page_signals;
  guint                 focus_active_tab_source;

  guint                 tab_overview_animating : 1;
  guint                 reset_nonvisible_from_size_allocate : 1;
};

G_DEFINE_FINAL_TYPE (PtyxisTabbedWindow, ptyxis_tabbed_window, PTYXIS_TYPE_WINDOW)

enum {
  PROP_0,
  PROP_ACTIVE_TAB,
  N_PROPS
};

static GParamSpec *tabbed_properties[N_PROPS];

static void
ptyxis_tabbed_window_setup_page (PtyxisTabbedWindow *self,
                                 PtyxisTab          *tab,
                                 AdwTabPage         *page)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (PTYXIS_IS_TAB (tab));
  g_assert (ADW_IS_TAB_PAGE (page));

  g_object_bind_property (tab, "title", page, "title", G_BINDING_SYNC_CREATE);
  g_object_bind_property (tab, "icon", page, "icon", G_BINDING_SYNC_CREATE);
  g_object_bind_property (tab, "indicator-icon", page, "indicator-icon", G_BINDING_SYNC_CREATE);
}

static PtyxisTerminalSurface *
ptyxis_tabbed_window_get_active_surface (PtyxisWindow *window)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  AdwTabPage *page;

  if (self->tab_view == NULL)
    return NULL;

  if (!(page = adw_tab_view_get_selected_page (self->tab_view)))
    return NULL;

  return ptyxis_tab_get_surface (PTYXIS_TAB (adw_tab_page_get_child (page)));
}

static PtyxisTerminalSurface *
ptyxis_tabbed_window_add_terminal (PtyxisWindow  *window,
                                   PtyxisProfile *profile)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  PtyxisTab *tab;
  AdwTabPage *page;

  tab = ptyxis_tab_new (profile);

  page = adw_tab_view_append (self->tab_view, GTK_WIDGET (tab));

  ptyxis_tabbed_window_setup_page (self, tab, page);

  ptyxis_tab_grab_focus (tab);

  return ptyxis_tab_get_surface (tab);
}

static void
ptyxis_tabbed_window_collect_surfaces (PtyxisWindow *window,
                                       GPtrArray    *surfaces)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  g_autoptr(GtkSelectionModel) model = NULL;
  guint n_items;

  if (self->tab_view == NULL)
    return;

  model = adw_tab_view_get_pages (self->tab_view);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (model));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (G_LIST_MODEL (model), i);
      PtyxisTab *tab = PTYXIS_TAB (adw_tab_page_get_child (page));
      PtyxisTerminalSurface *surface = ptyxis_tab_get_surface (tab);

      if (ptyxis_terminal_surface_is_running (surface, NULL))
        g_ptr_array_add (surfaces, g_object_ref (surface));
    }
}

static void
ptyxis_tabbed_window_open_new_window_vfunc (PtyxisWindow  *window,
                                            PtyxisProfile *profile,
                                            const char    *container_id)
{
  PtyxisWindow *new_window;
  PtyxisTerminalSurface *surface;
  PtyxisSettings *settings;
  GdkToplevel *toplevel;
  GdkToplevelState state;
  guint columns, rows;

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  new_window = ptyxis_application_new_window_empty (PTYXIS_APPLICATION_DEFAULT);
  surface = ptyxis_window_add_terminal (new_window, profile);
  _ptyxis_window_apply_current_settings_to_surface (window, surface, NULL);

  if (!ptyxis_str_empty0 (container_id))
    {
      g_autoptr(PtyxisIpcContainer) container = NULL;

      if ((container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, container_id)))
        ptyxis_terminal_surface_set_container (surface, container);
    }

  toplevel = GDK_TOPLEVEL (gtk_native_get_surface (GTK_NATIVE (window)));
  state = gdk_toplevel_get_state (toplevel);

  if (!!(state & (GDK_TOPLEVEL_STATE_MAXIMIZED | GDK_TOPLEVEL_STATE_FULLSCREEN | GDK_TOPLEVEL_STATE_TILED)) ||
      !ptyxis_settings_get_restore_window_size (settings))
    {
      PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (surface);
      ptyxis_settings_get_default_size (settings, &columns, &rows);
      vte_terminal_set_size (VTE_TERMINAL (terminal), columns, rows);
    }

  gtk_window_present (GTK_WINDOW (new_window));

  ptyxis_terminal_surface_grab_focus (surface);
}

static void
ptyxis_tabbed_window_surface_needs_attention_vfunc (PtyxisWindow          *window,
                                                    PtyxisTerminalSurface *surface)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  g_autoptr(GtkSelectionModel) model = NULL;
  guint n_items;

  if (self->tab_view == NULL)
    return;

  model = adw_tab_view_get_pages (self->tab_view);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (model));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (G_LIST_MODEL (model), i);
      PtyxisTab *tab = PTYXIS_TAB (adw_tab_page_get_child (page));

      if (ptyxis_tab_get_surface (tab) == surface)
        {
          ptyxis_tab_set_needs_attention (tab, TRUE);
          return;
        }
    }
}

static GtkWindow *
ptyxis_tabbed_window_create_preferences_window_vfunc (PtyxisWindow *window)
{
  return ptyxis_preferences_window_new (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT));
}

static gboolean
ptyxis_tabbed_window_resize_for_zoom_vfunc (PtyxisWindow *window)
{
  return TRUE;
}

static gboolean
ptyxis_tabbed_window_focus_by_uuid_vfunc (PtyxisWindow *window,
                                          const char   *uuid)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  g_autoptr(GtkSelectionModel) model = NULL;
  guint n_items;

  if (self->tab_view == NULL)
    return FALSE;

  model = adw_tab_view_get_pages (self->tab_view);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (model));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (G_LIST_MODEL (model), i);
      PtyxisTab *tab = PTYXIS_TAB (adw_tab_page_get_child (page));

      if (g_strcmp0 (uuid, ptyxis_tab_get_uuid (tab)) == 0)
        {
          ptyxis_tabbed_window_set_active_tab (self, tab);
          gtk_window_present (GTK_WINDOW (self));
          ptyxis_tab_grab_focus (tab);
          return TRUE;
        }
    }

  return FALSE;
}

static guint
ptyxis_tabbed_window_get_n_pages_vfunc (PtyxisWindow *window)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  g_autoptr(GListModel) pages = NULL;

  if (self->tab_view == NULL)
    return 0;

  pages = ptyxis_tabbed_window_list_pages (self);

  return g_list_model_get_n_items (pages);
}

static GVariant *
ptyxis_tabbed_window_serialize_vfunc (PtyxisWindow *window)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  PtyxisSettings *settings;
  gboolean restore_session;
  g_autoptr(GListModel) pages = NULL;
  GVariantBuilder builder;
  guint n_items;

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  restore_session = ptyxis_settings_get_restore_session (settings);

  pages = ptyxis_tabbed_window_list_pages (self);
  n_items = g_list_model_get_n_items (pages);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("{sv}"));
  g_variant_builder_add (&builder, "s", "tabs");
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("v"));
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("aa{sv}"));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (pages, i);
      gboolean pinned;

      g_assert (ADW_IS_TAB_PAGE (page));

      pinned = adw_tab_page_get_pinned (page);

      if (restore_session || pinned)
        {
          PtyxisTab *tab = PTYXIS_TAB (adw_tab_page_get_child (page));
          PtyxisTerminalSurface *surface;
          gboolean is_active;

          surface = ptyxis_tab_get_surface (tab);
          is_active = ptyxis_tabbed_window_get_active_tab (self) == tab;

          g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));
          ptyxis_terminal_surface_serialize (surface, &builder);
          g_variant_builder_add_parsed (&builder, "{'pinned', <%b>}", pinned);
          g_variant_builder_add_parsed (&builder, "{'active', <%b>}", is_active);
          g_variant_builder_close (&builder);
        }
    }

  g_variant_builder_close (&builder);
  g_variant_builder_close (&builder);
  g_variant_builder_close (&builder);

  return g_variant_builder_end (&builder);
}

static gboolean
ptyxis_tabbed_window_deserialize_vfunc (PtyxisWindow *window,
                                        GVariant     *state)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (window);
  g_autoptr(GVariant) tabs = NULL;
  g_autoptr(GVariant) tab = NULL;
  PtyxisSettings *settings;
  PtyxisTab *active_tab = NULL;
  GVariantIter tab_iter;
  gboolean restore_session;
  gboolean restore_window_size;
  gboolean maximized;

  tabs = g_variant_lookup_value (state, "tabs", G_VARIANT_TYPE ("aa{sv}"));
  if (tabs == NULL || g_variant_n_children (tabs) == 0)
    return FALSE;

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  restore_session = ptyxis_settings_get_restore_session (settings);
  restore_window_size = ptyxis_settings_get_restore_window_size (settings);

  if (!g_variant_lookup (state, "maximized", "b", &maximized))
    maximized = FALSE;

  g_variant_iter_init (&tab_iter, tabs);
  while (g_variant_iter_loop (&tab_iter, "@a{sv}", &tab))
    {
      g_autoptr(PtyxisProfile) profile = NULL;
      PtyxisTerminalSurface *surface;
      const char *profile_id;
      PtyxisTab *the_tab;
      gboolean is_active;
      gboolean pinned;

      if (!g_variant_lookup (tab, "pinned", "b", &pinned))
        pinned = FALSE;

      if (!pinned && !restore_session)
        continue;

      if (!g_variant_lookup (tab, "profile", "&s", &profile_id))
        profile_id = NULL;

      if (!g_variant_lookup (tab, "active", "b", &is_active))
        is_active = FALSE;

      if (!ptyxis_str_empty0 (profile_id))
        profile = ptyxis_application_dup_profile (PTYXIS_APPLICATION_DEFAULT, profile_id);

      if (profile == NULL)
        profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);

      the_tab = ptyxis_tab_new (profile);
      surface = ptyxis_tab_get_surface (the_tab);
      ptyxis_terminal_surface_deserialize (surface, tab);

      if (!restore_window_size || maximized)
        {
          PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (surface);
          uint32_t columns;
          uint32_t rows;

          ptyxis_settings_get_default_size (settings, &columns, &rows);
          vte_terminal_set_size (VTE_TERMINAL (terminal), columns, rows);
        }

      ptyxis_tabbed_window_add_tab_at_end (self, the_tab);
      ptyxis_tabbed_window_set_tab_pinned (self, the_tab, pinned);

      if (is_active)
        active_tab = the_tab;
    }

  if (!restore_session)
    {
      g_autoptr(PtyxisProfile) profile = NULL;
      PtyxisTab *the_tab;

      profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);
      the_tab = ptyxis_tab_new (profile);
      ptyxis_tabbed_window_add_tab_at_end (self, the_tab);

      if (!active_tab)
        active_tab = the_tab;
    }

  if (active_tab)
    {
      ptyxis_tabbed_window_set_active_tab (self, active_tab);
      ptyxis_tab_grab_focus (active_tab);
    }

  return TRUE;
}

static void
ptyxis_tabbed_window_close_page_dialog_cb (GObject      *object,
                                           GAsyncResult *result,
                                           gpointer      user_data)
{
  g_autoptr(PtyxisTab) tab = user_data;
  g_autoptr(GError) error = NULL;
  PtyxisTabbedWindow *self;
  AdwTabPage *page;

  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_TAB (tab));

  self = PTYXIS_TABBED_WINDOW (gtk_widget_get_ancestor (GTK_WIDGET (tab), PTYXIS_TYPE_TABBED_WINDOW));
  page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab));

  if (!_ptyxis_close_dialog_run_finish (result, &error))
    {
      adw_tab_view_close_page_finish (self->tab_view, page, FALSE);
      return;
    }

  ptyxis_parking_lot_push (_ptyxis_window_get_priv (PTYXIS_WINDOW (self))->parking_lot,
                           GTK_WIDGET (tab), ptyxis_tab_get_surface (tab));

  _ptyxis_tab_ignore_snapshot (tab);

  adw_tab_view_close_page_finish (self->tab_view, page, TRUE);

  if (adw_tab_view_get_n_pages (self->tab_view) == 1)
    gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);
}

static gboolean
ptyxis_tabbed_window_close_page_cb (PtyxisTabbedWindow *self,
                                    AdwTabPage         *tab_page,
                                    AdwTabView         *tab_view)
{
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  g_autoptr(GPtrArray) surfaces = NULL;
  PtyxisTerminalSurface *surface;
  PtyxisSettings *settings;
  PtyxisTab *tab;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_PAGE (tab_page));
  g_assert (ADW_IS_TAB_VIEW (tab_view));

  ptyxis_window_save_size (PTYXIS_WINDOW (self));

  tab = PTYXIS_TAB (adw_tab_page_get_child (tab_page));
  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  if (priv->disposed)
    return GDK_EVENT_PROPAGATE;

  if (!ptyxis_tab_is_running (tab, NULL) ||
      !ptyxis_settings_get_prompt_on_close (settings))
    {
      ptyxis_parking_lot_push (priv->parking_lot,
                               GTK_WIDGET (tab), ptyxis_tab_get_surface (tab));
      gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);
      return GDK_EVENT_PROPAGATE;
    }

  surfaces = g_ptr_array_new_with_free_func (g_object_unref);
  surface = ptyxis_tab_get_surface (tab);
  g_ptr_array_add (surfaces, g_object_ref (surface));

  _ptyxis_close_dialog_run_async (GTK_WINDOW (self),
                                  surfaces,
                                  FALSE,
                                  NULL,
                                  ptyxis_tabbed_window_close_page_dialog_cb,
                                  g_object_ref (tab));

  return GDK_EVENT_STOP;
}

static PtyxisTab *
ptyxis_tabbed_window_get_tab_at_tab_bar_point (PtyxisTabbedWindow *self,
                                               graphene_point_t   *point)
{
  static GType tab_type = G_TYPE_INVALID;
  g_autoptr(AdwTabPage) page = NULL;
  GtkWidget *pick;
  GtkWidget *child;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (point != NULL);

  if (tab_type == G_TYPE_INVALID)
    {
      tab_type = g_type_from_name ("AdwTab");
      g_assert (tab_type != G_TYPE_INVALID);
    }

  if (!(pick = gtk_widget_pick (GTK_WIDGET (self->tab_bar), point->x, point->y, GTK_PICK_DEFAULT)))
    return NULL;

  if (!G_TYPE_CHECK_INSTANCE_TYPE (pick, tab_type))
    {
      if (!(pick = gtk_widget_get_ancestor (pick, tab_type)))
        return NULL;
    }

  g_object_get (pick, "page", &page, NULL);
  g_assert (!page || ADW_IS_TAB_PAGE (page));

  if (page == NULL ||
      !(child = adw_tab_page_get_child (page)) ||
      !PTYXIS_IS_TAB (child))
    return NULL;

  return PTYXIS_TAB (child);
}

static void
ptyxis_tabbed_window_tab_bar_click_pressed_cb (PtyxisTabbedWindow *self,
                                               guint               n_press,
                                               double              x,
                                               double              y,
                                               GtkGestureClick    *click)
{
  PtyxisTabMiddleClickBehavior behavior;
  PtyxisSettings *settings;
  PtyxisTab *tab;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (GTK_IS_GESTURE_CLICK (click));

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  behavior = ptyxis_settings_get_tab_middle_click (settings);

  if (behavior == PTYXIS_TAB_MIDDLE_CLICK_CLOSE)
    return;

  if (!(tab = ptyxis_tabbed_window_get_tab_at_tab_bar_point (self, &GRAPHENE_POINT_INIT (x, y))))
    return;

  ptyxis_tabbed_window_set_active_tab (self, tab);

  if (behavior == PTYXIS_TAB_MIDDLE_CLICK_PASTE)
    {
      PtyxisTerminal *terminal = ptyxis_tab_get_terminal (tab);

      if (ptyxis_terminal_can_paste (terminal))
        ptyxis_terminal_paste (terminal);
    }

  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
}

static gboolean
ptyxis_tabbed_window_focus_active_tab_cb (gpointer data)
{
  PtyxisTabbedWindow *self = data;
  PtyxisTab *active_tab;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  self->focus_active_tab_source = 0;
  self->tab_overview_animating = FALSE;

  if ((active_tab = ptyxis_tabbed_window_get_active_tab (self)))
    {
      ptyxis_tab_grab_focus (active_tab);
      gtk_widget_queue_resize (GTK_WIDGET (active_tab));
    }

  return G_SOURCE_REMOVE;
}

static void
ptyxis_tabbed_window_tab_overview_notify_open_cb (PtyxisTabbedWindow *self,
                                                  GParamSpec         *pspec,
                                                  AdwTabOverview     *tab_overview)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_OVERVIEW (tab_overview));

  g_clear_handle_id (&self->focus_active_tab_source, g_source_remove);

  if (!adw_tab_overview_get_open (tab_overview))
    {
      PtyxisTab *active_tab;
      GtkSettings *settings = gtk_settings_get_default ();
      gboolean gtk_enable_animations = TRUE;
      guint delay_msec = 425;

      g_object_get (settings,
                    "gtk-enable-animations", &gtk_enable_animations,
                    NULL);

      if (!gtk_enable_animations)
        delay_msec = 10;

      self->focus_active_tab_source = g_timeout_add_full (G_PRIORITY_LOW,
                                                          delay_msec,
                                                          ptyxis_tabbed_window_focus_active_tab_cb,
                                                          self, NULL);

      if ((active_tab = ptyxis_tabbed_window_get_active_tab (self)))
        ptyxis_tab_grab_focus (active_tab);
    }

  self->tab_overview_animating = TRUE;
}

static void
ptyxis_tabbed_window_setup_menu_cb (PtyxisTabbedWindow *self,
                                    AdwTabPage         *page,
                                    AdwTabView         *view)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (!page || ADW_IS_TAB_PAGE (page));
  g_assert (ADW_IS_TAB_VIEW (view));

  if (page != NULL)
    adw_tab_view_set_selected_page (view, page);
}

static AdwTabView *
ptyxis_tabbed_window_create_window_cb (PtyxisTabbedWindow *self,
                                       AdwTabView         *tab_view)
{
  PtyxisTabbedWindow *other;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  other = g_object_new (PTYXIS_TYPE_TABBED_WINDOW,
                        "application", PTYXIS_APPLICATION_DEFAULT,
                        NULL);

  gtk_window_present (GTK_WINDOW (other));

  return other->tab_view;
}

static void
ptyxis_tabbed_window_update_visible_and_maybe_close (PtyxisTabbedWindow *self)
{
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  gboolean visible;
  gboolean was_visible;
  guint n_pages;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (priv->disposed || self->tab_view == NULL)
    return;

  n_pages = adw_tab_view_get_n_pages (self->tab_view);

  if (n_pages == 0 && !adw_tab_view_get_is_transferring_page (self->tab_view))
    {
      if (!priv->in_close_request)
        ptyxis_application_save_session (PTYXIS_APPLICATION_DEFAULT);
      gtk_window_destroy (GTK_WINDOW (self));
      return;
    }

  was_visible = gtk_widget_get_visible (GTK_WIDGET (self->tab_bar));
  visible = !priv->single_terminal_mode && n_pages > 1;

  if (visible != was_visible)
    {
      gtk_widget_set_visible (GTK_WIDGET (self->tab_bar), visible);
      gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);

      if (visible)
        gtk_widget_add_css_class (GTK_WIDGET (self), "has-tab-bar");
      else
        gtk_widget_remove_css_class (GTK_WIDGET (self), "has-tab-bar");
    }
}

static void
ptyxis_tabbed_window_page_attached_cb (PtyxisTabbedWindow *self,
                                       AdwTabPage         *page,
                                       int                 position,
                                       AdwTabView         *tab_view)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_PAGE (page));
  g_assert (ADW_IS_TAB_VIEW (tab_view));

  ptyxis_tabbed_window_update_visible_and_maybe_close (self);
}

static void
ptyxis_tabbed_window_page_detached_cb (PtyxisTabbedWindow *self,
                                       AdwTabPage         *page,
                                       int                 position,
                                       AdwTabView         *tab_view)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_PAGE (page));
  g_assert (ADW_IS_TAB_VIEW (tab_view));

  ptyxis_tabbed_window_update_visible_and_maybe_close (self);
}

static void
ptyxis_tabbed_window_notify_selected_page_cb (PtyxisTabbedWindow *self,
                                              GParamSpec         *pspec,
                                              AdwTabView         *tab_view)
{
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  g_autoptr(GPropertyAction) read_only = NULL;
  g_autoptr(GListModel) profiles = NULL;
  PtyxisTerminal *terminal = NULL;
  AdwTabPage *page = NULL;
  PtyxisTab *tab = NULL;
  guint n_profiles;
  gboolean has_page = FALSE;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_VIEW (tab_view));

  if (self->tab_view != NULL)
    page = adw_tab_view_get_selected_page (self->tab_view);

  g_signal_group_set_target (self->selected_page_signals, page);

  if (page != NULL)
    {
      PtyxisTerminalSurface *surface;

      tab = PTYXIS_TAB (adw_tab_page_get_child (page));
      surface = ptyxis_tab_get_surface (tab);

      has_page = TRUE;

      terminal = ptyxis_tab_get_terminal (tab);

      g_signal_group_set_target (priv->active_surface_signals, surface);
      g_binding_group_set_source (priv->active_surface_bindings, surface);

      read_only = g_property_action_new ("tab.read-only", tab, "read-only");

      adw_tab_page_set_needs_attention (page, FALSE);

      ptyxis_tab_grab_focus (tab);
    }
  else
    {
      g_signal_group_set_target (priv->active_surface_signals, NULL);
      g_binding_group_set_source (priv->active_surface_bindings, NULL);
    }

  if (terminal == NULL)
    {
      gtk_revealer_set_reveal_child (priv->find_bar_revealer, FALSE);
      gtk_window_set_title (GTK_WINDOW (self), ptyxis_app_name ());
    }

  ptyxis_find_bar_set_terminal (priv->find_bar, terminal);

  if (!has_page)
    {
      gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.zoom-in", FALSE);
      gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.zoom-out", FALSE);
      gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.zoom-one", FALSE);
    }

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.search", has_page);

  profiles = ptyxis_application_list_profiles (PTYXIS_APPLICATION_DEFAULT);
  n_profiles = g_list_model_get_n_items (profiles);
  gtk_widget_action_set_enabled (GTK_WIDGET (self),
                                 "win.set-profile",
                                 has_page && n_profiles > 1);

  g_action_map_remove_action (G_ACTION_MAP (self), "tab.read-only");
  if (read_only != NULL)
    g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (read_only));

  g_binding_group_set_source (self->active_tab_bindings, tab);

  g_object_notify_by_pspec (G_OBJECT (self), tabbed_properties[PROP_ACTIVE_TAB]);

  ptyxis_fullscreen_box_reveal (priv->fullscreen_box);
}

static AdwTabPage *
ptyxis_tabbed_window_tab_overview_create_tab_cb (PtyxisTabbedWindow *self,
                                                 AdwTabOverview     *tab_overview)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_OVERVIEW (tab_overview));

  gtk_widget_activate_action (GTK_WIDGET (tab_overview), "win.new-tab", "(ss)", "", "");

  return adw_tab_view_get_selected_page (self->tab_view);
}

static void
ptyxis_tabbed_window_close_action (GtkWidget  *widget,
                                   const char *action_name,
                                   GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  PtyxisTab *tab;
  AdwTabPage *tab_page;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (!(tab = ptyxis_tabbed_window_get_active_tab (self)))
    return;

  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab));
  adw_tab_view_close_page (self->tab_view, tab_page);
}

static void
ptyxis_tabbed_window_close_others_action (GtkWidget  *widget,
                                          const char *action_name,
                                          GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  PtyxisTab *tab;
  AdwTabPage *tab_page;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (!(tab = ptyxis_tabbed_window_get_active_tab (self)))
    return;

  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab));
  adw_tab_view_close_other_pages (self->tab_view, tab_page);
}

static void
ptyxis_tabbed_window_detach_action (GtkWidget  *widget,
                                    const char *action_name,
                                    GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  PtyxisTabbedWindow *new_window;
  PtyxisTab *tab;
  AdwTabPage *tab_page;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (!(tab = ptyxis_tabbed_window_get_active_tab (self)))
    return;

  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab));

  new_window = g_object_new (PTYXIS_TYPE_TABBED_WINDOW,
                             "application", PTYXIS_APPLICATION_DEFAULT,
                             NULL);
  adw_tab_view_transfer_page (self->tab_view, tab_page, new_window->tab_view, 0);

  gtk_window_present (GTK_WINDOW (new_window));
}

static void
ptyxis_tabbed_window_tab_pin_action (GtkWidget  *widget,
                                     const char *action_name,
                                     GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  AdwTabPage *tab_page;
  PtyxisTab *tab;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (!(tab = ptyxis_tabbed_window_get_active_tab (self)) ||
      !(tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab))))
    return;

  adw_tab_view_set_page_pinned (self->tab_view, tab_page, TRUE);
}

static void
ptyxis_tabbed_window_tab_unpin_action (GtkWidget  *widget,
                                       const char *action_name,
                                       GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  AdwTabPage *tab_page;
  PtyxisTab *tab;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (!(tab = ptyxis_tabbed_window_get_active_tab (self)) ||
      !(tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab))))
    return;

  adw_tab_view_set_page_pinned (self->tab_view, tab_page, FALSE);
}

static void
ptyxis_tabbed_window_page_previous_action (GtkWidget  *widget,
                                           const char *action_name,
                                           GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  guint n_pages = adw_tab_view_get_n_pages (self->tab_view);

  if (n_pages == 0)
    return;

  if (!adw_tab_view_select_previous_page (self->tab_view))
    adw_tab_view_set_selected_page (self->tab_view,
                                    adw_tab_view_get_nth_page (self->tab_view, n_pages - 1));
}

static void
ptyxis_tabbed_window_page_next_action (GtkWidget  *widget,
                                       const char *action_name,
                                       GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  guint n_pages = adw_tab_view_get_n_pages (self->tab_view);

  if (n_pages == 0)
    return;

  if (!adw_tab_view_select_next_page (self->tab_view))
    adw_tab_view_set_selected_page (self->tab_view,
                                    adw_tab_view_get_nth_page (self->tab_view, 0));
}

static void
ptyxis_tabbed_window_tab_focus_action (GtkWidget  *widget,
                                       const char *action_name,
                                       GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  guint nth = 0;
  guint n_pages;
  int position;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE_INT32));

  position = g_variant_get_int32 (param);
  n_pages = adw_tab_view_get_n_pages (self->tab_view);

  if (n_pages == 0)
    return;

  if (position <= 0)
    nth = n_pages - 1;
  else if (position > 0 && (guint)position <= n_pages)
    nth = (guint)position - 1;
  else
    nth = n_pages - 1;

  if (nth < n_pages)
    {
      AdwTabPage *page = adw_tab_view_get_nth_page (self->tab_view, nth);
      adw_tab_view_set_selected_page (self->tab_view, page);
    }
}

static void
ptyxis_tabbed_window_move_left_action (GtkWidget  *widget,
                                       const char *action_name,
                                       GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  PtyxisTab *tab;
  AdwTabPage *tab_page;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (!(tab = ptyxis_tabbed_window_get_active_tab (self)))
    return;

  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab));
  adw_tab_view_reorder_backward (self->tab_view, tab_page);
  ptyxis_tab_raise (tab);
  ptyxis_tab_grab_focus (tab);
}

static void
ptyxis_tabbed_window_move_right_action (GtkWidget  *widget,
                                        const char *action_name,
                                        GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  PtyxisTab *tab;
  AdwTabPage *tab_page;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if (!(tab = ptyxis_tabbed_window_get_active_tab (self)))
    return;

  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab));
  adw_tab_view_reorder_forward (self->tab_view, tab_page);
  ptyxis_tab_raise (tab);
  ptyxis_tab_grab_focus (tab);
}

static void
ptyxis_tabbed_window_tab_overview_action (GtkWidget  *widget,
                                          const char *action_name,
                                          GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  adw_tab_overview_set_open (self->tab_overview,
                             !adw_tab_overview_get_open (self->tab_overview));
}

static void
ptyxis_tabbed_window_tab_menu_action (GtkWidget  *widget,
                                      const char *action_name,
                                      GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  gtk_menu_button_popup (self->new_terminal_menu_button);
}

static void
ptyxis_tabbed_window_undo_close_tab_action (GtkWidget  *widget,
                                            const char *action_name,
                                            GVariant   *param)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  g_autoptr(GtkWidget) popped = NULL;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  if ((popped = ptyxis_parking_lot_pop (priv->parking_lot)))
    {
      PtyxisTab *tab = PTYXIS_TAB (popped);

      if (!ptyxis_tab_is_running (tab, NULL))
        ptyxis_tab_show_banner (tab);
      ptyxis_tabbed_window_add_tab (self, tab);
      ptyxis_tabbed_window_set_active_tab (self, tab);
      ptyxis_tab_grab_focus (tab);
    }
}

static void
ptyxis_tabbed_window_selected_page_notify_pinned_cb (PtyxisTabbedWindow *self,
                                                     GParamSpec         *pspec,
                                                     AdwTabPage         *page)
{
  gboolean pinned;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_PAGE (page));

  pinned = adw_tab_page_get_pinned (page);

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "tab.pin", !pinned);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "tab.unpin", pinned);
}

static void
ptyxis_tabbed_window_selected_page_bind_cb (PtyxisTabbedWindow *self,
                                            AdwTabPage         *page,
                                            GSignalGroup       *group)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (ADW_IS_TAB_PAGE (page));
  g_assert (G_IS_SIGNAL_GROUP (group));

  ptyxis_tabbed_window_selected_page_notify_pinned_cb (self, NULL, page);
}

static gboolean
ptyxis_tabbed_window_check_singular (gpointer instance,
                                     guint    n_items)
{
  return n_items <= 1;
}

static char *
ptyxis_tabbed_window_get_header_title (gpointer instance,
                                       gpointer item)
{
  if (PTYXIS_IPC_IS_CONTAINER (item))
    {
      const char *str = ptyxis_ipc_container_get_display_name (item);

      if (str && str[0])
        return g_strdup (_("Containers"));

      return NULL;
    }

  if (PTYXIS_IS_PROFILE (item))
    return g_strdup (_("Profiles"));

  return NULL;
}

static char *
ptyxis_tabbed_window_get_container_title (gpointer first,
                                          gpointer second)
{
  PtyxisIpcContainer *container;
  const char *title;

  g_assert (PTYXIS_IPC_IS_CONTAINER (first) || PTYXIS_IPC_IS_CONTAINER (second));

  if (PTYXIS_IPC_IS_CONTAINER (first))
    container = first;
  else if (PTYXIS_IPC_IS_CONTAINER (second))
    container = second;
  else
    g_assert_not_reached ();

  title = ptyxis_ipc_container_get_display_name (container);

  if (title && title[0])
    return g_strdup (title);

  return g_strdup (_("My Computer"));
}

static void
ptyxis_tabbed_window_menu_stop_search (PtyxisTabbedWindow *self,
                                       GtkSearchEntry     *entry)
{
  PtyxisTab *tab;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (GTK_IS_SEARCH_ENTRY (entry));

  gtk_editable_set_text (GTK_EDITABLE (entry), "");
  gtk_menu_button_popdown (self->new_terminal_menu_button);

  if ((tab = ptyxis_tabbed_window_get_active_tab (self)))
    ptyxis_tab_grab_focus (tab);
}

static void
ptyxis_tabbed_window_menu_activate_cb (PtyxisTabbedWindow *self,
                                       guint               position,
                                       GtkListView        *list_view)
{
  GtkSelectionModel *model;
  g_autoptr(GObject) item = NULL;
  PtyxisTab *tab;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (GTK_IS_LIST_VIEW (list_view));

  model = gtk_list_view_get_model (list_view);
  item = g_list_model_get_item (G_LIST_MODEL (model), position);

  gtk_menu_button_popdown (self->new_terminal_menu_button);
  gtk_editable_set_text (GTK_EDITABLE (self->menu_search), "");

  if ((tab = ptyxis_tabbed_window_get_active_tab (self)))
    ptyxis_tab_grab_focus (tab);

  if (PTYXIS_IS_PROFILE (item))
    {
      const char *uuid = ptyxis_profile_get_uuid (PTYXIS_PROFILE (item));

      gtk_widget_activate_action (GTK_WIDGET (self), "win.new-terminal", "(ss)", uuid, "");
    }
  else if (PTYXIS_IPC_IS_CONTAINER (item))
    {
      const char *id = ptyxis_ipc_container_get_id (PTYXIS_IPC_CONTAINER (item));

      gtk_widget_activate_action (GTK_WIDGET (self), "win.new-terminal", "(ss)", "", id);
    }
  else
    {
      g_warning ("Unknown object type %s", G_OBJECT_TYPE_NAME (item));
    }
}

static void
ptyxis_tabbed_window_menu_search_activate_cb (PtyxisTabbedWindow *self,
                                              GtkSearchEntry     *entry)
{
  GtkSelectionModel *model;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (GTK_IS_SEARCH_ENTRY (entry));

  model = gtk_list_view_get_model (self->menu_list_view);

  if (g_list_model_get_n_items (G_LIST_MODEL (model)) > 0)
    ptyxis_tabbed_window_menu_activate_cb (self, 0, self->menu_list_view);
}

static void
ptyxis_tabbed_window_notify_menu_n_items_cb (PtyxisTabbedWindow *self,
                                             GParamSpec         *pspec,
                                             GListModel         *model)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (G_IS_LIST_MODEL (model));

  if (g_list_model_get_n_items (model) == 0)
    gtk_stack_set_visible_child_name (self->menu_search_stack, "empty");
  else
    gtk_stack_set_visible_child_name (self->menu_search_stack, "results");
}

static gboolean
nonempty_display_name (gpointer item,
                       gpointer user_data)
{
  const char *str = ptyxis_ipc_container_get_display_name (item);

  return str && str[0];
}

static gboolean
empty_display_name (gpointer item,
                    gpointer user_data)
{
  const char *str = ptyxis_ipc_container_get_display_name (item);

  return !str || !str[0];
}

static void
ptyxis_tabbed_window_bind_section_title (PtyxisTabbedWindow       *self,
                                         GtkListHeader            *header,
                                         GtkSignalListItemFactory *factory)
{
  const char *str;
  GtkWidget *child;
  GObject *item;
  gboolean visible = TRUE;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (GTK_IS_LIST_HEADER (header));
  g_assert (GTK_IS_SIGNAL_LIST_ITEM_FACTORY (factory));

  if (!(child = gtk_list_header_get_child (header)))
    {
      child = gtk_label_new (NULL);
      gtk_label_set_xalign (GTK_LABEL (child), .5);
      gtk_widget_add_css_class (child, "dimmed");
      gtk_widget_add_css_class (child, "title");
      gtk_list_header_set_child (header, child);
    }

  item = gtk_list_header_get_item (header);

  if (PTYXIS_IS_PROFILE (item))
    gtk_label_set_label (GTK_LABEL (child), _("Profiles"));
  else if (PTYXIS_IPC_IS_CONTAINER (item) &&
           (str = ptyxis_ipc_container_get_display_name (PTYXIS_IPC_CONTAINER (item))) &&
           (str && str[0]))
    gtk_label_set_label (GTK_LABEL (child), _("Containers"));
  else
    visible = FALSE;

  gtk_widget_set_visible (child, visible);
}

static void
ptyxis_tabbed_window_update_menu_visibility (PtyxisTabbedWindow *self)
{
  g_autoptr(GListModel) containers = NULL;
  g_autoptr(GListModel) profiles = NULL;
  gboolean visible;
  guint n_profiles;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  containers = ptyxis_application_list_containers (PTYXIS_APPLICATION_DEFAULT);
  profiles = ptyxis_application_list_profiles (PTYXIS_APPLICATION_DEFAULT);
  n_profiles = g_list_model_get_n_items (profiles);
  visible = g_list_model_get_n_items (containers) > 1 ||
            n_profiles > 1;

  gtk_widget_action_set_enabled (GTK_WIDGET (self),
                                 "my-computer",
                                 g_list_model_get_n_items (containers) > 1);

  gtk_widget_action_set_enabled (GTK_WIDGET (self),
                                 "win.set-profile",
                                 n_profiles > 1);

  if (self->new_terminal_separator)
    gtk_widget_set_visible (GTK_WIDGET (self->new_terminal_separator), visible);
  if (self->new_terminal_menu_button)
    gtk_widget_set_visible (GTK_WIDGET (self->new_terminal_menu_button), visible);
}

static void
ptyxis_tabbed_window_notify_decoration_layout_cb (PtyxisTabbedWindow *self,
                                                  GParamSpec         *pspec,
                                                  GtkSettings        *gtk_settings)
{
  g_autofree char *layout = NULL;
  gboolean inverted = FALSE;
  const char *colon;
  const char *close_;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (GTK_IS_SETTINGS (gtk_settings));

  g_object_get (gtk_settings,
                "gtk-decoration-layout", &layout,
                NULL);

  if ((colon = strchr (layout, ':')) && (close_ = strstr (layout, "close")))
    inverted = close_ < colon;

  if (self->tab_bar)
    adw_tab_bar_set_inverted (self->tab_bar, inverted);
}

static void
ptyxis_tabbed_window_shortcuts_notify_cb (PtyxisTabbedWindow *self,
                                          GParamSpec         *pspec,
                                          PtyxisShortcuts    *shortcuts)
{
  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (PTYXIS_IS_SHORTCUTS (shortcuts));

  if (self->tab_menu)
    ptyxis_shortcuts_update_menu (shortcuts, self->tab_menu);
}

static void
ptyxis_tabbed_window_toplevel_state_changed_cb (PtyxisTabbedWindow *self,
                                                GParamSpec         *pspec,
                                                GdkToplevel        *toplevel)
{
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  GdkToplevelState state;
  gboolean is_fullscreen;
  gboolean is_maximized;

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));
  g_assert (GDK_IS_TOPLEVEL (toplevel));

  state = gdk_toplevel_get_state (toplevel);
  is_fullscreen = !!(state & GDK_TOPLEVEL_STATE_FULLSCREEN);
  is_maximized = !!(state & GDK_TOPLEVEL_STATE_MAXIMIZED);

  if ((!is_maximized && priv->is_maximized) ||
      (!is_fullscreen && priv->is_fullscreen))
    self->reset_nonvisible_from_size_allocate = TRUE;
}

static void
ptyxis_tabbed_window_size_allocate (GtkWidget *widget,
                                    int        width,
                                    int        height,
                                    int        baseline)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);

  g_assert (PTYXIS_IS_TABBED_WINDOW (self));

  GTK_WIDGET_CLASS (ptyxis_tabbed_window_parent_class)->size_allocate (widget, width, height, baseline);

  if (self->reset_nonvisible_from_size_allocate)
    {
      g_autoptr(GtkSelectionModel) model = adw_tab_view_get_pages (self->tab_view);
      guint n_pages = g_list_model_get_n_items (G_LIST_MODEL (model));

      for (guint i = 0; i < n_pages; i++)
        {
          g_autoptr(AdwTabPage) page = g_list_model_get_item (G_LIST_MODEL (model), i);
          PtyxisTab *tab = PTYXIS_TAB (adw_tab_page_get_child (page));
          PtyxisTerminal *terminal = ptyxis_tab_get_terminal (tab);

          if (page == adw_tab_view_get_selected_page (self->tab_view))
            continue;

          ptyxis_terminal_reset_for_size (terminal);
        }

      self->reset_nonvisible_from_size_allocate = FALSE;
    }
}

static void
ptyxis_tabbed_window_realize (GtkWidget *widget)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (widget);
  GdkToplevel *toplevel;

  GTK_WIDGET_CLASS (ptyxis_tabbed_window_parent_class)->realize (widget);

  toplevel = GDK_TOPLEVEL (gtk_native_get_surface (GTK_NATIVE (self)));

  g_signal_connect_object (toplevel,
                           "notify::state",
                           G_CALLBACK (ptyxis_tabbed_window_toplevel_state_changed_cb),
                           self,
                           G_CONNECT_SWAPPED);
}

static void
ptyxis_tabbed_window_constructed (GObject *object)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (object);
  PtyxisWindowPrivate *priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));
  g_autoptr(GListModel) profiles = NULL;
  g_autoptr(GListModel) containers = NULL;
  GtkSettings *gtk_settings;

  G_OBJECT_CLASS (ptyxis_tabbed_window_parent_class)->constructed (object);

  containers = ptyxis_application_list_containers (PTYXIS_APPLICATION_DEFAULT);
  g_signal_connect_object (containers,
                           "items-changed",
                           G_CALLBACK (ptyxis_tabbed_window_update_menu_visibility),
                           self,
                           G_CONNECT_SWAPPED);
  profiles = ptyxis_application_list_profiles (PTYXIS_APPLICATION_DEFAULT);
  g_signal_connect_object (profiles,
                           "items-changed",
                           G_CALLBACK (ptyxis_tabbed_window_update_menu_visibility),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tabbed_window_update_menu_visibility (self);

  gtk_settings = gtk_settings_get_default ();
  g_signal_connect_object (gtk_settings,
                           "notify::gtk-decoration-layout",
                           G_CALLBACK (ptyxis_tabbed_window_notify_decoration_layout_cb),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tabbed_window_notify_decoration_layout_cb (self, NULL, gtk_settings);

  g_signal_connect_object (priv->shortcuts,
                           "notify",
                           G_CALLBACK (ptyxis_tabbed_window_shortcuts_notify_cb),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tabbed_window_shortcuts_notify_cb (self, NULL, priv->shortcuts);
}

static void
ptyxis_tabbed_window_dispose (GObject *object)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (object);

  g_clear_handle_id (&self->focus_active_tab_source, g_source_remove);

  if (self->active_tab_bindings)
    {
      g_binding_group_set_source (self->active_tab_bindings, NULL);
      g_clear_object (&self->active_tab_bindings);
    }

  if (self->selected_page_signals)
    {
      g_signal_group_set_target (self->selected_page_signals, NULL);
      g_clear_object (&self->selected_page_signals);
    }

  gtk_widget_dispose_template (GTK_WIDGET (self), PTYXIS_TYPE_TABBED_WINDOW);

  G_OBJECT_CLASS (ptyxis_tabbed_window_parent_class)->dispose (object);
}

static void
ptyxis_tabbed_window_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (object);

  switch (prop_id)
    {
    case PROP_ACTIVE_TAB:
      g_value_set_object (value, ptyxis_tabbed_window_get_active_tab (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_tabbed_window_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  PtyxisTabbedWindow *self = PTYXIS_TABBED_WINDOW (object);

  switch (prop_id)
    {
    case PROP_ACTIVE_TAB:
      ptyxis_tabbed_window_set_active_tab (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static GMenu *
ptyxis_tabbed_window_create_terminal_menu_vfunc (PtyxisWindow *window)
{
  g_autoptr(GtkBuilder) builder = NULL;
  GMenu *menu;

  builder = gtk_builder_new_from_resource ("/org/gnome/Ptyxis/ptyxis-tabbed-terminal-menu.ui");
  menu = G_MENU (gtk_builder_get_object (builder, "terminal_menu"));

  return g_object_ref (menu);
}

static void
ptyxis_tabbed_window_class_init (PtyxisTabbedWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  PtyxisWindowClass *window_class = PTYXIS_WINDOW_CLASS (klass);

  object_class->constructed = ptyxis_tabbed_window_constructed;
  object_class->dispose = ptyxis_tabbed_window_dispose;
  object_class->get_property = ptyxis_tabbed_window_get_property;
  object_class->set_property = ptyxis_tabbed_window_set_property;

  widget_class->size_allocate = ptyxis_tabbed_window_size_allocate;
  widget_class->realize = ptyxis_tabbed_window_realize;

  window_class->get_active_surface = ptyxis_tabbed_window_get_active_surface;
  window_class->add_terminal = ptyxis_tabbed_window_add_terminal;
  window_class->collect_surfaces = ptyxis_tabbed_window_collect_surfaces;
  window_class->focus_by_uuid = ptyxis_tabbed_window_focus_by_uuid_vfunc;
  window_class->get_n_pages = ptyxis_tabbed_window_get_n_pages_vfunc;
  window_class->serialize = ptyxis_tabbed_window_serialize_vfunc;
  window_class->deserialize = ptyxis_tabbed_window_deserialize_vfunc;
  window_class->open_new_window = ptyxis_tabbed_window_open_new_window_vfunc;
  window_class->resize_for_zoom = ptyxis_tabbed_window_resize_for_zoom_vfunc;
  window_class->surface_needs_attention = ptyxis_tabbed_window_surface_needs_attention_vfunc;
  window_class->create_preferences_window = ptyxis_tabbed_window_create_preferences_window_vfunc;
  window_class->create_terminal_menu = ptyxis_tabbed_window_create_terminal_menu_vfunc;

  tabbed_properties[PROP_ACTIVE_TAB] =
    g_param_spec_object ("active-tab", NULL, NULL,
                         PTYXIS_TYPE_TAB,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, tabbed_properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Ptyxis/ptyxis-tabbed-window.ui");

  _ptyxis_window_class_bind_shared_template (widget_class);

  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, maybe_containers);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, maybe_containers_filter);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, menu_list_view);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, menu_search);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, menu_search_stack);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, new_tab_box);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, new_terminal_button);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, new_terminal_menu_button);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, new_terminal_separator);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, not_containers);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, not_containers_filter);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, tab_bar);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, tab_menu);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, tab_overview);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, tab_overview_button);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTabbedWindow, tab_view);

  /* Keep the callback names as ptyxis_window_* to match the UI file handler attributes */
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_bind_section_title);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_check_singular);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_get_container_title);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_get_header_title);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_menu_activate_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_menu_search_activate_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_menu_stop_search);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_notify_menu_n_items_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_page_attached_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_page_detached_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_notify_selected_page_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_create_window_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_close_page_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_setup_menu_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_tab_overview_notify_open_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tabbed_window_tab_overview_create_tab_cb);

  gtk_widget_class_install_action (widget_class, "win.tab-menu", NULL, ptyxis_tabbed_window_tab_menu_action);
  gtk_widget_class_install_action (widget_class, "win.tab-overview", NULL, ptyxis_tabbed_window_tab_overview_action);
  gtk_widget_class_install_action (widget_class, "win.undo-close-tab", NULL, ptyxis_tabbed_window_undo_close_tab_action);
  gtk_widget_class_install_action (widget_class, "page.move-left", NULL, ptyxis_tabbed_window_move_left_action);
  gtk_widget_class_install_action (widget_class, "page.move-right", NULL, ptyxis_tabbed_window_move_right_action);
  gtk_widget_class_install_action (widget_class, "page.close", NULL, ptyxis_tabbed_window_close_action);
  gtk_widget_class_install_action (widget_class, "page.close-others", NULL, ptyxis_tabbed_window_close_others_action);
  gtk_widget_class_install_action (widget_class, "page.detach", NULL, ptyxis_tabbed_window_detach_action);
  gtk_widget_class_install_action (widget_class, "tab.pin", NULL, ptyxis_tabbed_window_tab_pin_action);
  gtk_widget_class_install_action (widget_class, "tab.unpin", NULL, ptyxis_tabbed_window_tab_unpin_action);
  gtk_widget_class_install_action (widget_class, "tab.focus", "i", ptyxis_tabbed_window_tab_focus_action);
  gtk_widget_class_install_action (widget_class, "page.next", NULL, ptyxis_tabbed_window_page_next_action);
  gtk_widget_class_install_action (widget_class, "page.previous", NULL, ptyxis_tabbed_window_page_previous_action);

  g_type_ensure (PTYXIS_TYPE_GATED_LIST_MODEL);
  g_type_ensure (PTYXIS_TYPE_MENU_ROW);
  g_type_ensure (PTYXIS_TYPE_SHRINKER);
}

static void
ptyxis_tabbed_window_init (PtyxisTabbedWindow *self)
{
  g_autoptr(GIcon) default_icon = NULL;
  GtkGestureClick *click;

  gtk_widget_init_template (GTK_WIDGET (self));
  _ptyxis_window_init_after_template (PTYXIS_WINDOW (self));

  self->active_tab_bindings = g_binding_group_new ();
  self->selected_page_signals = g_signal_group_new (ADW_TYPE_TAB_PAGE);

  g_signal_connect_object (self->selected_page_signals,
                           "bind",
                           G_CALLBACK (ptyxis_tabbed_window_selected_page_bind_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->selected_page_signals,
                                 "notify::pinned",
                                 G_CALLBACK (ptyxis_tabbed_window_selected_page_notify_pinned_cb),
                                 self,
                                 G_CONNECT_SWAPPED);

  default_icon = g_themed_icon_new ("utilities-terminal-symbolic");
  adw_tab_view_set_default_icon (self->tab_view, default_icon);

  adw_tab_view_set_shortcuts (self->tab_view, 0);

  g_binding_group_bind_full (self->active_tab_bindings, "title",
                             self, "title",
                             G_BINDING_SYNC_CREATE,
                             _ptyxis_window_bind_title_cb, NULL, NULL, NULL);

  click = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 2);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect_object (click,
                           "pressed",
                           G_CALLBACK (ptyxis_tabbed_window_tab_bar_click_pressed_cb),
                           self,
                           G_CONNECT_SWAPPED);
  gtk_widget_add_controller (GTK_WIDGET (self->tab_bar),
                             GTK_EVENT_CONTROLLER (g_steal_pointer (&click)));

  gtk_custom_filter_set_filter_func (self->not_containers_filter,
                                     empty_display_name, NULL, NULL);
  gtk_custom_filter_set_filter_func (self->maybe_containers_filter,
                                     nonempty_display_name, NULL, NULL);

}

PtyxisTabbedWindow *
ptyxis_tabbed_window_new (void)
{
  return g_object_new (PTYXIS_TYPE_TABBED_WINDOW,
                       "application", PTYXIS_APPLICATION_DEFAULT,
                       NULL);
}

PtyxisTabbedWindow *
ptyxis_tabbed_window_new_for_profile (PtyxisProfile *profile)
{
  g_autoptr(PtyxisProfile) default_profile = NULL;
  PtyxisSettings *settings;
  PtyxisTabbedWindow *self;
  PtyxisTerminalSurface *surface;
  PtyxisTerminal *terminal;
  guint columns = 0;
  guint rows = 0;

  if (profile == NULL)
    {
      default_profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);
      profile = default_profile;
    }

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  self = g_object_new (PTYXIS_TYPE_TABBED_WINDOW,
                       "application", PTYXIS_APPLICATION_DEFAULT,
                       NULL);

  surface = ptyxis_window_add_terminal (PTYXIS_WINDOW (self), profile);
  terminal = ptyxis_terminal_surface_get_terminal (surface);

  ptyxis_settings_get_default_size (settings, &columns, &rows);

  if (ptyxis_settings_get_restore_window_size (settings))
    ptyxis_settings_get_window_size (settings, &columns, &rows);

  if (!columns || !rows)
    ptyxis_settings_get_default_size (settings, &columns, &rows);

  vte_terminal_set_size (VTE_TERMINAL (terminal), columns, rows);

  gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);

  return self;
}

PtyxisTabbedWindow *
ptyxis_tabbed_window_new_for_command (PtyxisProfile      *profile,
                                     const char * const *argv,
                                     const char         *cwd_uri)
{
  g_autoptr(PtyxisProfile) default_profile = NULL;
  PtyxisWindowPrivate *priv;
  PtyxisTabbedWindow *self;
  PtyxisSettings *settings;
  PtyxisTerminal *terminal;
  PtyxisTab *tab;
  GApplicationFlags flags;
  guint columns = 0;
  guint rows = 0;

  g_return_val_if_fail (!profile || PTYXIS_IS_PROFILE (profile), NULL);
  g_return_val_if_fail (argv != NULL, NULL);
  g_return_val_if_fail (argv[0] != NULL, NULL);

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  ptyxis_settings_get_default_size (settings, &columns, &rows);

  if (profile == NULL)
    {
      default_profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);
      profile = default_profile;
    }

  g_assert (PTYXIS_IS_PROFILE (profile));

  self = g_object_new (PTYXIS_TYPE_TABBED_WINDOW,
                       "application", PTYXIS_APPLICATION_DEFAULT,
                       NULL);
  priv = _ptyxis_window_get_priv (PTYXIS_WINDOW (self));

  tab = ptyxis_tab_new (profile);
  terminal = ptyxis_tab_get_terminal (tab);

  if (ptyxis_settings_get_restore_window_size (settings))
    ptyxis_settings_get_window_size (settings, &columns, &rows);

  if (!columns || !rows)
    ptyxis_settings_get_default_size (settings, &columns, &rows);

  vte_terminal_set_size (VTE_TERMINAL (terminal), columns, rows);

  if (argv != NULL && argv[0] != NULL)
    {
      ptyxis_tab_set_command (tab, argv);
      gtk_window_set_title (GTK_WINDOW (self), argv[0]);
    }

  if (!ptyxis_str_empty0 (cwd_uri))
    ptyxis_tab_set_previous_working_directory_uri (tab, cwd_uri);

  ptyxis_tabbed_window_append_tab (self, tab);

  gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);

  flags = g_application_get_flags (G_APPLICATION (PTYXIS_APPLICATION_DEFAULT));
  priv->single_terminal_mode = !!(flags & G_APPLICATION_NON_UNIQUE);

  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.new-tab", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.new-window", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.new-terminal", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.tab-overview", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "page.move-left", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "page.move-right", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "page.close-others", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "page.detach", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "tab.pin", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "tab.unpin", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "page.next", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "page.previous", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.undo-close-tab", FALSE);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "my-computer", FALSE);

  gtk_widget_set_visible (GTK_WIDGET (self->tab_bar), FALSE);
  gtk_widget_set_visible (self->new_tab_box, FALSE);
  gtk_widget_set_visible (self->tab_overview_button, FALSE);

  return self;
}

void
ptyxis_tabbed_window_add_tab (PtyxisTabbedWindow *self,
                              PtyxisTab          *tab)
{
  PtyxisNewTabPosition new_tab_position;
  PtyxisApplication *app;
  PtyxisSettings *settings;
  AdwTabPage *page;
  int position = 0;

  g_return_if_fail (PTYXIS_IS_TABBED_WINDOW (self));
  g_return_if_fail (PTYXIS_IS_TAB (tab));

  app = PTYXIS_APPLICATION_DEFAULT;
  settings = ptyxis_application_get_settings (app);
  new_tab_position = ptyxis_settings_get_new_tab_position (settings);

  if ((page = adw_tab_view_get_selected_page (self->tab_view)))
    {
      position = adw_tab_view_get_page_position (self->tab_view, page);

      switch (new_tab_position)
        {
        case PTYXIS_NEW_TAB_POSITION_NEXT:
          position++;
          break;

        case PTYXIS_NEW_TAB_POSITION_LAST:
          position = adw_tab_view_get_n_pages (self->tab_view);
          break;

        default:
          g_assert_not_reached ();
        }
    }

  page = adw_tab_view_insert (self->tab_view, GTK_WIDGET (tab), position);

  ptyxis_tabbed_window_setup_page (self, tab, page);

  if (adw_tab_view_get_n_pages (self->tab_view) == 2)
    gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);
}

void
ptyxis_tabbed_window_add_tab_at_end (PtyxisTabbedWindow *self,
                                     PtyxisTab          *tab)
{
  AdwTabPage *page;
  int position;

  g_return_if_fail (PTYXIS_IS_TABBED_WINDOW (self));
  g_return_if_fail (PTYXIS_IS_TAB (tab));

  position = adw_tab_view_get_n_pages (self->tab_view);
  page = adw_tab_view_insert (self->tab_view, GTK_WIDGET (tab), position);

  ptyxis_tabbed_window_setup_page (self, tab, page);

  if (adw_tab_view_get_n_pages (self->tab_view) == 2)
    gtk_window_set_default_size (GTK_WINDOW (self), -1, -1);
}

void
ptyxis_tabbed_window_append_tab (PtyxisTabbedWindow *self,
                                 PtyxisTab          *tab)
{
  AdwTabPage *page;

  g_return_if_fail (PTYXIS_IS_TABBED_WINDOW (self));
  g_return_if_fail (PTYXIS_IS_TAB (tab));

  page = adw_tab_view_append (self->tab_view, GTK_WIDGET (tab));

  ptyxis_tabbed_window_setup_page (self, tab, page);

  ptyxis_tab_grab_focus (tab);
}

PtyxisTab *
ptyxis_tabbed_window_get_active_tab (PtyxisTabbedWindow *self)
{
  AdwTabPage *page;

  g_return_val_if_fail (PTYXIS_IS_TABBED_WINDOW (self), NULL);

  if (self->tab_view == NULL)
    return NULL;

  if (!(page = adw_tab_view_get_selected_page (self->tab_view)))
    return NULL;

  return PTYXIS_TAB (adw_tab_page_get_child (page));
}

void
ptyxis_tabbed_window_set_active_tab (PtyxisTabbedWindow *self,
                                     PtyxisTab          *tab)
{
  AdwTabPage *page;

  g_return_if_fail (PTYXIS_IS_TABBED_WINDOW (self));
  g_return_if_fail (!tab || PTYXIS_IS_TAB (tab));

  if (self->tab_view == NULL || tab == NULL)
    return;

  if (!(page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab))))
    return;

  adw_tab_view_set_selected_page (self->tab_view, page);
}

GListModel *
ptyxis_tabbed_window_list_pages (PtyxisTabbedWindow *self)
{
  g_return_val_if_fail (PTYXIS_IS_TABBED_WINDOW (self), NULL);

  return G_LIST_MODEL (adw_tab_view_get_pages (self->tab_view));
}

gboolean
ptyxis_tabbed_window_focus_tab_by_uuid (PtyxisTabbedWindow *self,
                                        const char         *uuid)
{
  g_autoptr(GtkSelectionModel) model = NULL;
  guint n_items;

  g_return_val_if_fail (PTYXIS_IS_TABBED_WINDOW (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  model = adw_tab_view_get_pages (self->tab_view);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (model));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (G_LIST_MODEL (model), i);
      PtyxisTab *tab = PTYXIS_TAB (adw_tab_page_get_child (page));

      g_debug ("Window has tab \"%s\"", ptyxis_tab_get_uuid (tab));

      if (0 == g_strcmp0 (uuid, ptyxis_tab_get_uuid (tab)))
        {
          ptyxis_tabbed_window_set_active_tab (self, tab);
          gtk_window_present (GTK_WINDOW (self));
          ptyxis_tab_grab_focus (tab);
          return TRUE;
        }
    }

  return FALSE;
}

gboolean
ptyxis_tabbed_window_is_animating (PtyxisTabbedWindow *self)
{
  g_return_val_if_fail (PTYXIS_IS_TABBED_WINDOW (self), FALSE);

  return self->tab_overview_animating;
}

void
ptyxis_tabbed_window_set_tab_pinned (PtyxisTabbedWindow *self,
                                     PtyxisTab          *tab,
                                     gboolean            pinned)
{
  AdwTabPage *tab_page;

  g_return_if_fail (PTYXIS_IS_TABBED_WINDOW (self));
  g_return_if_fail (PTYXIS_IS_TAB (tab));

  if ((tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (tab))))
    adw_tab_view_set_page_pinned (self->tab_view, tab_page, pinned);
}

/*
 * ptyxis-tab.c
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

#include <glib/gi18n.h>

#include <cairo.h>

#include "ptyxis-application.h"
#include "ptyxis-enums.h"
#include "ptyxis-inspector.h"
#include "ptyxis-surface-monitor.h"
#include "ptyxis-notify.h"
#include "ptyxis-tab-private.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-util.h"
#include "ptyxis-window.h"

struct _PtyxisTab
{
  GtkWidget              parent_instance;

  PtyxisTerminalSurface *surface;         /* (owned) (not nullable) */
  PtyxisProfile         *profile;          /* (owned) (not nullable) */
  PtyxisSurfaceMonitor  *monitor;          /* (owned) (nullable) */
  GtkBox                *box;              /* (unowned) (not nullable) - template child */
  AdwBanner             *banner;           /* (unowned) (not nullable) - template child */
  GdkTexture            *cached_texture;   /* (owned) (nullable) */
  char                  *uuid;             /* (owned) (not nullable) */
  PtyxisNotify           notify;

  unsigned int           inhibit_cookie;
  unsigned int           ignore_snapshot : 1;
};

enum {
  PROP_0,
  PROP_COMMAND_LINE,
  PROP_DISABLE_DYNAMIC_TITLE,
  PROP_ICON,
  PROP_IGNORE_OSC_TITLE,
  PROP_INDICATOR_ICON,
  PROP_PROCESS_LEADER_KIND,
  PROP_PROFILE,
  PROP_PROGRESS,
  PROP_PROGRESS_FRACTION,
  PROP_READ_ONLY,
  PROP_SUBTITLE,
  PROP_TITLE,
  PROP_TITLE_PREFIX,
  PROP_UUID,
  PROP_ZOOM,
  PROP_ZOOM_LABEL,
  N_PROPS
};

enum {
  BELL,
  COMMIT,
  N_SIGNALS
};

G_DEFINE_FINAL_TYPE (PtyxisTab, ptyxis_tab, GTK_TYPE_WIDGET)

PtyxisTerminalSurface *
ptyxis_tab_get_surface (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->surface;
}

static GParamSpec *properties[N_PROPS];
static unsigned int signals[N_SIGNALS];

static gboolean
ptyxis_tab_is_active (PtyxisTab *self)
{
  GtkWidget *window;

  g_assert (PTYXIS_IS_TAB (self));

  if ((window = gtk_widget_get_ancestor (GTK_WIDGET (self), PTYXIS_TYPE_WINDOW)))
    return ptyxis_window_get_active_tab (PTYXIS_WINDOW (window)) == self;

  return FALSE;
}

static void
ptyxis_tab_update_inhibit (PtyxisTab *self)
{
  PtyxisSettings *settings;
  const char *command_line;
  gboolean inhibit = FALSE;
  GtkWidget *window;

  g_assert (PTYXIS_IS_TAB (self));

  if (self->surface == NULL)
    return;

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  if (!ptyxis_settings_get_inhibit_logout (settings))
    {
      if (self->inhibit_cookie)
        {
          gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                     self->inhibit_cookie);
          self->inhibit_cookie = 0;
        }

      return;
    }

  command_line = ptyxis_terminal_surface_get_command_line (self->surface);

  if (command_line != NULL)
    {
      const char *space = strchr (command_line, ' ');

      if (space != NULL)
        {
          g_autofree char *program_name = g_strndup (command_line, space - command_line);

          if (!ptyxis_is_shell (program_name))
            inhibit = TRUE;
        }
    }

  if ((inhibit && self->inhibit_cookie != 0) ||
      (!inhibit && self->inhibit_cookie == 0))
    return;

  window = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);

  if (inhibit)
    {
      if (window != NULL)
        {
          self->inhibit_cookie =
            gtk_application_inhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                     GTK_WINDOW (window),
                                     GTK_APPLICATION_INHIBIT_LOGOUT,
                                     _("A foreground process is running"));
        }
    }
  else
    {
      gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                 self->inhibit_cookie);
      self->inhibit_cookie = 0;
    }
}

/*
 * Surface signal handlers
 */

static void
ptyxis_tab_surface_notify_user_cb (PtyxisTab             *self,
                                   const char            *title,
                                   const char            *button_label,
                                   const char            *action_name,
                                   GVariant              *action_target,
                                   PtyxisTerminalSurface *surface)
{
  g_assert (PTYXIS_IS_TAB (self));

  if (self->banner == NULL)
    return;

  if (title == NULL)
    {
      gtk_widget_set_visible (GTK_WIDGET (self->banner), FALSE);
      return;
    }

  adw_banner_set_title (self->banner, title);
  adw_banner_set_button_label (self->banner, button_label);

  if (g_strcmp0 (action_name, "surface.respawn") == 0)
    action_name = "tab.respawn";

  if (action_target != NULL)
    gtk_actionable_set_action_target_value (GTK_ACTIONABLE (self->banner), action_target);

  gtk_actionable_set_action_name (GTK_ACTIONABLE (self->banner), action_name);
  gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);
}

static void
ptyxis_tab_surface_request_close_cb (PtyxisTab             *self,
                                     PtyxisTerminalSurface *surface)
{
  GtkWidget *tab_view;
  AdwTabPage *page;

  g_assert (PTYXIS_IS_TAB (self));

  tab_view = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW);

  if (!ADW_IS_TAB_VIEW (tab_view))
    return;

  page = adw_tab_view_get_page (ADW_TAB_VIEW (tab_view), GTK_WIDGET (self));

  if (!ADW_IS_TAB_PAGE (page))
    return;

  if (adw_tab_page_get_pinned (page))
    adw_tab_view_set_page_pinned (ADW_TAB_VIEW (tab_view), page, /* pinned = */ FALSE);

  adw_tab_view_close_page (ADW_TAB_VIEW (tab_view), page);
}

static void
ptyxis_tab_surface_bell_cb (PtyxisTab             *self,
                            PtyxisTerminalSurface *surface)
{
  g_assert (PTYXIS_IS_TAB (self));

  g_signal_emit (self, signals[BELL], 0);
}

static void
ptyxis_tab_surface_commit_cb (PtyxisTab             *self,
                              const char            *str,
                              PtyxisTerminalSurface *surface)
{
  g_assert (PTYXIS_IS_TAB (self));

  g_signal_emit (self, signals[COMMIT], 0, str);
}

/*
 * Surface property relay handler
 */

static void
ptyxis_tab_surface_notify_cb (PtyxisTab             *self,
                              GParamSpec            *pspec,
                              PtyxisTerminalSurface *surface)
{
  const char *name;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));

  name = g_param_spec_get_name (pspec);

  if (strcmp (name, "title") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
  else if (strcmp (name, "subtitle") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUBTITLE]);
    }
  else if (strcmp (name, "icon") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ICON]);
    }
  else if (strcmp (name, "command-line") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMMAND_LINE]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
      ptyxis_tab_update_inhibit (self);
    }
  else if (strcmp (name, "process-leader-kind") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROCESS_LEADER_KIND]);

      if (!ptyxis_tab_is_active (self))
        ptyxis_tab_set_needs_attention (self, TRUE);
    }
  else if (strcmp (name, "progress") == 0 ||
           strcmp (name, "progress-fraction") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROGRESS]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROGRESS_FRACTION]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_INDICATOR_ICON]);
    }
  else if (strcmp (name, "zoom") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM_LABEL]);
    }
  else if (strcmp (name, "zoom-label") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM_LABEL]);
    }
  else if (strcmp (name, "ignore-osc-title") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_IGNORE_OSC_TITLE]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
  else if (strcmp (name, "disable-dynamic-title") == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DISABLE_DYNAMIC_TITLE]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
  else if (strcmp (name, "state") == 0)
    {
      ptyxis_tab_update_inhibit (self);
    }
}

/*
 * UI event handlers
 */

static void
ptyxis_tab_notify_contains_focus_cb (PtyxisTab               *self,
                                     GParamSpec              *pspec,
                                     GtkEventControllerFocus *focus)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (GTK_IS_EVENT_CONTROLLER_FOCUS (focus));

  if (!gtk_event_controller_focus_contains_focus (focus))
    return;

  ptyxis_tab_set_needs_attention (self, FALSE);
  g_application_withdraw_notification (G_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                       self->uuid);
}

static void
ptyxis_tab_invalidate_thumbnail (PtyxisTab *self)
{
  GtkWidget *view;
  AdwTabPage *page;

  g_assert (PTYXIS_IS_TAB (self));

  g_clear_object (&self->cached_texture);

  gtk_widget_queue_draw (GTK_WIDGET (self));

  if ((view = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW)) &&
      (page = adw_tab_view_get_page (ADW_TAB_VIEW (view), GTK_WIDGET (self))))
    adw_tab_page_invalidate_thumbnail (page);
}

static void
ptyxis_tab_notify_palette_cb (PtyxisTab      *self,
                              GParamSpec     *pspec,
                              PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  ptyxis_tab_invalidate_thumbnail (self);
}

static void
ptyxis_tab_snapshot (GtkWidget   *widget,
                     GtkSnapshot *snapshot)
{
  PtyxisTab *self = (PtyxisTab *)widget;
  PtyxisTerminal *terminal;
  PtyxisWindow *window;
  GdkRGBA bg;
  gboolean animating;
  int width;
  int height;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (GTK_IS_SNAPSHOT (snapshot));

  if (self->ignore_snapshot)
    return;

  terminal = ptyxis_terminal_surface_get_terminal (self->surface);
  window = PTYXIS_WINDOW (gtk_widget_get_root (widget));
  animating = ptyxis_window_is_animating (window);
  width = gtk_widget_get_width (widget);
  height = gtk_widget_get_height (widget);

  vte_terminal_get_color_background_for_draw (VTE_TERMINAL (terminal), &bg);

  if (animating &&
      ptyxis_window_get_active_tab (window) == self)
    {
      if (self->cached_texture == NULL)
        {
          int scale_factor = gtk_widget_get_scale_factor (widget);
          g_autoptr(GskRenderNode) node = NULL;
          graphene_matrix_t matrix;
          GtkSnapshot *sub_snapshot;
          GskRenderer *renderer;

          sub_snapshot = gtk_snapshot_new ();
          gtk_snapshot_scale (sub_snapshot, scale_factor, scale_factor);
          gtk_snapshot_append_color (sub_snapshot,
                                     &bg,
                                     &GRAPHENE_RECT_INIT (0, 0, width, height));

          if (gtk_widget_compute_transform (GTK_WIDGET (terminal),
                                            GTK_WIDGET (self),
                                            &matrix))
            {
              gtk_snapshot_transform_matrix (sub_snapshot, &matrix);
              GTK_WIDGET_GET_CLASS (terminal)->snapshot (GTK_WIDGET (terminal), sub_snapshot);
            }

          node = gtk_snapshot_free_to_node (sub_snapshot);
          renderer = gtk_native_get_renderer (GTK_NATIVE (window));

          self->cached_texture = gsk_renderer_render_texture (renderer,
                                                              node,
                                                              &GRAPHENE_RECT_INIT (0,
                                                                                   0,
                                                                                   width * scale_factor,
                                                                                   height * scale_factor));
        }

      gtk_snapshot_append_texture (snapshot,
                                   self->cached_texture,
                                   &GRAPHENE_RECT_INIT (0, 0, width, height));
    }
  else
    {
      g_clear_object (&self->cached_texture);

      if (animating)
        gtk_snapshot_append_color (snapshot,
                                   &bg,
                                   &GRAPHENE_RECT_INIT (0, 0, width, height));

      GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->snapshot (widget, snapshot);
    }
}

static void
ptyxis_tab_size_allocate (GtkWidget *widget,
                          int        width,
                          int        height,
                          int        baseline)
{
  PtyxisTab *self = (PtyxisTab *)widget;

  g_assert (PTYXIS_IS_TAB (self));

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->size_allocate (widget, width, height, baseline);

  g_clear_object (&self->cached_texture);
}

static void
ptyxis_tab_root (GtkWidget *widget)
{
  PtyxisTab *self = PTYXIS_TAB (widget);

  self->ignore_snapshot = FALSE;

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->root (widget);
}

static void
ptyxis_tab_unroot (GtkWidget *widget)
{
  PtyxisTab *self = PTYXIS_TAB (widget);

  if (self->inhibit_cookie != 0)
    {
      gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                 self->inhibit_cookie);
      self->inhibit_cookie = 0;
    }

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->unroot (widget);
}

/*
 * Actions
 */

static void
ptyxis_tab_respawn_action (GtkWidget  *widget,
                           const char *action_name,
                           GVariant   *params)
{
  PtyxisTab *self = (PtyxisTab *)widget;

  g_assert (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_respawn (self->surface);
}

static void
ptyxis_tab_inspect_action (GtkWidget  *widget,
                           const char *action_name,
                           GVariant   *params)
{
  PtyxisTab *self = (PtyxisTab *)widget;
  PtyxisInspector *inspector;
  GtkRoot *root;

  g_assert (PTYXIS_IS_TAB (self));

  inspector = ptyxis_inspector_new (self);
  root = gtk_widget_get_root (GTK_WIDGET (self));

  gtk_window_set_transient_for (GTK_WINDOW (inspector), GTK_WINDOW (root));
  gtk_window_set_modal (GTK_WINDOW (inspector), /* modal = */ FALSE);
  gtk_window_present (GTK_WINDOW (inspector));
}

/*
 *
 * GObject vfuncs
 */

static void
ptyxis_tab_constructed (GObject *object)
{
  PtyxisTab *self = (PtyxisTab *)object;
  PtyxisSettings *settings;
  GtkScrolledWindow *scrolled_window;
  PtyxisTerminal *terminal;

  G_OBJECT_CLASS (ptyxis_tab_parent_class)->constructed (object);

  /* Create the terminal surface */
  self->surface = ptyxis_terminal_surface_new (self->profile);

  /* Embed the surface's scrolled window into our box */
  scrolled_window = ptyxis_terminal_surface_get_scrolled_window (self->surface);
  gtk_box_append (self->box, GTK_WIDGET (scrolled_window));

  /* Connect surface signals */
  g_signal_connect_object (self->surface, "notify-user",
                           G_CALLBACK (ptyxis_tab_surface_notify_user_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->surface, "request-close",
                           G_CALLBACK (ptyxis_tab_surface_request_close_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->surface, "bell",
                           G_CALLBACK (ptyxis_tab_surface_bell_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->surface, "commit",
                           G_CALLBACK (ptyxis_tab_surface_commit_cb),
                           self, G_CONNECT_SWAPPED);

  /* Relay property notifications from surface to tab */
  g_signal_connect_object (self->surface, "notify",
                           G_CALLBACK (ptyxis_tab_surface_notify_cb),
                           self, G_CONNECT_SWAPPED);

  /* Connect to terminal's palette changes for thumbnail invalidation */
  terminal = ptyxis_terminal_surface_get_terminal (self->surface);
  g_signal_connect_object (terminal, "notify::palette",
                           G_CALLBACK (ptyxis_tab_notify_palette_cb),
                           self, G_CONNECT_SWAPPED);

  /* Desktop notifications for shell commands */
  ptyxis_notify_init (&self->notify,
                      GTK_WIDGET (self),
                      terminal,
                      self->surface,
                      self->uuid);

  /* Inhibit logout */
  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  g_signal_connect_object (settings,
                           "notify::inhibit-logout",
                           G_CALLBACK (ptyxis_tab_update_inhibit),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tab_update_inhibit (self);

  /* Tab monitor for periodic polling */
  self->monitor = ptyxis_surface_monitor_new (self->surface, GTK_WIDGET (self));
}

static void
ptyxis_tab_dispose (GObject *object)
{
  PtyxisTab *self = (PtyxisTab *)object;
  GtkWidget *child;

  ptyxis_notify_destroy (&self->notify);

  if (self->surface != NULL)
    ptyxis_terminal_surface_force_quit (self->surface);

  g_clear_object (&self->monitor);
  g_clear_object (&self->surface);

  gtk_widget_dispose_template (GTK_WIDGET (self), PTYXIS_TYPE_TAB);

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);

  g_clear_object (&self->cached_texture);
  g_clear_object (&self->profile);

  if (self->inhibit_cookie != 0)
    {
      gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                 self->inhibit_cookie);
      self->inhibit_cookie = 0;
    }

  G_OBJECT_CLASS (ptyxis_tab_parent_class)->dispose (object);
}

static void
ptyxis_tab_finalize (GObject *object)
{
  PtyxisTab *self = (PtyxisTab *)object;

  g_clear_pointer (&self->uuid, g_free);

  G_OBJECT_CLASS (ptyxis_tab_parent_class)->finalize (object);
}

static void
ptyxis_tab_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PtyxisTab *self = PTYXIS_TAB (object);

  switch (prop_id)
    {
    case PROP_COMMAND_LINE:
      g_value_set_string (value, ptyxis_terminal_surface_get_command_line (self->surface));
      break;

    case PROP_ICON:
      g_value_take_object (value, ptyxis_terminal_surface_dup_icon (self->surface));
      break;

    case PROP_DISABLE_DYNAMIC_TITLE:
      {
        gboolean disable_dynamic_title;
        g_object_get (self->surface, "disable-dynamic-title", &disable_dynamic_title, NULL);
        g_value_set_boolean (value, disable_dynamic_title);
      }
      break;

    case PROP_IGNORE_OSC_TITLE:
      g_value_set_boolean (value, ptyxis_terminal_surface_get_ignore_osc_title (self->surface));
      break;

    case PROP_INDICATOR_ICON:
      g_value_take_object (value, ptyxis_tab_dup_indicator_icon (self));
      break;

    case PROP_PROCESS_LEADER_KIND:
      g_value_set_enum (value, ptyxis_terminal_surface_get_process_leader_kind (self->surface));
      break;

    case PROP_PROGRESS:
      g_value_set_enum (value, ptyxis_terminal_surface_get_progress (self->surface));
      break;

    case PROP_PROGRESS_FRACTION:
      g_value_set_double (value, ptyxis_terminal_surface_get_progress_fraction (self->surface));
      break;

    case PROP_PROFILE:
      g_value_set_object (value, ptyxis_tab_get_profile (self));
      break;

    case PROP_READ_ONLY:
      {
        PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (self->surface);
        g_value_set_boolean (value, !vte_terminal_get_input_enabled (VTE_TERMINAL (terminal)));
      }
      break;

    case PROP_SUBTITLE:
      g_value_take_string (value, ptyxis_tab_dup_subtitle (self));
      break;

    case PROP_TITLE:
      g_value_take_string (value, ptyxis_tab_dup_title (self));
      break;

    case PROP_TITLE_PREFIX:
      g_value_set_string (value, ptyxis_tab_get_title_prefix (self));
      break;

    case PROP_UUID:
      g_value_set_string (value, ptyxis_tab_get_uuid (self));
      break;

    case PROP_ZOOM:
      g_value_set_enum (value, ptyxis_terminal_surface_get_zoom (self->surface));
      break;

    case PROP_ZOOM_LABEL:
      g_value_take_string (value, ptyxis_terminal_surface_dup_zoom_label (self->surface));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_tab_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PtyxisTab *self = PTYXIS_TAB (object);

  switch (prop_id)
    {
    case PROP_DISABLE_DYNAMIC_TITLE:
      if (self->surface != NULL)
        g_object_set (self->surface, "disable-dynamic-title", g_value_get_boolean (value), NULL);
      break;

    case PROP_IGNORE_OSC_TITLE:
      if (self->surface != NULL)
        ptyxis_terminal_surface_set_ignore_osc_title (self->surface, g_value_get_boolean (value));
      break;

    case PROP_PROFILE:
      self->profile = g_value_dup_object (value);
      break;

    case PROP_READ_ONLY:
      if (self->surface != NULL)
        {
          PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (self->surface);
          vte_terminal_set_input_enabled (VTE_TERMINAL (terminal), !g_value_get_boolean (value));
        }
      break;

    case PROP_TITLE_PREFIX:
      ptyxis_tab_set_title_prefix (self, g_value_get_string (value));
      break;

    case PROP_ZOOM:
      if (self->surface != NULL)
        ptyxis_terminal_surface_set_zoom (self->surface, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_tab_class_init (PtyxisTabClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = ptyxis_tab_constructed;
  object_class->dispose = ptyxis_tab_dispose;
  object_class->finalize = ptyxis_tab_finalize;
  object_class->get_property = ptyxis_tab_get_property;
  object_class->set_property = ptyxis_tab_set_property;

  widget_class->snapshot = ptyxis_tab_snapshot;
  widget_class->size_allocate = ptyxis_tab_size_allocate;
  widget_class->root = ptyxis_tab_root;
  widget_class->unroot = ptyxis_tab_unroot;

  properties[PROP_COMMAND_LINE] =
    g_param_spec_string ("command-line", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL,
                         G_TYPE_ICON,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_DISABLE_DYNAMIC_TITLE] =
    g_param_spec_boolean ("disable-dynamic-title", NULL, NULL,
                         FALSE,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_IGNORE_OSC_TITLE] =
    g_param_spec_boolean ("ignore-osc-title", NULL, NULL,
                         FALSE,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_INDICATOR_ICON] =
    g_param_spec_object ("indicator-icon", NULL, NULL,
                         G_TYPE_ICON,
                         (G_PARAM_READABLE |
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

  properties[PROP_READ_ONLY] =
    g_param_spec_boolean ("read-only", NULL, NULL,
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));

  properties[PROP_SUBTITLE] =
    g_param_spec_string ("subtitle", NULL, NULL,
                         NULL,
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

  properties[PROP_UUID] =
    g_param_spec_string ("uuid", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
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

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Ptyxis/ptyxis-tab.ui");
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "ptyxistab");

  gtk_widget_class_bind_template_child (widget_class, PtyxisTab, banner);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTab, box);

  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_notify_contains_focus_cb);

  gtk_widget_class_install_action (widget_class, "tab.respawn", NULL, ptyxis_tab_respawn_action);
  gtk_widget_class_install_action (widget_class, "tab.inspect", NULL, ptyxis_tab_inspect_action);
}

static void
ptyxis_tab_init (PtyxisTab *self)
{
  self->uuid = g_uuid_string_random ();

  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect_object (adw_style_manager_get_default (),
                           "notify::dark",
                           G_CALLBACK (ptyxis_tab_invalidate_thumbnail),
                           self,
                           G_CONNECT_SWAPPED);
}

/*
 * Public API
 */

PtyxisTab *
ptyxis_tab_new (PtyxisProfile *profile)
{
  g_return_val_if_fail (PTYXIS_IS_PROFILE (profile), NULL);

  return g_object_new (PTYXIS_TYPE_TAB,
                       "profile", profile,
                       NULL);
}

PtyxisProfile *
ptyxis_tab_get_profile (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->profile;
}

void
ptyxis_tab_apply_profile (PtyxisTab     *self,
                          PtyxisProfile *new_profile)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (PTYXIS_IS_PROFILE (new_profile));

  if (self->profile == new_profile)
    return;

  g_set_object (&self->profile, new_profile);
  ptyxis_terminal_surface_apply_profile (self->surface, new_profile);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROFILE]);
}

const char *
ptyxis_tab_get_title_prefix (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_get_title_prefix (self->surface);
}

void
ptyxis_tab_set_title_prefix (PtyxisTab  *self,
                             const char *title_prefix)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_title_prefix (self->surface, title_prefix);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE_PREFIX]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

char *
ptyxis_tab_dup_title (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_dup_title (self->surface);
}

char *
ptyxis_tab_dup_subtitle (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_dup_subtitle (self->surface);
}

char *
ptyxis_tab_dup_current_directory_uri (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_dup_current_directory_uri (self->surface);
}

void
ptyxis_tab_set_initial_working_directory_uri (PtyxisTab  *self,
                                              const char *initial_working_directory_uri)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_initial_working_directory_uri (self->surface, initial_working_directory_uri);
}

char *
ptyxis_tab_dup_previous_working_directory_uri (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_dup_previous_working_directory_uri (self->surface);
}

void
ptyxis_tab_set_previous_working_directory_uri (PtyxisTab  *self,
                                               const char *previous_working_directory_uri)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_previous_working_directory_uri (self->surface, previous_working_directory_uri);
}

PtyxisZoomLevel
ptyxis_tab_get_zoom (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), 0);

  return ptyxis_terminal_surface_get_zoom (self->surface);
}

void
ptyxis_tab_set_zoom (PtyxisTab       *self,
                     PtyxisZoomLevel  zoom)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_zoom (self->surface, zoom);
}

void
ptyxis_tab_zoom_in (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_zoom_in (self->surface);
}

void
ptyxis_tab_zoom_out (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_zoom_out (self->surface);
}

PtyxisTerminal *
ptyxis_tab_get_terminal (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_get_terminal (self->surface);
}

void
ptyxis_tab_raise (PtyxisTab *self)
{
  AdwTabView *tab_view;
  AdwTabPage *tab_page;

  g_return_if_fail (PTYXIS_IS_TAB (self));

  if ((tab_view = ADW_TAB_VIEW (gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW))) &&
      (tab_page = adw_tab_view_get_page (tab_view, GTK_WIDGET (self))))
    adw_tab_view_set_selected_page (tab_view, tab_page);
}

gboolean
ptyxis_tab_is_running (PtyxisTab  *self,
                       char      **cmdline)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  return ptyxis_terminal_surface_is_running (self->surface, cmdline);
}

void
ptyxis_tab_force_quit (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (self->surface != NULL)
    ptyxis_terminal_surface_force_quit (self->surface);
}

char *
ptyxis_tab_dup_zoom_label (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_dup_zoom_label (self->surface);
}

void
ptyxis_tab_show_banner (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);
}

void
ptyxis_tab_set_needs_attention (PtyxisTab *self,
                                gboolean   needs_attention)
{
  GtkWidget *tab_view;
  AdwTabPage *page;

  g_return_if_fail (PTYXIS_IS_TAB (self));

  if ((tab_view = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW)) &&
      (page = adw_tab_view_get_page (ADW_TAB_VIEW (tab_view), GTK_WIDGET (self))))
    adw_tab_page_set_needs_attention (page, needs_attention);
}

const char *
ptyxis_tab_get_uuid (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->uuid;
}

PtyxisIpcContainer *
ptyxis_tab_dup_container (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_dup_container (self->surface);
}

void
ptyxis_tab_set_container (PtyxisTab          *self,
                          PtyxisIpcContainer *container)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (!container || PTYXIS_IPC_IS_CONTAINER (container));

  ptyxis_terminal_surface_set_container (self->surface, container);
}

gboolean
ptyxis_tab_has_foreground_process (PtyxisTab  *self,
                                   GPid       *pid,
                                   char      **cmdline)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  return ptyxis_terminal_surface_has_foreground_process (self->surface, pid, cmdline);
}

void
ptyxis_tab_set_command (PtyxisTab          *self,
                        const char * const *command)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_command (self->surface, command);
}

const char *
ptyxis_tab_get_initial_title (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_get_initial_title (self->surface);
}

void
ptyxis_tab_set_initial_title (PtyxisTab  *self,
                              const char *initial_title)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_initial_title (self->surface, initial_title);
}

const char *
ptyxis_tab_get_command_line (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return ptyxis_terminal_surface_get_command_line (self->surface);
}

void
ptyxis_tab_open_uri (PtyxisTab  *self,
                     const char *uri)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (uri != NULL);

  ptyxis_terminal_surface_open_uri (self->surface, uri);
}

PtyxisTabProgress
ptyxis_tab_get_progress (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), 0);

  return ptyxis_terminal_surface_get_progress (self->surface);
}

double
ptyxis_tab_get_progress_fraction (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), .0);

  return ptyxis_terminal_surface_get_progress_fraction (self->surface);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
draw_progress (cairo_t         *cr,
               GtkStyleContext *style_context,
               int              width,
               int              height,
               double           progress)
{
  GdkRGBA rgba;
  double alpha;

  g_assert (cr != NULL);
  g_assert (style_context != NULL);

  progress = CLAMP (progress, 0, 1);

  gtk_style_context_get_color (style_context, &rgba);

  alpha = rgba.alpha;
  rgba.alpha *= .15;
  gdk_cairo_set_source_rgba (cr, &rgba);

  cairo_arc (cr,
             width / 2,
             height / 2,
             width / 2,
             0.0,
             2 * M_PI);
  cairo_fill (cr);

  if (progress > 0.0)
    {
      rgba.alpha = alpha;
      gdk_cairo_set_source_rgba (cr, &rgba);

      cairo_arc (cr,
                 width / 2,
                 height / 2,
                 width / 2,
                 (-.5 * M_PI),
                 (2 * progress * M_PI) - (.5 * M_PI));

      if (progress != 1.0)
        {
          cairo_line_to (cr, width / 2, height / 2);
          cairo_line_to (cr, width / 2, 0);
        }

      cairo_fill (cr);
    }
}
G_GNUC_END_IGNORE_DEPRECATIONS

GIcon *
ptyxis_tab_dup_indicator_icon (PtyxisTab *self)
{
  PtyxisTabProgress progress;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  progress = ptyxis_terminal_surface_get_progress (self->surface);

  if (progress == PTYXIS_TAB_PROGRESS_ERROR)
    return g_themed_icon_new ("dialog-error-symbolic");

  if (progress == PTYXIS_TAB_PROGRESS_INDETERMINATE)
    return NULL;

  if (progress == PTYXIS_TAB_PROGRESS_ACTIVE)
    {
      g_autoptr(GdkTexture) texture = NULL;
      g_autoptr(GBytes) bytes = NULL;
      cairo_surface_t *surface;
      cairo_t *cr;
      double fraction;
      int stride;
      int scale;
      int width;
      int height;

      fraction = ptyxis_terminal_surface_get_progress_fraction (self->surface);
      scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
      width = 16 * scale;
      height = 16 * scale;

      surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
      stride = cairo_image_surface_get_stride (surface);
      cr = cairo_create (surface);

      G_GNUC_BEGIN_IGNORE_DEPRECATIONS {
        GtkStyleContext *style_context = gtk_widget_get_style_context (GTK_WIDGET (self));
        draw_progress (cr, style_context, width, height, fraction);
      } G_GNUC_END_IGNORE_DEPRECATIONS

      cairo_destroy (cr);

      bytes = g_bytes_new (cairo_image_surface_get_data (surface), height * stride);
      texture = gdk_memory_texture_new (width, height, GDK_MEMORY_DEFAULT, bytes, stride);

      cairo_surface_destroy (surface);

      return G_ICON (g_steal_pointer (&texture));
    }

  return NULL;
}

gboolean
ptyxis_tab_get_ignore_osc_title (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  return ptyxis_terminal_surface_get_ignore_osc_title (self->surface);
}

void
ptyxis_tab_set_ignore_osc_title (PtyxisTab *self,
                                 gboolean   ignore_osc_title)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_ignore_osc_title (self->surface, ignore_osc_title);
}

void
_ptyxis_tab_ignore_snapshot (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  self->ignore_snapshot = TRUE;
}

void
ptyxis_tab_grab_focus (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_grab_focus (self->surface);
}

void
ptyxis_tab_set_environment (PtyxisTab          *self,
                            const char * const *env)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_environment (self->surface, env);
}

void
ptyxis_tab_set_keep_terminal_open (PtyxisTab *self,
                                   gboolean   keep_terminal_open)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_terminal_surface_set_keep_terminal_open (self->surface, keep_terminal_open);
}

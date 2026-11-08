/*
 * ptyxis-pane.c
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

#include <glib/gi18n.h>

#include "ptyxis-application.h"
#include "ptyxis-enums.h"
#include "ptyxis-find-bar.h"
#include "ptyxis-inspector.h"
#include "ptyxis-pane.h"
#include "ptyxis-split-tree.h"
#include "ptyxis-surface-monitor.h"
#include "ptyxis-notify.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-tiling-window.h"
#include "ptyxis-window.h"

struct _PtyxisPane
{
  GtkWidget              parent_instance;

  PtyxisTerminalSurface *surface;      /* (owned) (not nullable) */
  PtyxisProfile         *profile;      /* (owned) (not nullable) */
  PtyxisSurfaceMonitor  *monitor;      /* (owned) (nullable) */
  GtkBox                *box;          /* (unowned) (not nullable) - template child */
  AdwBanner             *banner;       /* (unowned) (not nullable) - template child */
  GtkWidget             *chrome;       /* (unowned) (not nullable) - template child */
  GtkMenuButton         *title_button; /* (unowned) (not nullable) - template child */
  GtkLabel              *title_label;  /* (unowned) (not nullable) - template child */
  GtkImage              *read_only_icon; /* (unowned) (not nullable) - template child */
  GtkImage              *read_only_check; /* (unowned) (nullable) */
  GtkWidget             *zoom_label;     /* (unowned) (nullable) */
  GtkRevealer           *close_revealer;      /* (unowned) (nullable) */
  GtkLabel              *close_process_label; /* (unowned) (nullable) */
  GtkButton             *split_down_button;  /* (unowned) (not nullable) - template child */
  GtkButton             *split_right_button; /* (unowned) (not nullable) - template child */
  GtkButton             *maximize_button;    /* (unowned) (not nullable) - template child */
  GtkButton             *close_button;       /* (unowned) (not nullable) - template child */
  GtkWidget             *drop_highlight; /* (unowned) (nullable) */
  PtyxisFindBar         *find_bar;       /* (unowned) (not nullable) */
  GtkRevealer           *find_bar_revealer; /* (unowned) (not nullable) */
  char                  *uuid;           /* (owned) (not nullable) */
  PtyxisNotify           notify;
};

enum {
  PROP_0,
  PROP_PROFILE,
  PROP_TITLE,
  PROP_ZOOM,
  PROP_ZOOM_LABEL,
  PROP_READ_ONLY,
  N_PROPS
};

enum {
  CLOSE_REQUEST,
  N_SIGNALS
};

G_DEFINE_FINAL_TYPE (PtyxisPane, ptyxis_pane, GTK_TYPE_WIDGET)

PtyxisTerminalSurface *
ptyxis_pane_get_surface (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return self->surface;
}

static GParamSpec *properties[N_PROPS];
static unsigned int signals[N_SIGNALS];

/*
 * Surface signal handlers
 */

static void
ptyxis_pane_surface_notify_user_cb (PtyxisPane            *self,
                                    const char            *title,
                                    const char            *button_label,
                                    const char            *action_name,
                                    GVariant              *action_target,
                                    PtyxisTerminalSurface *surface)
{
  g_assert (PTYXIS_IS_PANE (self));

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
    action_name = "pane.respawn";

  if (action_target != NULL)
    gtk_actionable_set_action_target_value (GTK_ACTIONABLE (self->banner), action_target);

  gtk_actionable_set_action_name (GTK_ACTIONABLE (self->banner), action_name);
  gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);
}

static void
ptyxis_pane_surface_request_close_cb (PtyxisPane            *self,
                                      PtyxisTerminalSurface *surface)
{
  g_assert (PTYXIS_IS_PANE (self));

  g_signal_emit (self, signals[CLOSE_REQUEST], 0);
}

static void
ptyxis_pane_update_title_label (PtyxisPane *self)
{
  g_autofree char *title = NULL;

  title = ptyxis_terminal_surface_dup_title (self->surface);
  gtk_label_set_label (self->title_label, title);
}

static void
ptyxis_pane_surface_notify_cb (PtyxisPane            *self,
                               GParamSpec            *pspec,
                               PtyxisTerminalSurface *surface)
{
  const char *name;

  g_assert (PTYXIS_IS_PANE (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));

  name = g_param_spec_get_name (pspec);

  if (strcmp (name, "title") == 0)
    {
      ptyxis_pane_update_title_label (self);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
  else if (strcmp (name, "zoom") == 0)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM]);
  else if (strcmp (name, "zoom-label") == 0)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM_LABEL]);
}

/*
 * Actions
 */

static void
ptyxis_pane_close_confirm_action (GtkWidget  *widget,
                                  const char *action_name,
                                  GVariant   *param)
{
  PtyxisPane *self = PTYXIS_PANE (widget);

  gtk_widget_set_can_target (GTK_WIDGET (self->close_revealer), FALSE);
  gtk_revealer_set_reveal_child (self->close_revealer, FALSE);
  g_signal_emit (self, signals[CLOSE_REQUEST], 0);
}

static void
ptyxis_pane_close_cancel_action (GtkWidget  *widget,
                                 const char *action_name,
                                 GVariant   *param)
{
  PtyxisPane *self = PTYXIS_PANE (widget);

  gtk_widget_set_can_target (GTK_WIDGET (self->close_revealer), FALSE);
  gtk_revealer_set_reveal_child (self->close_revealer, FALSE);
}

static void
ptyxis_pane_close_action (GtkWidget  *widget,
                          const char *action_name,
                          GVariant   *param)
{
  PtyxisPane *self = PTYXIS_PANE (widget);
  PtyxisSettings *settings;

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  if (ptyxis_terminal_surface_is_running (self->surface, NULL) &&
      ptyxis_settings_get_prompt_on_close (settings))
    {
      g_autofree char *cmdline = NULL;
      g_autofree char *process_text = NULL;
      GPid pid;

      if (ptyxis_terminal_surface_has_foreground_process (self->surface, &pid, &cmdline))
        {
          g_utf8_make_valid (cmdline, -1);
          process_text = g_strdup_printf ("%s (PID %d)", cmdline, pid);
        }

      gtk_label_set_label (self->close_process_label,
                           process_text ? process_text : "");
      gtk_widget_set_visible (GTK_WIDGET (self->close_process_label),
                              process_text != NULL);

      gtk_widget_set_can_target (GTK_WIDGET (self->close_revealer), TRUE);
      gtk_revealer_set_reveal_child (self->close_revealer, TRUE);
      return;
    }

  g_signal_emit (self, signals[CLOSE_REQUEST], 0);
}

static void
ptyxis_pane_toggle_read_only_action (GtkWidget  *widget,
                                     const char *action_name,
                                     GVariant   *param)
{
  PtyxisPane *self = PTYXIS_PANE (widget);
  PtyxisTerminal *terminal;
  gboolean input_enabled;

  terminal = ptyxis_terminal_surface_get_terminal (self->surface);
  input_enabled = vte_terminal_get_input_enabled (VTE_TERMINAL (terminal));
  vte_terminal_set_input_enabled (VTE_TERMINAL (terminal), !input_enabled);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_READ_ONLY]);

  gtk_popover_popdown (gtk_menu_button_get_popover (self->title_button));
}

static void
ptyxis_pane_respawn_action (GtkWidget  *widget,
                            const char *action_name,
                            GVariant   *param)
{
  PtyxisPane *self = PTYXIS_PANE (widget);

  ptyxis_terminal_surface_respawn (self->surface);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static void
ptyxis_pane_inspect_action (GtkWidget  *widget,
                            const char *action_name,
                            GVariant   *param)
{
  PtyxisPane *self = PTYXIS_PANE (widget);
  PtyxisTerminal *terminal;
  PtyxisInspector *inspector;
  GtkRoot *root;

  terminal = ptyxis_terminal_surface_get_terminal (self->surface);
  inspector = ptyxis_inspector_new_for_terminal (terminal);
  root = gtk_widget_get_root (widget);

  gtk_window_set_transient_for (GTK_WINDOW (inspector), GTK_WINDOW (root));
  gtk_window_set_modal (GTK_WINDOW (inspector), /* modal = */ FALSE);
  gtk_window_present (GTK_WINDOW (inspector));
}

G_GNUC_END_IGNORE_DEPRECATIONS

/*
 * Drag and drop
 */

static void
pane_deserialize_cb (GdkContentDeserializer *deserializer)
{
  /* Same-app drags bypass deserialization — this only runs for
   * cross-app drops which we don't support */
  gdk_content_deserializer_return_error (
    deserializer,
    g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         "Cross-app pane transfer not supported"));
}

typedef enum _DropZone
{
  DROP_ZONE_NONE,
  DROP_ZONE_TOP,
  DROP_ZONE_BOTTOM,
  DROP_ZONE_LEFT,
  DROP_ZONE_RIGHT,
} DropZone;

static PtyxisPane *current_drag_source;
static gboolean drag_over_any_pane;

static DropZone
compute_drop_zone (double x,
                   double y,
                   int    width,
                   int    height)
{
  double py_w;
  double px_h;
  double wpx_h;

  if (width <= 0 || height <= 0)
    return DROP_ZONE_NONE;

  py_w = y * width;
  px_h = x * height;
  wpx_h = (width - x) * height;

  /* Quadrants divided by diagonals from corners */
  if (py_w < px_h)
    {
      if (py_w < wpx_h)
        return DROP_ZONE_TOP;
      return DROP_ZONE_RIGHT;
    }

  if (py_w < wpx_h)
    return DROP_ZONE_LEFT;
  return DROP_ZONE_BOTTOM;
}

static void
update_drop_highlight (PtyxisPane *self,
                       DropZone    zone)
{
  static const char * const zone_classes[] = {
    [DROP_ZONE_TOP] = "drop-top",
    [DROP_ZONE_BOTTOM] = "drop-bottom",
    [DROP_ZONE_LEFT] = "drop-left",
    [DROP_ZONE_RIGHT] = "drop-right",
  };

  for (size_t i = DROP_ZONE_TOP; i <= DROP_ZONE_RIGHT; i++)
    gtk_widget_remove_css_class (self->drop_highlight, zone_classes[i]);

  if (zone == DROP_ZONE_NONE)
    {
      gtk_widget_set_visible (self->drop_highlight, FALSE);
      return;
    }

  gtk_widget_set_visible (self->drop_highlight, TRUE);
  gtk_widget_add_css_class (self->drop_highlight, zone_classes[zone]);
}

static GdkContentProvider *
on_drag_prepare (GtkDragSource *source,
                 double         x,
                 double         y,
                 PtyxisPane    *self)
{
  GdkContentProvider *providers[2];
  GBytes *bytes;

  providers[0] = gdk_content_provider_new_typed (PTYXIS_TYPE_PANE, self);

  /* Private MIME type enables cross-surface drag on Wayland
   * without being accepted by other applications */
  bytes = g_bytes_new_static ("", 1);
  providers[1] = gdk_content_provider_new_for_bytes ("application/x-ptyxis-pane", bytes);
  g_bytes_unref (bytes);

  return gdk_content_provider_new_union (providers, 2);
}

static void
on_drag_begin (GtkDragSource *source,
               GdkDrag       *drag,
               PtyxisPane    *self)
{
  GdkPaintable *paintable;

  current_drag_source = self;

  paintable = gtk_widget_paintable_new (self->chrome);
  gtk_drag_source_set_icon (source, paintable, 0, 0);
  g_object_unref (paintable);
}

static gboolean
on_drag_cancel (GtkDragSource       *source,
                GdkDrag             *drag,
                GdkDragCancelReason  reason,
                PtyxisPane          *self)
{
  PtyxisSplitTree *split_tree;
  GtkWindow *new_window;

  if (reason == GDK_DRAG_CANCEL_USER_CANCELLED)
    return FALSE;

  if (drag_over_any_pane)
    return FALSE;

  split_tree = PTYXIS_SPLIT_TREE (gtk_widget_get_ancestor (GTK_WIDGET (self),
                                                            PTYXIS_TYPE_SPLIT_TREE));
  if (split_tree == NULL)
    return FALSE;

  if (ptyxis_split_tree_get_n_panes (split_tree) <= 1)
    return FALSE;

  if (!ptyxis_split_tree_detach_pane (split_tree, self))
    return FALSE;

  new_window = GTK_WINDOW (ptyxis_tiling_window_new_with_pane (self));
  gtk_window_present (new_window);

  return TRUE;
}

static void
on_drag_end (GtkDragSource *source,
             GdkDrag       *drag,
             gboolean       delete_data,
             PtyxisPane    *self)
{
  current_drag_source = NULL;
  drag_over_any_pane = FALSE;
}

static GdkDragAction
on_drop_enter (GtkDropTarget *target,
               double         x,
               double         y,
               PtyxisPane    *self)
{
  int width;
  int height;
  DropZone zone;

  drag_over_any_pane = TRUE;

  if (current_drag_source == self)
    return GDK_ACTION_MOVE;

  width = gtk_widget_get_width (GTK_WIDGET (self));
  height = gtk_widget_get_height (GTK_WIDGET (self));
  zone = compute_drop_zone (x, y, width, height);
  update_drop_highlight (self, zone);

  return GDK_ACTION_MOVE;
}

static GdkDragAction
on_drop_motion (GtkDropTarget *target,
                double         x,
                double         y,
                PtyxisPane    *self)
{
  int width;
  int height;
  DropZone zone;

  drag_over_any_pane = TRUE;

  if (current_drag_source == self)
    return GDK_ACTION_MOVE;

  width = gtk_widget_get_width (GTK_WIDGET (self));
  height = gtk_widget_get_height (GTK_WIDGET (self));
  zone = compute_drop_zone (x, y, width, height);
  update_drop_highlight (self, zone);

  return GDK_ACTION_MOVE;
}

static void
on_drop_leave (GtkDropTarget *target,
               PtyxisPane    *self)
{
  drag_over_any_pane = FALSE;
  update_drop_highlight (self, DROP_ZONE_NONE);
}

static gboolean
on_drop (GtkDropTarget *target,
         GValue        *value,
         double         x,
         double         y,
         PtyxisPane    *self)
{
  PtyxisPane *source;
  PtyxisSplitTree *target_tree;
  PtyxisSplitTree *source_tree;
  int width;
  int height;
  DropZone zone;
  GtkOrientation orientation;
  gboolean after;

  update_drop_highlight (self, DROP_ZONE_NONE);

  if (G_VALUE_HOLDS (value, PTYXIS_TYPE_PANE))
    source = g_value_get_object (value);
  else
    source = current_drag_source;

  if (source == NULL)
    return FALSE;

  if (source == self)
    return TRUE;

  target_tree = PTYXIS_SPLIT_TREE (gtk_widget_get_ancestor (GTK_WIDGET (self),
                                                             PTYXIS_TYPE_SPLIT_TREE));
  if (target_tree == NULL)
    return FALSE;

  width = gtk_widget_get_width (GTK_WIDGET (self));
  height = gtk_widget_get_height (GTK_WIDGET (self));
  zone = compute_drop_zone (x, y, width, height);

  switch (zone)
    {
    case DROP_ZONE_NONE:
      return FALSE;
    case DROP_ZONE_TOP:
      orientation = GTK_ORIENTATION_VERTICAL;
      after = FALSE;
      break;
    case DROP_ZONE_BOTTOM:
      orientation = GTK_ORIENTATION_VERTICAL;
      after = TRUE;
      break;
    case DROP_ZONE_LEFT:
      orientation = GTK_ORIENTATION_HORIZONTAL;
      after = FALSE;
      break;
    case DROP_ZONE_RIGHT:
      orientation = GTK_ORIENTATION_HORIZONTAL;
      after = TRUE;
      break;
    default:
      return FALSE;
    }

  source_tree = PTYXIS_SPLIT_TREE (gtk_widget_get_ancestor (GTK_WIDGET (source),
                                                             PTYXIS_TYPE_SPLIT_TREE));

  if (source_tree == target_tree)
    {
      ptyxis_split_tree_move_pane (target_tree, source, self, orientation, after);
    }
  else
    {
      if (source_tree != NULL)
        ptyxis_split_tree_detach_pane (source_tree, source);

      ptyxis_split_tree_insert_pane (target_tree, source, self, orientation, after);
    }

  return TRUE;
}

/*
 * GObject
 */

static void
ptyxis_pane_add_zoom_controls (PtyxisPane *self)
{
  PangoAttrList *attrs;
  GtkPopover *popover;
  GtkWidget *zoom_box;
  GtkWidget *zoom_out;
  GtkWidget *zoom_in;

  popover = gtk_menu_button_get_popover (self->title_button);

  zoom_box = g_object_new (GTK_TYPE_BOX,
                           "spacing", 12,
                           "margin-start", 18,
                           "margin-end", 18,
                           "width-request", 160,
                           NULL);
  zoom_out = g_object_new (GTK_TYPE_BUTTON,
                           "action-name", "win.zoom-out",
                           "action-target", g_variant_new_boolean (FALSE),
                           "child", g_object_new (GTK_TYPE_IMAGE,
                                                   "icon-name", "zoom-out-symbolic",
                                                   "pixel-size", 16,
                                                   NULL),
                           NULL);
  gtk_widget_add_css_class (zoom_out, "circular");
  gtk_widget_add_css_class (zoom_out, "flat");
  gtk_accessible_update_property (GTK_ACCESSIBLE (zoom_out),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Zoom out"), -1);
  zoom_in = g_object_new (GTK_TYPE_BUTTON,
                          "action-name", "win.zoom-in",
                          "action-target", g_variant_new_boolean (FALSE),
                          "child", g_object_new (GTK_TYPE_IMAGE,
                                                  "icon-name", "zoom-in-symbolic",
                                                  "pixel-size", 16,
                                                  NULL),
                          NULL);
  gtk_widget_add_css_class (zoom_in, "circular");
  gtk_widget_add_css_class (zoom_in, "flat");
  gtk_accessible_update_property (GTK_ACCESSIBLE (zoom_in),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Zoom in"), -1);
  attrs = pango_attr_list_new ();

  pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
  self->zoom_label = g_object_new (GTK_TYPE_BUTTON,
                                   "css-classes", (const char * const[]) {"flat", "pill", NULL},
                                   "action-name", "win.zoom-one",
                                   "action-target", g_variant_new_boolean (FALSE),
                                   "hexpand", TRUE,
                                   "tooltip-text", _("Reset Zoom"),
                                   "label", "100%",
                                   NULL);
  gtk_label_set_attributes (GTK_LABEL (gtk_button_get_child (GTK_BUTTON (self->zoom_label))), attrs);
  pango_attr_list_unref (attrs);
  gtk_box_append (GTK_BOX (zoom_box), zoom_out);
  gtk_box_append (GTK_BOX (zoom_box), self->zoom_label);
  gtk_box_append (GTK_BOX (zoom_box), zoom_in);
  gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), zoom_box, "zoom");
}

static void
ptyxis_pane_add_read_only_toggle (PtyxisPane *self)
{
  GtkPopover *popover;
  GtkWidget *button;
  GtkWidget *box;

  popover = gtk_menu_button_get_popover (self->title_button);

  box = g_object_new (GTK_TYPE_BOX,
                      "spacing", 6,
                      NULL);
  gtk_box_append (GTK_BOX (box),
                  g_object_new (GTK_TYPE_LABEL,
                                "label", _("Read-Only"),
                                "hexpand", TRUE,
                                "xalign", 0.0f,
                                NULL));
  self->read_only_check = g_object_new (GTK_TYPE_IMAGE,
                                        "icon-name", "object-select-symbolic",
                                        "visible", FALSE,
                                        NULL);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->read_only_check));

  button = g_object_new (GTK_TYPE_BUTTON,
                         "action-name", "pane.read-only",
                         "child", box,
                         NULL);
  gtk_widget_add_css_class (button, "flat");

  gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), button, "read-only");
}

static void
ptyxis_pane_constructed (GObject *object)
{
  PtyxisPane *self = (PtyxisPane *)object;
  PtyxisTerminal *terminal;
  GtkScrolledWindow *scrolled_window;
  GtkWidget *overlay;
  GtkWidget *revealer;
  GtkWidget *dimming;
  GtkWidget *dialog_box;
  GtkWidget *heading;
  GtkWidget *body;
  GtkWidget *process_label;
  GtkWidget *button_box;
  GtkWidget *cancel_button;
  GtkWidget *close_button;
  GtkWidget *find_revealer;
  GtkWidget *find_bar;

  G_OBJECT_CLASS (ptyxis_pane_parent_class)->constructed (object);

  self->surface = ptyxis_terminal_surface_new (self->profile);

  scrolled_window = ptyxis_terminal_surface_get_scrolled_window (self->surface);

  overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), GTK_WIDGET (scrolled_window));

  revealer = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_can_target (revealer, FALSE);
  gtk_widget_set_halign (revealer, GTK_ALIGN_FILL);
  gtk_widget_set_valign (revealer, GTK_ALIGN_FILL);

  dimming = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (dimming, "pane-close-dialog");
  gtk_widget_set_halign (dimming, GTK_ALIGN_FILL);
  gtk_widget_set_valign (dimming, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand (dimming, TRUE);
  gtk_widget_set_vexpand (dimming, TRUE);

  dialog_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (dialog_box, "pane-close-dialog-box");
  gtk_widget_set_halign (dialog_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (dialog_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (dialog_box, TRUE);
  gtk_widget_set_vexpand (dialog_box, TRUE);
  gtk_widget_set_size_request (dialog_box, 250, -1);

  heading = gtk_label_new (_("Close Terminal?"));
  gtk_widget_add_css_class (heading, "title-2");
  gtk_label_set_wrap (GTK_LABEL (heading), TRUE);
  gtk_label_set_justify (GTK_LABEL (heading), GTK_JUSTIFY_CENTER);

  body = gtk_label_new (_("A process is still running."));
  gtk_widget_add_css_class (body, "body");
  gtk_label_set_wrap (GTK_LABEL (body), TRUE);
  gtk_label_set_justify (GTK_LABEL (body), GTK_JUSTIFY_CENTER);

  process_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (process_label, "dim-label");
  gtk_label_set_ellipsize (GTK_LABEL (process_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_visible (process_label, FALSE);
  self->close_process_label = GTK_LABEL (process_label);

  button_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (button_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (button_box, 6);

  cancel_button = gtk_button_new_with_mnemonic (_("_Cancel"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (cancel_button),
                                 "pane.close-cancel");

  close_button = gtk_button_new_with_mnemonic (_("_Close"));
  gtk_widget_add_css_class (close_button, "destructive-action");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (close_button),
                                 "pane.close-confirm");

  gtk_box_append (GTK_BOX (button_box), cancel_button);
  gtk_box_append (GTK_BOX (button_box), close_button);

  gtk_box_append (GTK_BOX (dialog_box), heading);
  gtk_box_append (GTK_BOX (dialog_box), body);
  gtk_box_append (GTK_BOX (dialog_box), process_label);
  gtk_box_append (GTK_BOX (dialog_box), button_box);

  gtk_box_append (GTK_BOX (dimming), dialog_box);

  gtk_revealer_set_child (GTK_REVEALER (revealer), dimming);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), revealer);

  self->close_revealer = GTK_REVEALER (revealer);

  gtk_box_append (self->box, overlay);

  find_revealer = gtk_revealer_new ();
  gtk_revealer_set_reveal_child (GTK_REVEALER (find_revealer), FALSE);
  gtk_revealer_set_transition_duration (GTK_REVEALER (find_revealer), 0);

  find_bar = g_object_new (PTYXIS_TYPE_FIND_BAR, NULL);
  ptyxis_find_bar_set_terminal (PTYXIS_FIND_BAR (find_bar),
                                ptyxis_terminal_surface_get_terminal (self->surface));
  gtk_revealer_set_child (GTK_REVEALER (find_revealer), find_bar);

  self->find_bar = PTYXIS_FIND_BAR (find_bar);
  self->find_bar_revealer = GTK_REVEALER (find_revealer);

  gtk_box_append (self->box, find_revealer);

  g_signal_connect_object (self->surface, "notify-user",
                           G_CALLBACK (ptyxis_pane_surface_notify_user_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->surface, "request-close",
                           G_CALLBACK (ptyxis_pane_surface_request_close_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->surface, "notify",
                           G_CALLBACK (ptyxis_pane_surface_notify_cb),
                           self, G_CONNECT_SWAPPED);

  g_object_bind_property (self, "read-only",
                          self->read_only_icon, "visible",
                          G_BINDING_SYNC_CREATE);

  ptyxis_pane_add_zoom_controls (self);
  ptyxis_pane_add_read_only_toggle (self);

  g_object_bind_property (self, "zoom-label",
                          self->zoom_label, "label",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "read-only",
                          self->read_only_check, "visible",
                          G_BINDING_SYNC_CREATE);

  self->monitor = ptyxis_surface_monitor_new (self->surface, GTK_WIDGET (self));

  terminal = ptyxis_terminal_surface_get_terminal (self->surface);

  ptyxis_notify_init (&self->notify,
                      GTK_WIDGET (self),
                      terminal,
                      self->surface,
                      self->uuid);
}

static void
ptyxis_pane_dispose (GObject *object)
{
  PtyxisPane *self = (PtyxisPane *)object;
  GtkWidget *child;

  ptyxis_notify_destroy (&self->notify);
  g_clear_object (&self->monitor);
  g_clear_pointer (&self->uuid, g_free);

  if (self->surface != NULL)
    ptyxis_terminal_surface_force_quit (self->surface);

  g_clear_object (&self->surface);

  gtk_widget_dispose_template (GTK_WIDGET (self), PTYXIS_TYPE_PANE);

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);

  g_clear_object (&self->profile);

  G_OBJECT_CLASS (ptyxis_pane_parent_class)->dispose (object);
}

static void
ptyxis_pane_get_property (GObject    *object,
                          unsigned int prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  PtyxisPane *self = PTYXIS_PANE (object);

  switch (prop_id)
    {
    case PROP_PROFILE:
      g_value_set_object (value, ptyxis_pane_get_profile (self));
      break;

    case PROP_TITLE:
      g_value_take_string (value, ptyxis_pane_dup_title (self));
      break;

    case PROP_ZOOM:
      if (self->surface != NULL)
        g_value_set_enum (value, ptyxis_terminal_surface_get_zoom (self->surface));
      else
        g_value_set_enum (value, PTYXIS_ZOOM_LEVEL_DEFAULT);
      break;

    case PROP_ZOOM_LABEL:
      if (self->surface != NULL)
        g_value_take_string (value, ptyxis_terminal_surface_dup_zoom_label (self->surface));
      else
        g_value_set_static_string (value, "100%");
      break;

    case PROP_READ_ONLY:
      if (self->surface != NULL)
        {
          PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (self->surface);

          g_value_set_boolean (value, !vte_terminal_get_input_enabled (VTE_TERMINAL (terminal)));
        }
      else
        g_value_set_boolean (value, FALSE);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_pane_set_property (GObject      *object,
                          unsigned int  prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  PtyxisPane *self = PTYXIS_PANE (object);

  switch (prop_id)
    {
    case PROP_PROFILE:
      self->profile = g_value_dup_object (value);
      break;

    case PROP_ZOOM:
      if (self->surface != NULL)
        ptyxis_terminal_surface_set_zoom (self->surface, g_value_get_enum (value));
      break;

    case PROP_READ_ONLY:
      if (self->surface != NULL)
        {
          PtyxisTerminal *terminal = ptyxis_terminal_surface_get_terminal (self->surface);

          vte_terminal_set_input_enabled (VTE_TERMINAL (terminal), !g_value_get_boolean (value));
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_pane_class_init (PtyxisPaneClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = ptyxis_pane_constructed;
  object_class->dispose = ptyxis_pane_dispose;
  object_class->get_property = ptyxis_pane_get_property;
  object_class->set_property = ptyxis_pane_set_property;

  properties[PROP_PROFILE] =
    g_param_spec_object ("profile", NULL, NULL,
                         PTYXIS_TYPE_PROFILE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  properties[PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ZOOM] =
    g_param_spec_enum ("zoom", NULL, NULL,
                       PTYXIS_TYPE_ZOOM_LEVEL,
                       PTYXIS_ZOOM_LEVEL_DEFAULT,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ZOOM_LABEL] =
    g_param_spec_string ("zoom-label", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_READ_ONLY] =
    g_param_spec_boolean ("read-only", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[CLOSE_REQUEST] =
    g_signal_new ("close-request",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Ptyxis/ptyxis-pane.ui");
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, box);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, banner);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, chrome);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, title_button);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, title_label);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, read_only_icon);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, split_down_button);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, split_right_button);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, maximize_button);
  gtk_widget_class_bind_template_child (widget_class, PtyxisPane, close_button);

  gtk_widget_class_install_action (widget_class, "pane.close", NULL, ptyxis_pane_close_action);
  gtk_widget_class_install_action (widget_class, "pane.close-confirm", NULL, ptyxis_pane_close_confirm_action);
  gtk_widget_class_install_action (widget_class, "pane.close-cancel", NULL, ptyxis_pane_close_cancel_action);
  gtk_widget_class_install_action (widget_class, "pane.inspect", NULL, ptyxis_pane_inspect_action);
  gtk_widget_class_install_action (widget_class, "pane.read-only", NULL, ptyxis_pane_toggle_read_only_action);
  gtk_widget_class_install_action (widget_class, "pane.respawn", NULL, ptyxis_pane_respawn_action);

  /* Register deserializer so GtkDropTarget matching PTYXIS_TYPE_PANE also
   * accepts the "application/x-ptyxis-pane" MIME type offered cross-surface */
  gdk_content_register_deserializer ("application/x-ptyxis-pane",
                                     PTYXIS_TYPE_PANE,
                                     pane_deserialize_cb,
                                     NULL, NULL);
}

static void
ptyxis_pane_init (PtyxisPane *self)
{
  GtkDragSource *drag_source;
  GtkDropTarget *drop_target;

  self->uuid = g_uuid_string_random ();

  gtk_widget_init_template (GTK_WIDGET (self));

  drag_source = gtk_drag_source_new ();
  gtk_drag_source_set_actions (drag_source, GDK_ACTION_MOVE);
  g_signal_connect (drag_source, "prepare", G_CALLBACK (on_drag_prepare), self);
  g_signal_connect (drag_source, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag_source, "drag-cancel", G_CALLBACK (on_drag_cancel), self);
  g_signal_connect (drag_source, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (self->chrome, GTK_EVENT_CONTROLLER (drag_source));

  drop_target = gtk_drop_target_new (PTYXIS_TYPE_PANE, GDK_ACTION_MOVE);
  g_signal_connect (drop_target, "enter", G_CALLBACK (on_drop_enter), self);
  g_signal_connect (drop_target, "motion", G_CALLBACK (on_drop_motion), self);
  g_signal_connect (drop_target, "leave", G_CALLBACK (on_drop_leave), self);
  g_signal_connect (drop_target, "drop", G_CALLBACK (on_drop), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drop_target));

  self->drop_highlight = g_object_new (GTK_TYPE_BOX, NULL);
  gtk_widget_add_css_class (self->drop_highlight, "drop-highlight");
  gtk_widget_set_can_target (self->drop_highlight, FALSE);
  gtk_widget_set_visible (self->drop_highlight, FALSE);
  gtk_widget_set_parent (self->drop_highlight, GTK_WIDGET (self));
}

/*
 * Public API
 */

PtyxisPane *
ptyxis_pane_new (PtyxisProfile *profile)
{
  g_return_val_if_fail (PTYXIS_IS_PROFILE (profile), NULL);

  return g_object_new (PTYXIS_TYPE_PANE,
                       "profile", profile,
                       NULL);
}

PtyxisTerminal *
ptyxis_pane_get_terminal (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return ptyxis_terminal_surface_get_terminal (self->surface);
}

PtyxisProfile *
ptyxis_pane_get_profile (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return ptyxis_terminal_surface_get_profile (self->surface);
}

void
ptyxis_pane_apply_profile (PtyxisPane    *self,
                           PtyxisProfile *profile)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));
  g_return_if_fail (PTYXIS_IS_PROFILE (profile));

  ptyxis_terminal_surface_apply_profile (self->surface, profile);
  g_set_object (&self->profile, profile);
}

PtyxisZoomLevel
ptyxis_pane_get_zoom (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), PTYXIS_ZOOM_LEVEL_DEFAULT);

  return ptyxis_terminal_surface_get_zoom (self->surface);
}

void
ptyxis_pane_set_zoom (PtyxisPane      *self,
                      PtyxisZoomLevel  zoom)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  ptyxis_terminal_surface_set_zoom (self->surface, zoom);
}

void
ptyxis_pane_zoom_in (PtyxisPane *self)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  ptyxis_terminal_surface_zoom_in (self->surface);
}

void
ptyxis_pane_zoom_out (PtyxisPane *self)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  ptyxis_terminal_surface_zoom_out (self->surface);
}

char *
ptyxis_pane_dup_zoom_label (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return ptyxis_terminal_surface_dup_zoom_label (self->surface);
}

char *
ptyxis_pane_dup_title (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return ptyxis_terminal_surface_dup_title (self->surface);
}

char *
ptyxis_pane_dup_subtitle (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return ptyxis_terminal_surface_dup_subtitle (self->surface);
}

char *
ptyxis_pane_dup_current_directory_uri (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return ptyxis_terminal_surface_dup_current_directory_uri (self->surface);
}

void
ptyxis_pane_set_previous_working_directory_uri (PtyxisPane *self,
                                                const char *uri)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  ptyxis_terminal_surface_set_previous_working_directory_uri (self->surface, uri);
}

PtyxisIpcContainer *
ptyxis_pane_dup_container (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return ptyxis_terminal_surface_dup_container (self->surface);
}

void
ptyxis_pane_set_container (PtyxisPane         *self,
                           PtyxisIpcContainer *container)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  ptyxis_terminal_surface_set_container (self->surface, container);
}

void
ptyxis_pane_grab_focus (PtyxisPane *self)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  ptyxis_terminal_surface_grab_focus (self->surface);
}

gboolean
ptyxis_pane_has_foreground_process (PtyxisPane  *self,
                                    GPid        *pid,
                                    char       **cmdline)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), FALSE);

  return ptyxis_terminal_surface_has_foreground_process (self->surface, pid, cmdline);
}

void
ptyxis_pane_force_quit (PtyxisPane *self)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  if (self->surface != NULL)
    ptyxis_terminal_surface_force_quit (self->surface);
}

const char *
ptyxis_pane_get_uuid (PtyxisPane *self)
{
  g_return_val_if_fail (PTYXIS_IS_PANE (self), NULL);

  return self->uuid;
}

void
ptyxis_pane_show_find_bar (PtyxisPane *self)
{
  g_return_if_fail (PTYXIS_IS_PANE (self));

  gtk_revealer_set_reveal_child (self->find_bar_revealer, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self->find_bar));
}


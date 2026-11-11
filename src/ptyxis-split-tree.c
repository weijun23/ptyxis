/*
 * ptyxis-split-tree.c
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
#include "ptyxis-profile.h"
#include "ptyxis-split-tree.h"
#include "ptyxis-terminal-surface.h"
#include "ptyxis-util.h"

/*
 * SplitNode — pure data structure for the split tree
 *
 * All panes and separator handles are direct children of
 * PtyxisSplitTree.  SplitNode only records the logical tree
 * so that size_allocate can position each widget with a
 * gsk_transform_translate.  Because panes are never
 * reparented, VTE terminals are never unrealized/realized
 * during split or close, which avoids cursor-position
 * corruption from text reflow.
 */

typedef struct _SplitNode SplitNode;

typedef struct _SplitBranch
{
  SplitNode      *start_child; /* (owned) (not nullable) */
  SplitNode      *end_child;   /* (owned) (not nullable) */
  GtkWidget      *handle;      /* (unowned) parented to PtyxisSplitTree */
  GtkOrientation  orientation;
  double          position;    /* 0.0–1.0 fraction for start child */
  double          drag_start_position;
  int             alloc_width;
  int             alloc_height;
  gboolean        position_user_set;
} SplitBranch;

struct _SplitNode
{
  gboolean is_leaf;
  union {
    PtyxisPane  *pane;   /* when is_leaf == TRUE */
    SplitBranch  branch; /* when is_leaf == FALSE */
  };
};

static SplitNode *
split_node_new_leaf (PtyxisPane *pane)
{
  SplitNode *node = g_new0 (SplitNode, 1);

  node->is_leaf = TRUE;
  node->pane = pane;

  return node;
}

static SplitNode *
split_node_new_branch (GtkOrientation  orientation,
                       SplitNode      *start_child,
                       SplitNode      *end_child,
                       GtkWidget      *handle)
{
  SplitNode *node = g_new0 (SplitNode, 1);

  node->is_leaf = FALSE;
  node->branch.start_child = start_child;
  node->branch.end_child = end_child;
  node->branch.handle = handle;
  node->branch.orientation = orientation;
  node->branch.position = 0.5;

  return node;
}

static void
split_node_free_recursive (SplitNode *node)
{
  if (node == NULL)
    return;

  if (!node->is_leaf)
    {
      split_node_free_recursive (node->branch.start_child);
      split_node_free_recursive (node->branch.end_child);
    }

  g_free (node);
}

static SplitNode *
find_leaf (SplitNode  *node,
           PtyxisPane *pane)
{
  SplitNode *result;

  if (node->is_leaf)
    return node->pane == pane ? node : NULL;

  result = find_leaf (node->branch.start_child, pane);
  if (result != NULL)
    return result;

  return find_leaf (node->branch.end_child, pane);
}

static SplitNode *
find_parent (SplitNode *root,
             SplitNode *child)
{
  SplitNode *result;

  if (root->is_leaf)
    return NULL;

  if (root->branch.start_child == child ||
      root->branch.end_child == child)
    return root;

  result = find_parent (root->branch.start_child, child);
  if (result != NULL)
    return result;

  return find_parent (root->branch.end_child, child);
}

static void
collect_panes_node (SplitNode *node,
                    GPtrArray *panes)
{
  if (node == NULL)
    return;

  if (node->is_leaf)
    {
      g_ptr_array_add (panes, node->pane);
      return;
    }

  collect_panes_node (node->branch.start_child, panes);
  collect_panes_node (node->branch.end_child, panes);
}

static void
set_node_widgets_visible (SplitNode  *node,
                          gboolean    visible,
                          PtyxisPane *except)
{
  if (node == NULL)
    return;

  if (node->is_leaf)
    {
      if (node->pane != except)
        gtk_widget_set_visible (GTK_WIDGET (node->pane), visible);
      return;
    }

  gtk_widget_set_visible (node->branch.handle, visible);
  set_node_widgets_visible (node->branch.start_child, visible, except);
  set_node_widgets_visible (node->branch.end_child, visible, except);
}

/*
 * Layout helpers
 */

static void
measure_node (SplitNode      *node,
              GtkOrientation  orientation,
              int             for_size,
              int            *minimum,
              int            *natural)
{
  SplitBranch *b;
  int start_min = 0, start_nat = 0;
  int end_min = 0, end_nat = 0;

  if (node->is_leaf)
    {
      gtk_widget_measure (GTK_WIDGET (node->pane), orientation, for_size,
                          minimum, natural, NULL, NULL);
      return;
    }

  b = &node->branch;

  measure_node (b->start_child, orientation, -1, &start_min, &start_nat);
  measure_node (b->end_child, orientation, -1, &end_min, &end_nat);

  if (orientation == b->orientation)
    {
      if (minimum != NULL)
        *minimum = start_min + end_min;
      if (natural != NULL)
        *natural = start_nat + end_nat;
    }
  else
    {
      if (minimum != NULL)
        *minimum = MAX (start_min, end_min);
      if (natural != NULL)
        *natural = MAX (start_nat, end_nat);
    }
}

static void
allocate_node (SplitNode *node,
               int        x,
               int        y,
               int        width,
               int        height)
{
  SplitBranch *b;
  int handle_size = 0;
  int total;
  int start_min = 0, end_min = 0;
  int start_size;
  int end_size;

  if (node->is_leaf)
    {
      gtk_widget_allocate (GTK_WIDGET (node->pane), width, height, -1,
                           gsk_transform_translate (NULL,
                                                    &GRAPHENE_POINT_INIT (x, y)));
      return;
    }

  b = &node->branch;

  b->alloc_width = width;
  b->alloc_height = height;

  gtk_widget_measure (b->handle, b->orientation, -1,
                      &handle_size, NULL, NULL, NULL);

  total = (b->orientation == GTK_ORIENTATION_HORIZONTAL) ? width : height;

  measure_node (b->start_child, b->orientation, -1, &start_min, NULL);
  measure_node (b->end_child, b->orientation, -1, &end_min, NULL);

  start_size = (int) (total * b->position);

  if (start_min + end_min <= total)
    start_size = CLAMP (start_size, start_min, total - end_min);

  end_size = total - start_size;

  if (b->orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      allocate_node (b->start_child, x, y, start_size, height);
      allocate_node (b->end_child, x + start_size, y, end_size, height);
      gtk_widget_allocate (b->handle, handle_size, height, -1,
                           gsk_transform_translate (NULL,
                                                    &GRAPHENE_POINT_INIT (x + start_size - handle_size / 2, y)));
    }
  else
    {
      allocate_node (b->start_child, x, y, width, start_size);
      allocate_node (b->end_child, x, y + start_size, width, end_size);
      gtk_widget_allocate (b->handle, width, handle_size, -1,
                           gsk_transform_translate (NULL,
                                                    &GRAPHENE_POINT_INIT (x, y + start_size - handle_size / 2)));
    }
}

/*
 * Rebalancing
 */

static int
count_leaves (SplitNode      *node,
              GtkOrientation  orientation)
{
  if (node->is_leaf)
    return 1;

  if (node->branch.orientation != orientation)
    return 1;

  return count_leaves (node->branch.start_child, orientation) +
         count_leaves (node->branch.end_child, orientation);
}

static void
rebalance_subtree (SplitNode *node)
{
  SplitBranch *b;
  int n_start;
  int n_end;

  if (node->is_leaf)
    return;

  b = &node->branch;

  if (!b->position_user_set)
    {
      n_start = count_leaves (b->start_child, b->orientation);
      n_end = count_leaves (b->end_child, b->orientation);

      b->position = (double) n_start / (double) (n_start + n_end);
    }

  rebalance_subtree (b->start_child);
  rebalance_subtree (b->end_child);
}

/*
 * PtyxisSplitTree
 */

struct _PtyxisSplitTree
{
  GtkWidget    parent_instance;

  SplitNode   *root;           /* (owned) (nullable) */
  PtyxisPane  *focused_pane;   /* (unowned) (nullable) */
  SplitNode   *saved_root;     /* (owned) (nullable) — non-NULL when maximized */
  gulong       focused_title_handler;
  double       drag_start_tree_coord;
  gboolean     needs_rebalance;
  size_t       n_panes;
};

enum {
  PROP_0,
  PROP_TITLE,
  PROP_FOCUSED_PANE,
  PROP_SINGLE_PANE,
  PROP_MAXIMIZED,
  N_PROPS
};

enum {
  EMPTY,
  N_SIGNALS
};

G_DEFINE_FINAL_TYPE (PtyxisSplitTree,
                     ptyxis_split_tree,
                     GTK_TYPE_WIDGET)

static GParamSpec *properties[N_PROPS];
static unsigned int signals[N_SIGNALS];

static void
set_n_panes (PtyxisSplitTree *self,
             size_t           n_panes)
{
  gboolean was_single = self->n_panes <= 1;
  gboolean is_single = n_panes <= 1;

  self->n_panes = n_panes;

  if (was_single != is_single)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SINGLE_PANE]);
}

static void
on_single_pane_changed (PtyxisSplitTree *self,
                        GParamSpec      *pspec,
                        PtyxisPane      *pane)
{
  gboolean single = self->n_panes <= 1;

  gtk_widget_action_set_enabled (GTK_WIDGET (pane), "pane.close", !single);
}

static void
bind_pane_controls (PtyxisSplitTree *self,
                    PtyxisPane      *pane)
{
  gboolean single = self->n_panes <= 1;

  gtk_widget_action_set_enabled (GTK_WIDGET (pane), "pane.close", !single);
  g_signal_connect_object (self, "notify::single-pane",
                           G_CALLBACK (on_single_pane_changed), pane, 0);
}

/*
 * Pane helpers
 */

static PtyxisPane *
create_pane_inheriting (PtyxisPane *source)
{
  PtyxisTerminalSurface *src_surface = ptyxis_pane_get_surface (source);
  PtyxisProfile *profile = ptyxis_terminal_surface_get_profile (src_surface);
  g_autoptr(PtyxisIpcContainer) container = ptyxis_terminal_surface_dup_container (src_surface);
  g_autofree char *cwd = ptyxis_terminal_surface_dup_current_directory_uri (src_surface);
  PtyxisPane *pane;
  PtyxisTerminalSurface *surface;

  pane = ptyxis_pane_new (profile);
  surface = ptyxis_pane_get_surface (pane);

  if (cwd != NULL)
    ptyxis_terminal_surface_set_previous_working_directory_uri (surface, cwd);

  if (container != NULL)
    ptyxis_terminal_surface_set_container (surface, container);

  ptyxis_terminal_surface_set_zoom (surface, ptyxis_terminal_surface_get_zoom (src_surface));

  return pane;
}

static void
on_focused_pane_notify_title (PtyxisPane      *pane,
                              GParamSpec      *pspec,
                              PtyxisSplitTree *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static void
set_focused_pane (PtyxisSplitTree *self,
                  PtyxisPane      *pane)
{
  if (self->focused_pane == pane)
    return;

  if (self->focused_pane != NULL)
    g_clear_signal_handler (&self->focused_title_handler, self->focused_pane);

  self->focused_pane = pane;

  if (self->focused_pane != NULL)
    self->focused_title_handler =
      g_signal_connect (self->focused_pane, "notify::title",
                        G_CALLBACK (on_focused_pane_notify_title), self);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FOCUSED_PANE]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static void
on_pane_close_request (PtyxisPane      *pane,
                       PtyxisSplitTree *self)
{
  set_focused_pane (self, pane);
  ptyxis_split_tree_close_pane (self);
}

static void
on_pane_focus_enter (GtkEventControllerFocus *focus,
                     PtyxisSplitTree         *self)
{
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (focus));

  if (PTYXIS_IS_PANE (widget))
    set_focused_pane (self, PTYXIS_PANE (widget));
}

static void
connect_pane (PtyxisSplitTree *self,
              PtyxisPane      *pane)
{
  GtkEventController *focus;

  g_signal_connect (pane, "close-request",
                    G_CALLBACK (on_pane_close_request), self);

  focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "enter",
                    G_CALLBACK (on_pane_focus_enter), self);
  gtk_widget_add_controller (GTK_WIDGET (pane), focus);
  g_object_set_data (G_OBJECT (pane), "split-tree-focus-controller", focus);
}

static void
disconnect_pane (PtyxisSplitTree *self,
                 PtyxisPane      *pane)
{
  GtkEventController *focus;

  if (self->focused_pane == pane)
    self->focused_title_handler = 0;

  g_signal_handlers_disconnect_by_data (pane, self);

  focus = g_object_get_data (G_OBJECT (pane), "split-tree-focus-controller");
  if (focus != NULL)
    {
      g_object_set_data (G_OBJECT (pane), "split-tree-focus-controller", NULL);
      gtk_widget_remove_controller (GTK_WIDGET (pane), focus);
    }
}

/*
 * Handle helpers
 */

static void
on_handle_drag_begin (GtkGestureDrag  *gesture,
                      double           start_x,
                      double           start_y,
                      PtyxisSplitTree *self)
{
  GtkWidget *handle = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  SplitNode *node = g_object_get_data (G_OBJECT (handle), "split-node");
  graphene_point_t tree_point;

  node->branch.drag_start_position = node->branch.position;

  if (!gtk_widget_compute_point (handle, GTK_WIDGET (self),
                                 &GRAPHENE_POINT_INIT ((float) start_x, (float) start_y),
                                 &tree_point))
    return;

  if (node->branch.orientation == GTK_ORIENTATION_HORIZONTAL)
    self->drag_start_tree_coord = tree_point.x;
  else
    self->drag_start_tree_coord = tree_point.y;
}

static void
on_handle_drag_update (GtkGestureDrag  *gesture,
                       double           offset_x,
                       double           offset_y,
                       PtyxisSplitTree *self)
{
  GtkWidget *handle = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  SplitNode *node = g_object_get_data (G_OBJECT (handle), "split-node");
  SplitBranch *b = &node->branch;
  GdkEventSequence *sequence;
  graphene_point_t tree_point;
  double current_x, current_y;
  double pixel_offset;
  double min_pos, max_pos;
  int total;
  int start_min = 0, end_min = 0;

  if (b->orientation == GTK_ORIENTATION_HORIZONTAL)
    total = b->alloc_width;
  else
    total = b->alloc_height;

  if (total <= 0)
    return;

  sequence = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  if (!gtk_gesture_get_point (GTK_GESTURE (gesture), sequence, &current_x, &current_y))
    return;

  if (!gtk_widget_compute_point (handle, GTK_WIDGET (self),
                                 &GRAPHENE_POINT_INIT ((float) current_x, (float) current_y),
                                 &tree_point))
    return;

  if (b->orientation == GTK_ORIENTATION_HORIZONTAL)
    pixel_offset = tree_point.x - self->drag_start_tree_coord;
  else
    pixel_offset = tree_point.y - self->drag_start_tree_coord;

  measure_node (b->start_child, b->orientation, -1, &start_min, NULL);
  measure_node (b->end_child, b->orientation, -1, &end_min, NULL);
  min_pos = (double) start_min / total;
  max_pos = 1.0 - (double) end_min / total;

  if (min_pos > max_pos)
    return;

  b->position = CLAMP (b->drag_start_position + pixel_offset / total, min_pos, max_pos);
  b->position_user_set = TRUE;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static GtkWidget *
create_handle (PtyxisSplitTree *self,
               GtkOrientation   orientation,
               SplitNode       *node)
{
  GtkWidget *handle;
  GtkGesture *gesture;
  GtkOrientation handle_orientation;

  handle_orientation = (orientation == GTK_ORIENTATION_HORIZONTAL)
                       ? GTK_ORIENTATION_VERTICAL
                       : GTK_ORIENTATION_HORIZONTAL;

  handle = gtk_separator_new (handle_orientation);
  gtk_widget_add_css_class (handle, "split-handle");
  gtk_widget_set_parent (handle, GTK_WIDGET (self));

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    gtk_widget_set_cursor_from_name (handle, "col-resize");
  else
    gtk_widget_set_cursor_from_name (handle, "row-resize");

  g_object_set_data (G_OBJECT (handle), "split-node", node);

  gesture = gtk_gesture_drag_new ();
  g_signal_connect (gesture, "drag-begin",
                    G_CALLBACK (on_handle_drag_begin), self);
  g_signal_connect (gesture, "drag-update",
                    G_CALLBACK (on_handle_drag_update), self);
  gtk_widget_add_controller (handle, GTK_EVENT_CONTROLLER (gesture));

  return handle;
}

/*
 * Split operations
 */

static void
split_pane (PtyxisSplitTree *self,
            GtkOrientation   orientation)
{
  PtyxisPane *target;
  PtyxisPane *new_pane;
  SplitNode *target_leaf;
  SplitNode *new_leaf;
  SplitNode *branch;
  SplitNode *parent;

  if (self->saved_root != NULL)
    return;

  target = self->focused_pane;
  if (target == NULL)
    return;

  target_leaf = find_leaf (self->root, target);
  if (target_leaf == NULL)
    return;

  new_pane = create_pane_inheriting (target);
  connect_pane (self, new_pane);
  gtk_widget_set_parent (GTK_WIDGET (new_pane), GTK_WIDGET (self));
  bind_pane_controls (self, new_pane);

  new_leaf = split_node_new_leaf (new_pane);

  /*
   * Wrap the target leaf and new leaf in a branch node.
   * The target pane stays parented to self — no reparenting,
   * no VTE unrealize/realize cycle.
   */
  branch = split_node_new_branch (orientation, target_leaf, new_leaf, NULL);
  branch->branch.handle = create_handle (self, orientation, branch);

  parent = find_parent (self->root, target_leaf);

  if (parent == NULL)
    self->root = branch;
  else if (parent->branch.start_child == target_leaf)
    parent->branch.start_child = branch;
  else
    parent->branch.end_child = branch;

  set_n_panes (self, self->n_panes + 1);

  self->needs_rebalance = TRUE;
  gtk_widget_queue_resize (GTK_WIDGET (self));

  ptyxis_pane_grab_focus (new_pane);
}

void
ptyxis_split_tree_close_pane (PtyxisSplitTree *self)
{
  g_autoptr(GPtrArray) panes = NULL;
  SplitNode *grandparent;
  PtyxisPane *target;
  SplitNode *target_leaf;
  SplitNode *parent;
  SplitNode *sibling;

  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  if (self->saved_root != NULL)
    ptyxis_split_tree_toggle_maximize (self);

  target = self->focused_pane;
  if (target == NULL)
    return;

  target_leaf = find_leaf (self->root, target);
  if (target_leaf == NULL)
    return;

  parent = find_parent (self->root, target_leaf);

  if (parent == NULL)
    {
      disconnect_pane (self, target);
      ptyxis_pane_force_quit (target);
      gtk_widget_unparent (GTK_WIDGET (target));
      g_free (target_leaf);
      self->root = NULL;
      g_clear_signal_handler (&self->focused_title_handler, self->focused_pane);
      self->focused_pane = NULL;
      self->n_panes = 0;
      g_signal_emit (self, signals[EMPTY], 0);
      return;
    }

  if (parent->branch.start_child == target_leaf)
    sibling = parent->branch.end_child;
  else
    sibling = parent->branch.start_child;

  disconnect_pane (self, target);
  ptyxis_pane_force_quit (target);
  gtk_widget_unparent (GTK_WIDGET (target));
  gtk_widget_unparent (parent->branch.handle);

  grandparent = find_parent (self->root, parent);

  if (grandparent == NULL)
    self->root = sibling;
  else if (grandparent->branch.start_child == parent)
    grandparent->branch.start_child = sibling;
  else
    grandparent->branch.end_child = sibling;

  g_free (target_leaf);
  g_free (parent);

  set_n_panes (self, self->n_panes - 1);

  self->needs_rebalance = TRUE;
  gtk_widget_queue_resize (GTK_WIDGET (self));

  panes = g_ptr_array_new ();
  collect_panes_node (sibling, panes);

  if (panes->len > 0)
    {
      PtyxisPane *new_focus = g_ptr_array_index (panes, 0);
      set_focused_pane (self, new_focus);
      ptyxis_pane_grab_focus (new_focus);
    }
}

void
ptyxis_split_tree_move_pane (PtyxisSplitTree *self,
                             PtyxisPane      *source,
                             PtyxisPane      *target,
                             GtkOrientation   orientation,
                             gboolean         after)
{
  SplitNode *grandparent;
  SplitNode *source_leaf;
  SplitNode *source_parent;
  SplitNode *target_leaf;
  SplitNode *target_parent;
  SplitNode *sibling;
  SplitNode *new_leaf;
  SplitNode *branch;

  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));
  g_return_if_fail (PTYXIS_IS_PANE (source));
  g_return_if_fail (PTYXIS_IS_PANE (target));
  g_return_if_fail (source != target);

  if (self->saved_root != NULL)
    ptyxis_split_tree_toggle_maximize (self);

  source_leaf = find_leaf (self->root, source);
  if (source_leaf == NULL)
    return;

  source_parent = find_parent (self->root, source_leaf);
  if (source_parent == NULL)
    return;

  /*
   * Detach source from the tree by promoting its sibling.
   */
  if (source_parent->branch.start_child == source_leaf)
    sibling = source_parent->branch.end_child;
  else
    sibling = source_parent->branch.start_child;

  gtk_widget_unparent (source_parent->branch.handle);

  grandparent = find_parent (self->root, source_parent);

  if (grandparent == NULL)
    self->root = sibling;
  else if (grandparent->branch.start_child == source_parent)
    grandparent->branch.start_child = sibling;
  else
    grandparent->branch.end_child = sibling;

  source_parent->branch.start_child = NULL;
  source_parent->branch.end_child = NULL;
  g_free (source_parent);

  /*
   * Now find the target leaf again — the tree may have changed.
   */
  target_leaf = find_leaf (self->root, target);
  if (target_leaf == NULL)
    {
      g_free (source_leaf);
      return;
    }

  target_parent = find_parent (self->root, target_leaf);

  /*
   * Insert source next to target.
   */
  new_leaf = source_leaf;

  if (after)
    branch = split_node_new_branch (orientation, target_leaf, new_leaf, NULL);
  else
    branch = split_node_new_branch (orientation, new_leaf, target_leaf, NULL);

  branch->branch.handle = create_handle (self, orientation, branch);

  if (target_parent == NULL)
    self->root = branch;
  else if (target_parent->branch.start_child == target_leaf)
    target_parent->branch.start_child = branch;
  else
    target_parent->branch.end_child = branch;

  self->needs_rebalance = TRUE;
  gtk_widget_queue_resize (GTK_WIDGET (self));

  set_focused_pane (self, source);
  ptyxis_pane_grab_focus (source);
}

gboolean
ptyxis_split_tree_detach_pane (PtyxisSplitTree *self,
                               PtyxisPane      *pane)
{
  g_autoptr(GPtrArray) panes = NULL;
  SplitNode *grandparent;
  SplitNode *leaf;
  SplitNode *parent;
  SplitNode *sibling;

  g_return_val_if_fail (PTYXIS_IS_SPLIT_TREE (self), FALSE);
  g_return_val_if_fail (PTYXIS_IS_PANE (pane), FALSE);

  if (self->saved_root != NULL)
    ptyxis_split_tree_toggle_maximize (self);

  leaf = find_leaf (self->root, pane);
  if (leaf == NULL)
    return FALSE;

  disconnect_pane (self, pane);
  gtk_widget_unparent (GTK_WIDGET (pane));

  parent = find_parent (self->root, leaf);
  if (parent == NULL)
    {
      g_free (leaf);
      self->root = NULL;
      g_clear_signal_handler (&self->focused_title_handler, self->focused_pane);
      self->focused_pane = NULL;
      self->n_panes = 0;

      g_signal_emit (self, signals[EMPTY], 0);
      return TRUE;
    }

  if (parent->branch.start_child == leaf)
    sibling = parent->branch.end_child;
  else
    sibling = parent->branch.start_child;

  gtk_widget_unparent (parent->branch.handle);

  grandparent = find_parent (self->root, parent);

  if (grandparent == NULL)
    self->root = sibling;
  else if (grandparent->branch.start_child == parent)
    grandparent->branch.start_child = sibling;
  else
    grandparent->branch.end_child = sibling;

  g_free (leaf);
  g_free (parent);

  set_n_panes (self, self->n_panes - 1);

  self->needs_rebalance = TRUE;
  gtk_widget_queue_resize (GTK_WIDGET (self));

  panes = g_ptr_array_new ();
  collect_panes_node (self->root, panes);

  if (panes->len > 0)
    {
      PtyxisPane *new_focus = g_ptr_array_index (panes, 0);
      set_focused_pane (self, new_focus);
      ptyxis_pane_grab_focus (new_focus);
    }

  return TRUE;
}

void
ptyxis_split_tree_insert_pane (PtyxisSplitTree *self,
                               PtyxisPane      *pane,
                               PtyxisPane      *target,
                               GtkOrientation   orientation,
                               gboolean         after)
{
  SplitNode *new_leaf;
  SplitNode *target_leaf;
  SplitNode *target_parent;
  SplitNode *branch;

  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));
  g_return_if_fail (PTYXIS_IS_PANE (pane));
  g_return_if_fail (PTYXIS_IS_PANE (target));

  if (self->saved_root != NULL)
    ptyxis_split_tree_toggle_maximize (self);

  connect_pane (self, pane);
  gtk_widget_set_parent (GTK_WIDGET (pane), GTK_WIDGET (self));
  bind_pane_controls (self, pane);

  new_leaf = split_node_new_leaf (pane);

  target_leaf = find_leaf (self->root, target);
  if (target_leaf == NULL)
    {
      g_free (new_leaf);
      return;
    }

  target_parent = find_parent (self->root, target_leaf);

  if (after)
    branch = split_node_new_branch (orientation, target_leaf, new_leaf, NULL);
  else
    branch = split_node_new_branch (orientation, new_leaf, target_leaf, NULL);

  branch->branch.handle = create_handle (self, orientation, branch);

  if (target_parent == NULL)
    self->root = branch;
  else if (target_parent->branch.start_child == target_leaf)
    target_parent->branch.start_child = branch;
  else
    target_parent->branch.end_child = branch;

  set_n_panes (self, self->n_panes + 1);

  self->needs_rebalance = TRUE;
  gtk_widget_queue_resize (GTK_WIDGET (self));

  set_focused_pane (self, pane);
  ptyxis_pane_grab_focus (pane);
}

/*
 * Focus navigation
 */

typedef enum _FocusDirection
{
  FOCUS_UP,
  FOCUS_DOWN,
  FOCUS_LEFT,
  FOCUS_RIGHT,
} FocusDirection;

typedef struct _PaneBounds
{
  double x, y, w, h;
} PaneBounds;

static gboolean
get_pane_bounds (PtyxisPane      *pane,
                 PtyxisSplitTree *tree,
                 PaneBounds      *bounds)
{
  graphene_point_t origin;

  if (!gtk_widget_compute_point (GTK_WIDGET (pane),
                                 GTK_WIDGET (tree),
                                 &GRAPHENE_POINT_INIT (0, 0),
                                 &origin))
    return FALSE;

  bounds->x = origin.x;
  bounds->y = origin.y;
  bounds->w = gtk_widget_get_width (GTK_WIDGET (pane));
  bounds->h = gtk_widget_get_height (GTK_WIDGET (pane));

  return TRUE;
}

static double
ranges_overlap (double a_start, double a_end,
                double b_start, double b_end)
{
  double overlap_start = MAX (a_start, b_start);
  double overlap_end = MIN (a_end, b_end);

  return MAX (0, overlap_end - overlap_start);
}

static gboolean
compute_direction_metrics (PaneBounds     *src,
                           PaneBounds     *dst,
                           FocusDirection  direction,
                           double         *out_overlap,
                           double         *out_dist)
{
  double overlap, dist;

  switch (direction)
    {
    case FOCUS_LEFT:
      if (dst->x >= src->x)
        return FALSE;
      overlap = ranges_overlap (src->y, src->y + src->h,
                                dst->y, dst->y + dst->h);
      dist = src->x - (dst->x + dst->w);
      break;
    case FOCUS_RIGHT:
      if (dst->x + dst->w <= src->x + src->w)
        return FALSE;
      overlap = ranges_overlap (src->y, src->y + src->h,
                                dst->y, dst->y + dst->h);
      dist = dst->x - (src->x + src->w);
      break;
    case FOCUS_UP:
      if (dst->y >= src->y)
        return FALSE;
      overlap = ranges_overlap (src->x, src->x + src->w,
                                dst->x, dst->x + dst->w);
      dist = src->y - (dst->y + dst->h);
      break;
    case FOCUS_DOWN:
      if (dst->y + dst->h <= src->y + src->h)
        return FALSE;
      overlap = ranges_overlap (src->x, src->x + src->w,
                                dst->x, dst->x + dst->w);
      dist = dst->y - (src->y + src->h);
      break;
    default:
      g_assert_not_reached ();
    }

  if (dist < 0)
    dist = 0;

  *out_overlap = overlap;
  *out_dist = dist;

  return TRUE;
}

static void
focus_direction (PtyxisSplitTree *self,
                 FocusDirection   direction)
{
  g_autoptr(GPtrArray) panes = NULL;
  PaneBounds src;
  double min_dist = G_MAXDOUBLE;
  double best_overlap = 0;
  PtyxisPane *best = NULL;

  if (self->focused_pane == NULL)
    return;

  panes = g_ptr_array_new ();
  collect_panes_node (self->root, panes);

  if (panes->len <= 1)
    return;

  if (!get_pane_bounds (self->focused_pane, self, &src))
    return;

  /* First pass: find the nearest distance tier */
  for (size_t i = 0; i < panes->len; i++)
    {
      PtyxisPane *pane = g_ptr_array_index (panes, i);
      PaneBounds dst;
      double overlap, dist;

      if (pane == self->focused_pane)
        continue;

      if (!get_pane_bounds (pane, self, &dst))
        continue;

      if (!compute_direction_metrics (&src, &dst, direction, &overlap, &dist))
        continue;

      if (overlap > 0 && dist < min_dist)
        min_dist = dist;
    }

  /* Second pass: pick the best overlap among nearest candidates */
  for (size_t i = 0; i < panes->len; i++)
    {
      PtyxisPane *pane = g_ptr_array_index (panes, i);
      PaneBounds dst;
      double overlap, dist;

      if (pane == self->focused_pane)
        continue;

      if (!get_pane_bounds (pane, self, &dst))
        continue;

      if (!compute_direction_metrics (&src, &dst, direction, &overlap, &dist))
        continue;

      if (dist > min_dist + 1)
        continue;

      if (overlap > best_overlap + 1)
        {
          best_overlap = overlap;
          best = pane;
        }
    }

  if (best != NULL)
    {
      set_focused_pane (self, best);
      ptyxis_pane_grab_focus (best);
    }
}

/*
 * GObject
 */

static void
ptyxis_split_tree_dispose (GObject *object)
{
  PtyxisSplitTree *self = (PtyxisSplitTree *) object;
  g_autoptr(GPtrArray) panes = NULL;
  GtkWidget *child;

  if (self->focused_pane != NULL)
    g_clear_signal_handler (&self->focused_title_handler, self->focused_pane);
  self->focused_pane = NULL;

  panes = g_ptr_array_new ();

  if (self->root != NULL)
    collect_panes_node (self->root, panes);
  if (self->saved_root != NULL)
    collect_panes_node (self->saved_root, panes);

  for (size_t i = 0; i < panes->len; i++)
    disconnect_pane (self, g_ptr_array_index (panes, i));

  g_clear_pointer (&self->root, split_node_free_recursive);
  g_clear_pointer (&self->saved_root, split_node_free_recursive);

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))) != NULL)
    gtk_widget_unparent (child);

  G_OBJECT_CLASS (ptyxis_split_tree_parent_class)->dispose (object);
}

static void
ptyxis_split_tree_get_property (GObject      *object,
                                unsigned int  prop_id,
                                GValue       *value,
                                GParamSpec   *pspec)
{
  PtyxisSplitTree *self = PTYXIS_SPLIT_TREE (object);

  switch (prop_id)
    {
    case PROP_TITLE:
      if (self->focused_pane != NULL)
        g_value_take_string (value, ptyxis_pane_dup_title (self->focused_pane));
      else
        g_value_set_static_string (value, NULL);
      break;

    case PROP_FOCUSED_PANE:
      g_value_set_object (value, self->focused_pane);
      break;

    case PROP_SINGLE_PANE:
      g_value_set_boolean (value, self->n_panes <= 1);
      break;

    case PROP_MAXIMIZED:
      g_value_set_boolean (value, self->saved_root != NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_split_tree_measure (GtkWidget      *widget,
                           GtkOrientation  orientation,
                           int             for_size,
                           int            *minimum,
                           int            *natural,
                           int            *minimum_baseline,
                           int            *natural_baseline)
{
  PtyxisSplitTree *self = PTYXIS_SPLIT_TREE (widget);

  if (self->root != NULL)
    measure_node (self->root, orientation, for_size, minimum, natural);
}

static void
raise_handles (GtkWidget *parent,
               SplitNode *node)
{
  if (node == NULL || node->is_leaf)
    return;

  raise_handles (parent, node->branch.start_child);
  raise_handles (parent, node->branch.end_child);

  /* Move handle to end of sibling list so it renders and picks
   * on top of all panes */
  gtk_widget_insert_before (node->branch.handle, parent, NULL);
}

static void
ptyxis_split_tree_size_allocate (GtkWidget *widget,
                                 int        width,
                                 int        height,
                                 int        baseline)
{
  PtyxisSplitTree *self = PTYXIS_SPLIT_TREE (widget);

  if (self->needs_rebalance && self->root != NULL)
    {
      rebalance_subtree (self->root);
      self->needs_rebalance = FALSE;
    }

  if (self->root != NULL)
    {
      raise_handles (widget, self->root);
      allocate_node (self->root, 0, 0, width, height);
    }
}

static void
ptyxis_split_tree_class_init (PtyxisSplitTreeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ptyxis_split_tree_dispose;
  object_class->get_property = ptyxis_split_tree_get_property;
  widget_class->measure = ptyxis_split_tree_measure;
  widget_class->size_allocate = ptyxis_split_tree_size_allocate;

  properties[PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_FOCUSED_PANE] =
    g_param_spec_object ("focused-pane", NULL, NULL,
                         PTYXIS_TYPE_PANE,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_SINGLE_PANE] =
    g_param_spec_boolean ("single-pane", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_MAXIMIZED] =
    g_param_spec_boolean ("maximized", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[EMPTY] =
    g_signal_new ("empty",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
ptyxis_split_tree_init (PtyxisSplitTree *self)
{
}

/*
 * Public API
 */

PtyxisSplitTree *
ptyxis_split_tree_new (PtyxisProfile *profile)
{
  PtyxisSplitTree *self;
  PtyxisPane *pane;

  g_return_val_if_fail (PTYXIS_IS_PROFILE (profile), NULL);

  self = g_object_new (PTYXIS_TYPE_SPLIT_TREE, NULL);

  pane = ptyxis_pane_new (profile);
  connect_pane (self, pane);
  gtk_widget_set_parent (GTK_WIDGET (pane), GTK_WIDGET (self));

  self->root = split_node_new_leaf (pane);
  self->focused_pane = pane;
  self->focused_title_handler =
    g_signal_connect (pane, "notify::title",
                      G_CALLBACK (on_focused_pane_notify_title), self);
  set_n_panes (self, 1);
  bind_pane_controls (self, pane);

  return self;
}

PtyxisSplitTree *
ptyxis_split_tree_new_with_pane (PtyxisPane *pane)
{
  PtyxisSplitTree *self;

  g_return_val_if_fail (PTYXIS_IS_PANE (pane), NULL);

  self = g_object_new (PTYXIS_TYPE_SPLIT_TREE, NULL);

  connect_pane (self, pane);
  gtk_widget_set_parent (GTK_WIDGET (pane), GTK_WIDGET (self));

  self->root = split_node_new_leaf (pane);
  self->focused_pane = pane;
  self->focused_title_handler =
    g_signal_connect (pane, "notify::title",
                      G_CALLBACK (on_focused_pane_notify_title), self);
  set_n_panes (self, 1);
  bind_pane_controls (self, pane);

  return self;
}

PtyxisPane *
ptyxis_split_tree_get_focused_pane (PtyxisSplitTree *self)
{
  g_return_val_if_fail (PTYXIS_IS_SPLIT_TREE (self), NULL);

  return self->focused_pane;
}

void
ptyxis_split_tree_split_down (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  split_pane (self, GTK_ORIENTATION_VERTICAL);
}

void
ptyxis_split_tree_split_right (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  split_pane (self, GTK_ORIENTATION_HORIZONTAL);
}

void
ptyxis_split_tree_focus_up (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  focus_direction (self, FOCUS_UP);
}

void
ptyxis_split_tree_focus_down (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  focus_direction (self, FOCUS_DOWN);
}

void
ptyxis_split_tree_focus_left (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  focus_direction (self, FOCUS_LEFT);
}

void
ptyxis_split_tree_focus_right (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  focus_direction (self, FOCUS_RIGHT);
}

void
ptyxis_split_tree_toggle_maximize (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  if (self->focused_pane == NULL)
    return;

  if (self->n_panes <= 1)
    return;

  if (self->saved_root == NULL)
    {
      self->saved_root = self->root;
      self->root = split_node_new_leaf (self->focused_pane);

      set_node_widgets_visible (self->saved_root, FALSE, self->focused_pane);
    }
  else
    {
      g_free (self->root);

      self->root = self->saved_root;
      self->saved_root = NULL;

      set_node_widgets_visible (self->root, TRUE, NULL);

      ptyxis_pane_grab_focus (self->focused_pane);
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MAXIMIZED]);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

size_t
ptyxis_split_tree_get_n_panes (PtyxisSplitTree *self)
{
  g_return_val_if_fail (PTYXIS_IS_SPLIT_TREE (self), 0);

  return self->n_panes;
}

void
ptyxis_split_tree_foreach_pane (PtyxisSplitTree *self,
                                GFunc            func,
                                gpointer         user_data)
{
  g_autoptr(GPtrArray) panes = NULL;

  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));
  g_return_if_fail (func != NULL);

  panes = g_ptr_array_new ();
  collect_panes_node (self->root, panes);

  for (size_t i = 0; i < panes->len; i++)
    func (g_ptr_array_index (panes, i), user_data);
}

gboolean
ptyxis_split_tree_get_maximized (PtyxisSplitTree *self)
{
  g_return_val_if_fail (PTYXIS_IS_SPLIT_TREE (self), FALSE);

  return self->saved_root != NULL;
}

void
ptyxis_split_tree_grab_focus (PtyxisSplitTree *self)
{
  g_return_if_fail (PTYXIS_IS_SPLIT_TREE (self));

  if (self->focused_pane != NULL)
    ptyxis_pane_grab_focus (self->focused_pane);
}

static GVariant *
split_node_to_variant (SplitNode *node)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  if (node->is_leaf)
    {
      PtyxisTerminalSurface *surface;

      surface = ptyxis_pane_get_surface (node->pane);

      g_variant_builder_add_parsed (&builder, "{'type', <'leaf'>}");
      ptyxis_terminal_surface_serialize (surface, &builder);
    }
  else
    {
      GVariant *start_child;
      GVariant *end_child;
      const char *orientation;

      start_child = split_node_to_variant (node->branch.start_child);
      end_child = split_node_to_variant (node->branch.end_child);
      orientation = (node->branch.orientation == GTK_ORIENTATION_HORIZONTAL)
                    ? "horizontal"
                    : "vertical";

      g_variant_builder_add_parsed (&builder, "{'type', <'branch'>}");
      g_variant_builder_add_parsed (&builder, "{'orientation', <%s>}", orientation);
      g_variant_builder_add_parsed (&builder, "{'position', <%i>}",
                                    (int) (node->branch.position * 10000));
      g_variant_builder_add (&builder, "{sv}", "start", start_child);
      g_variant_builder_add (&builder, "{sv}", "end", end_child);
    }

  return g_variant_builder_end (&builder);
}

GVariant *
ptyxis_split_tree_to_variant (PtyxisSplitTree *self)
{
  g_return_val_if_fail (PTYXIS_IS_SPLIT_TREE (self), NULL);

  if (self->root == NULL)
    return NULL;

  return split_node_to_variant (self->root);
}

static void
split_node_teardown_recursive (PtyxisSplitTree *self,
                               SplitNode       *node)
{
  if (node == NULL)
    return;

  if (node->is_leaf)
    {
      disconnect_pane (self, node->pane);
      gtk_widget_unparent (GTK_WIDGET (node->pane));
    }
  else
    {
      split_node_teardown_recursive (self, node->branch.start_child);
      split_node_teardown_recursive (self, node->branch.end_child);
    }

  g_free (node);
}

static SplitNode *
split_node_from_variant (PtyxisSplitTree *self,
                         GVariant        *variant,
                         PtyxisPane     **focused)
{
  const char *type;

  if (!g_variant_lookup (variant, "type", "&s", &type))
    return NULL;

  if (g_strcmp0 (type, "leaf") == 0)
    {
      g_autoptr(PtyxisProfile) profile = NULL;
      PtyxisApplication *app = PTYXIS_APPLICATION_DEFAULT;
      PtyxisTerminalSurface *surface;
      PtyxisPane *pane;
      const char *profile_uuid;

      if (!g_variant_lookup (variant, "profile", "&s", &profile_uuid))
        profile_uuid = NULL;

      if (!ptyxis_str_empty0 (profile_uuid))
        profile = ptyxis_application_dup_profile (app, profile_uuid);

      if (profile == NULL)
        profile = ptyxis_application_dup_default_profile (app);

      pane = ptyxis_pane_new (profile);
      surface = ptyxis_pane_get_surface (pane);
      ptyxis_terminal_surface_deserialize (surface, variant);

      connect_pane (self, pane);
      gtk_widget_set_parent (GTK_WIDGET (pane), GTK_WIDGET (self));
      bind_pane_controls (self, pane);

      if (*focused == NULL)
        *focused = pane;

      return split_node_new_leaf (pane);
    }
  else if (g_strcmp0 (type, "branch") == 0)
    {
      g_autoptr(GVariant) start_variant = NULL;
      g_autoptr(GVariant) end_variant = NULL;
      SplitNode *start_child;
      SplitNode *end_child;
      SplitNode *branch;
      GtkOrientation orientation;
      const char *orientation_str;
      int position_scaled;

      if (!g_variant_lookup (variant, "orientation", "&s", &orientation_str))
        return NULL;

      orientation = (g_strcmp0 (orientation_str, "horizontal") == 0)
                    ? GTK_ORIENTATION_HORIZONTAL
                    : GTK_ORIENTATION_VERTICAL;

      if (!g_variant_lookup (variant, "position", "i", &position_scaled))
        position_scaled = 5000;

      start_variant = g_variant_lookup_value (variant, "start", G_VARIANT_TYPE ("a{sv}"));
      end_variant = g_variant_lookup_value (variant, "end", G_VARIANT_TYPE ("a{sv}"));

      if (start_variant == NULL || end_variant == NULL)
        return NULL;

      start_child = split_node_from_variant (self, start_variant, focused);
      if (start_child == NULL)
        return NULL;

      end_child = split_node_from_variant (self, end_variant, focused);
      if (end_child == NULL)
        {
          split_node_teardown_recursive (self, start_child);
          return NULL;
        }

      branch = split_node_new_branch (orientation, start_child, end_child, NULL);
      branch->branch.position = CLAMP (position_scaled / 10000.0, 0.05, 0.95);
      branch->branch.position_user_set = TRUE;
      branch->branch.handle = create_handle (self, orientation, branch);

      return branch;
    }

  return NULL;
}

PtyxisSplitTree *
ptyxis_split_tree_new_from_variant (GVariant *variant)
{
  g_autoptr(GPtrArray) panes = NULL;
  PtyxisSplitTree *self;
  PtyxisPane *focused = NULL;
  SplitNode *root;

  g_return_val_if_fail (variant != NULL, NULL);
  g_return_val_if_fail (g_variant_is_of_type (variant, G_VARIANT_TYPE ("a{sv}")), NULL);

  self = g_object_new (PTYXIS_TYPE_SPLIT_TREE, NULL);

  root = split_node_from_variant (self, variant, &focused);
  if (root == NULL)
    {
      g_object_unref (self);
      return NULL;
    }

  self->root = root;
  self->focused_pane = focused;

  if (focused != NULL)
    {
      self->focused_title_handler =
        g_signal_connect (focused, "notify::title",
                          G_CALLBACK (on_focused_pane_notify_title), self);
    }

  panes = g_ptr_array_new ();
  collect_panes_node (root, panes);
  set_n_panes (self, panes->len);

  return self;
}

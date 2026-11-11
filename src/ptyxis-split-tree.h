/*
 * ptyxis-split-tree.h
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

#pragma once

#include <gtk/gtk.h>

#include "ptyxis-pane.h"

G_BEGIN_DECLS

#define PTYXIS_TYPE_SPLIT_TREE (ptyxis_split_tree_get_type())

G_DECLARE_FINAL_TYPE (PtyxisSplitTree,
                      ptyxis_split_tree,
                      PTYXIS, SPLIT_TREE,
                      GtkWidget)

PtyxisSplitTree * ptyxis_split_tree_new               (PtyxisProfile   *profile);
PtyxisSplitTree * ptyxis_split_tree_new_with_pane    (PtyxisPane      *pane);
PtyxisSplitTree * ptyxis_split_tree_new_from_variant (GVariant        *variant);
GVariant        * ptyxis_split_tree_to_variant       (PtyxisSplitTree *self);
PtyxisPane      * ptyxis_split_tree_get_focused_pane (PtyxisSplitTree *self);
void              ptyxis_split_tree_split_down       (PtyxisSplitTree *self);
void              ptyxis_split_tree_split_right      (PtyxisSplitTree *self);
void              ptyxis_split_tree_close_pane       (PtyxisSplitTree *self);
void              ptyxis_split_tree_focus_up         (PtyxisSplitTree *self);
void              ptyxis_split_tree_focus_down       (PtyxisSplitTree *self);
void              ptyxis_split_tree_focus_left       (PtyxisSplitTree *self);
void              ptyxis_split_tree_focus_right      (PtyxisSplitTree *self);
void              ptyxis_split_tree_toggle_maximize   (PtyxisSplitTree *self);
void              ptyxis_split_tree_move_pane         (PtyxisSplitTree *self,
                                                       PtyxisPane      *source,
                                                       PtyxisPane      *target,
                                                       GtkOrientation   orientation,
                                                       gboolean         after);
gboolean          ptyxis_split_tree_detach_pane       (PtyxisSplitTree *self,
                                                       PtyxisPane      *pane);
void              ptyxis_split_tree_insert_pane       (PtyxisSplitTree *self,
                                                       PtyxisPane      *pane,
                                                       PtyxisPane      *target,
                                                       GtkOrientation   orientation,
                                                       gboolean         after);
size_t            ptyxis_split_tree_get_n_panes      (PtyxisSplitTree *self);
void              ptyxis_split_tree_foreach_pane     (PtyxisSplitTree *self,
                                                      GFunc            func,
                                                      gpointer         user_data);
gboolean          ptyxis_split_tree_get_maximized    (PtyxisSplitTree *self);
void              ptyxis_split_tree_grab_focus       (PtyxisSplitTree *self);

G_END_DECLS

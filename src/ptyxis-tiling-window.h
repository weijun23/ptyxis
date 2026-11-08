/* ptyxis-tiling-window.h
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

#include "ptyxis-pane.h"
#include "ptyxis-split-tree.h"
#include "ptyxis-window.h"

G_BEGIN_DECLS

#define PTYXIS_TYPE_TILING_WINDOW (ptyxis_tiling_window_get_type())

G_DECLARE_FINAL_TYPE (PtyxisTilingWindow, ptyxis_tiling_window, PTYXIS, TILING_WINDOW, PtyxisWindow)

PtyxisTilingWindow *ptyxis_tiling_window_new                (PtyxisProfile   *profile);
PtyxisTilingWindow *ptyxis_tiling_window_new_with_pane      (PtyxisPane      *pane);
PtyxisTilingWindow *ptyxis_tiling_window_new_from_split_tree (PtyxisSplitTree *split_tree);
PtyxisSplitTree    *ptyxis_tiling_window_get_split_tree     (PtyxisTilingWindow *self);

G_END_DECLS

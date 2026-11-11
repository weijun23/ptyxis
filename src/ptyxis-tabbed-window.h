/* ptyxis-tabbed-window.h
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

#pragma once

#include "ptyxis-tab.h"
#include "ptyxis-window.h"

G_BEGIN_DECLS

#define PTYXIS_TYPE_TABBED_WINDOW (ptyxis_tabbed_window_get_type())

G_DECLARE_FINAL_TYPE (PtyxisTabbedWindow, ptyxis_tabbed_window, PTYXIS, TABBED_WINDOW, PtyxisWindow)

PtyxisTabbedWindow *ptyxis_tabbed_window_new                 (void);
PtyxisTabbedWindow *ptyxis_tabbed_window_new_for_profile     (PtyxisProfile      *profile);
PtyxisTabbedWindow *ptyxis_tabbed_window_new_for_command     (PtyxisProfile      *profile,
                                                              const char * const *argv,
                                                              const char         *cwd_uri);
void                ptyxis_tabbed_window_add_tab             (PtyxisTabbedWindow *self,
                                                              PtyxisTab          *tab);
void                ptyxis_tabbed_window_add_tab_at_end      (PtyxisTabbedWindow *self,
                                                              PtyxisTab          *tab);
void                ptyxis_tabbed_window_append_tab          (PtyxisTabbedWindow *self,
                                                              PtyxisTab          *tab);
PtyxisTab          *ptyxis_tabbed_window_get_active_tab      (PtyxisTabbedWindow *self);
void                ptyxis_tabbed_window_set_active_tab      (PtyxisTabbedWindow *self,
                                                              PtyxisTab          *active_tab);
GListModel         *ptyxis_tabbed_window_list_pages          (PtyxisTabbedWindow *self);
gboolean            ptyxis_tabbed_window_focus_tab_by_uuid   (PtyxisTabbedWindow *self,
                                                              const char         *uuid);
gboolean            ptyxis_tabbed_window_is_animating        (PtyxisTabbedWindow *self);
void                ptyxis_tabbed_window_set_tab_pinned      (PtyxisTabbedWindow *self,
                                                              PtyxisTab          *tab,
                                                              gboolean            pinned);

G_END_DECLS

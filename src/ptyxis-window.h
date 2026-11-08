/* ptyxis-window.h
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

#include <adwaita.h>

#include "ptyxis-profile.h"
#include "ptyxis-tab.h"
#include "ptyxis-terminal-surface.h"

G_BEGIN_DECLS

#define PTYXIS_TYPE_WINDOW (ptyxis_window_get_type())

G_DECLARE_DERIVABLE_TYPE (PtyxisWindow, ptyxis_window, PTYXIS, WINDOW, AdwApplicationWindow)

struct _PtyxisWindowClass
{
  AdwApplicationWindowClass parent_class;

  PtyxisTerminalSurface * (*get_active_surface)  (PtyxisWindow  *self);
  PtyxisTerminalSurface * (*add_terminal)        (PtyxisWindow  *self,
                                                   PtyxisProfile *profile);
  void                    (*collect_surfaces)     (PtyxisWindow  *self,
                                                   GPtrArray     *surfaces);
  gboolean                (*focus_by_uuid)        (PtyxisWindow  *self,
                                                   const char    *uuid);
  guint                   (*get_n_pages)          (PtyxisWindow  *self);
  GVariant              * (*serialize)            (PtyxisWindow  *self);
  gboolean                (*deserialize)          (PtyxisWindow  *self,
                                                   GVariant      *state);
  void                    (*configure_popover)    (PtyxisWindow  *self,
                                                   GtkWidget     *popover);
  void                    (*open_new_window)      (PtyxisWindow  *self,
                                                   PtyxisProfile *profile,
                                                   const char    *container_id);
  gboolean                (*resize_for_zoom)      (PtyxisWindow  *self);
  void                    (*surface_needs_attention) (PtyxisWindow          *self,
                                                      PtyxisTerminalSurface *surface);
  GtkWindow             * (*create_preferences_window) (PtyxisWindow *self);
};

PtyxisWindow          *ptyxis_window_new                 (void);
PtyxisWindow          *ptyxis_window_new_empty           (void);
PtyxisWindow          *ptyxis_window_new_for_command     (PtyxisProfile      *profile,
                                                          const char * const *argv,
                                                          const char         *cwd_uri);
PtyxisWindow          *ptyxis_window_new_for_profile     (PtyxisProfile      *profile);
void                   ptyxis_window_add_tab             (PtyxisWindow       *self,
                                                          PtyxisTab          *tab);
void                   ptyxis_window_add_tab_at_end      (PtyxisWindow       *self,
                                                          PtyxisTab          *tab);
PtyxisTab             *ptyxis_window_add_tab_for_command (PtyxisWindow       *self,
                                                          PtyxisProfile      *profile,
                                                          const char * const *argv,
                                                          const char         *cwd_uri);
GListModel            *ptyxis_window_list_pages          (PtyxisWindow       *self);
void                   ptyxis_window_append_tab          (PtyxisWindow       *self,
                                                          PtyxisTab          *tab);
PtyxisProfile         *ptyxis_window_get_active_profile  (PtyxisWindow       *self);
PtyxisTab             *ptyxis_window_get_active_tab      (PtyxisWindow       *self);
void                   ptyxis_window_set_active_tab      (PtyxisWindow       *self,
                                                          PtyxisTab          *active_tab);
void                   ptyxis_window_visual_bell         (PtyxisWindow       *self);
gboolean               ptyxis_window_focus_tab_by_uuid   (PtyxisWindow       *self,
                                                          const char         *uuid);
gboolean               ptyxis_window_is_animating        (PtyxisWindow       *self);
void                   ptyxis_window_set_tab_pinned      (PtyxisWindow       *self,
                                                          PtyxisTab          *tab,
                                                          gboolean            pinned);
PtyxisTerminalSurface *ptyxis_window_get_active_surface  (PtyxisWindow       *self);
PtyxisTerminalSurface *ptyxis_window_add_terminal        (PtyxisWindow       *self,
                                                          PtyxisProfile      *profile);
void                   ptyxis_window_collect_surfaces     (PtyxisWindow       *self,
                                                          GPtrArray          *surfaces);
gboolean               ptyxis_window_focus_by_uuid       (PtyxisWindow       *self,
                                                          const char         *uuid);
guint                  ptyxis_window_get_n_pages         (PtyxisWindow       *self);
GVariant              *ptyxis_window_serialize           (PtyxisWindow       *self);
gboolean               ptyxis_window_deserialize         (PtyxisWindow       *self,
                                                          GVariant           *state);
void                   ptyxis_window_configure_popover   (PtyxisWindow       *self,
                                                          GtkWidget          *popover);
void                   ptyxis_window_open_new_window     (PtyxisWindow       *self,
                                                          PtyxisProfile      *profile,
                                                          const char         *container_id);
gboolean               ptyxis_window_resize_for_zoom     (PtyxisWindow       *self);
GtkWindow             *ptyxis_window_create_preferences_window (PtyxisWindow *self);

G_END_DECLS

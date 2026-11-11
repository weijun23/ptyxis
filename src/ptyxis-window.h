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
  GMenu                 * (*create_terminal_menu) (PtyxisWindow  *self);
  void                    (*open_new_window)      (PtyxisWindow  *self,
                                                   PtyxisProfile *profile,
                                                   const char    *container_id);
  gboolean                (*resize_for_zoom)      (PtyxisWindow  *self);
  void                    (*surface_needs_attention) (PtyxisWindow          *self,
                                                      PtyxisTerminalSurface *surface);
  GtkWindow             * (*create_preferences_window) (PtyxisWindow *self);
};

PtyxisWindow          *ptyxis_window_new                (void);
PtyxisWindow          *ptyxis_window_new_empty          (void);
PtyxisWindow          *ptyxis_window_new_for_command    (PtyxisProfile      *profile,
                                                         const char * const *argv,
                                                         const char         *cwd_uri);
PtyxisWindow          *ptyxis_window_new_for_profile    (PtyxisProfile      *profile);
PtyxisProfile         *ptyxis_window_get_active_profile (PtyxisWindow       *self);
PtyxisTerminalSurface *ptyxis_window_get_active_surface (PtyxisWindow       *self);
PtyxisTerminalSurface *ptyxis_window_add_terminal       (PtyxisWindow       *self,
                                                         PtyxisProfile      *profile);
void                   ptyxis_window_visual_bell        (PtyxisWindow       *self);
gboolean               ptyxis_window_focus_by_uuid      (PtyxisWindow       *self,
                                                         const char         *uuid);
guint                  ptyxis_window_get_n_pages        (PtyxisWindow       *self);
GVariant              *ptyxis_window_serialize          (PtyxisWindow       *self);
gboolean               ptyxis_window_deserialize        (PtyxisWindow       *self,
                                                         GVariant           *state);
GMenu                 *ptyxis_window_create_terminal_menu (PtyxisWindow     *self);

G_END_DECLS

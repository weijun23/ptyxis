/*
 * ptyxis-pane.h
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

#include "ptyxis-profile.h"
#include "ptyxis-terminal.h"
#include "ptyxis-terminal-surface.h"

G_BEGIN_DECLS

#define PTYXIS_TYPE_PANE (ptyxis_pane_get_type())

G_DECLARE_FINAL_TYPE (PtyxisPane, ptyxis_pane, PTYXIS, PANE, GtkWidget)

PtyxisPane        * ptyxis_pane_new                          (PtyxisProfile         *profile);
PtyxisTerminal    * ptyxis_pane_get_terminal                 (PtyxisPane            *self);
PtyxisProfile     * ptyxis_pane_get_profile                  (PtyxisPane            *self);
void                ptyxis_pane_apply_profile                (PtyxisPane            *self,
                                                              PtyxisProfile         *profile);
PtyxisZoomLevel     ptyxis_pane_get_zoom                     (PtyxisPane            *self);
void                ptyxis_pane_set_zoom                     (PtyxisPane            *self,
                                                              PtyxisZoomLevel        zoom);
void                ptyxis_pane_zoom_in                      (PtyxisPane            *self);
void                ptyxis_pane_zoom_out                     (PtyxisPane            *self);
char              * ptyxis_pane_dup_zoom_label                (PtyxisPane            *self);
char              * ptyxis_pane_dup_title                     (PtyxisPane            *self);
char              * ptyxis_pane_dup_subtitle                  (PtyxisPane            *self);
char              * ptyxis_pane_dup_current_directory_uri     (PtyxisPane            *self);
void                ptyxis_pane_set_previous_working_directory_uri (PtyxisPane       *self,
                                                              const char            *uri);
PtyxisIpcContainer * ptyxis_pane_dup_container               (PtyxisPane            *self);
void                ptyxis_pane_set_container                (PtyxisPane            *self,
                                                              PtyxisIpcContainer    *container);
void                ptyxis_pane_grab_focus                   (PtyxisPane            *self);
gboolean            ptyxis_pane_has_foreground_process       (PtyxisPane            *self,
                                                              GPid                  *pid,
                                                              char                 **cmdline);
void                ptyxis_pane_force_quit                   (PtyxisPane            *self);
PtyxisTerminalSurface * ptyxis_pane_get_surface              (PtyxisPane            *self);
const char            * ptyxis_pane_get_uuid                 (PtyxisPane            *self);
void                    ptyxis_pane_show_find_bar             (PtyxisPane            *self);

G_END_DECLS

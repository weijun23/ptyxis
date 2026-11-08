/* ptyxis-window-private.h
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

#include "ptyxis-find-bar.h"
#include "ptyxis-fullscreen-box.h"
#include "ptyxis-parking-lot.h"
#include "ptyxis-window.h"
#include "ptyxis-window-dressing.h"

G_BEGIN_DECLS

typedef struct
{
  PtyxisShortcuts       *shortcuts;
  PtyxisParkingLot      *parking_lot;

  PtyxisFindBar         *find_bar;
  GtkRevealer           *find_bar_revealer;
  AdwHeaderBar          *header_bar;
  GMenu                 *primary_menu;
  GtkMenuButton         *primary_menu_button;
  AdwToastOverlay       *toasts;
  PtyxisFullscreenBox   *fullscreen_box;

  GBindingGroup         *profile_bindings;
  GBindingGroup         *active_surface_bindings;
  GSignalGroup          *active_surface_signals;
  PtyxisWindowDressing  *dressing;
  GtkBox                *visual_bell;
  GPropertyAction       *interface_style_action;

  guint                  visual_bell_source;

  guint                  disposed : 1;
  guint                  single_terminal_mode : 1;
  guint                  is_maximized : 1;
  guint                  is_fullscreen : 1;
  guint                  in_close_request : 1;
} PtyxisWindowPrivate;

PtyxisWindowPrivate *_ptyxis_window_get_priv       (PtyxisWindow   *self);

void     _ptyxis_window_class_bind_shared_template (GtkWidgetClass *widget_class);
void     _ptyxis_window_init_after_template        (PtyxisWindow   *self);

void     ptyxis_window_save_size                   (PtyxisWindow   *self);

void     _ptyxis_window_apply_current_settings_to_surface (PtyxisWindow          *self,
                                                           PtyxisTerminalSurface *surface,
                                                           PtyxisTerminalSurface *source);

gboolean _ptyxis_window_bind_title_cb              (GBinding       *binding,
                                                    const GValue   *from_value,
                                                    GValue         *to_value,
                                                    gpointer        user_data);

G_END_DECLS

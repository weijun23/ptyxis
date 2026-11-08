/*
 * ptyxis-preferences-window-private.h
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

#pragma once

#include "ptyxis-preferences-window.h"

G_BEGIN_DECLS

typedef struct
{
  char                 *default_palette_id;
  GtkCustomFilter      *filter;
  GtkFilterListModel   *filter_palettes;
  guint                 filter_show_more : 1;

  AdwSwitchRow         *audible_bell;
  AdwComboRow          *backspace_binding;
  AdwSpinRow           *cell_height_scale;
  AdwSpinRow           *cell_width_scale;
  AdwComboRow          *cjk_ambiguous_width;
  GListModel           *cjk_ambiguous_widths;
  AdwComboRow          *cursor_blink_mode;
  GListModel           *cursor_blink_modes;
  AdwComboRow          *cursor_shape;
  GListModel           *cursor_shapes;
  AdwComboRow          *delete_binding;
  AdwSwitchRow         *disable_dynamic_title;
  AdwSwitchRow         *enable_a11y;
  GListModel           *erase_bindings;
  AdwPreferencesGroup  *app_group;
  AdwPreferencesGroup  *app_group_2;
  AdwComboRow          *exit_action;
  GListModel           *exit_actions;
  GtkLabel             *font_name;
  AdwActionRow         *font_name_row;
  AdwPreferencesGroup  *interface_group;
  AdwSwitchRow         *limit_scrollback;
  AdwSwitchRow         *login_shell;
  GtkAdjustment        *opacity_adjustment;
  AdwPreferencesGroup  *opacity_group;
  GtkLabel             *opacity_label;
  GtkFlowBox           *palette_previews;
  AdwComboRow          *preserve_directory;
  GListModel           *preserve_directories;
  GtkListBox           *profiles_list_box;
  AdwSwitchRow         *restore_session;
  GtkSwitch            *restore_window_size;
  AdwSpinRow           *default_rows;
  AdwSpinRow           *default_columns;
  AdwSpinRow           *scrollback_lines;
  AdwSwitchRow         *scroll_on_output;
  AdwSwitchRow         *scroll_on_keystroke;
  AdwComboRow          *scrollbar_policy;
  GListModel           *scrollbar_policies;
  AdwButtonContent     *show_more_palettes;
  AdwComboRow          *text_blink_mode;
  GListModel           *text_blink_modes;
  AdwSwitchRow         *use_system_font;
  AdwSwitchRow         *visual_bell;
  GtkListBox           *custom_links_list_box;
  GtkWidget            *shortcuts_page;
  AdwPreferencesGroup  *zoom_group;
} PtyxisPreferencesWindowPrivate;

PtyxisPreferencesWindowPrivate *_ptyxis_preferences_window_get_priv (PtyxisPreferencesWindow *self);

void     ptyxis_preferences_window_bind_shortcut_rows  (PtyxisPreferencesWindow *self,
                                                        GtkWidget               *page);
gboolean ptyxis_preferences_window_string_to_index     (GValue                  *value,
                                                        GVariant                *variant,
                                                        gpointer                 user_data);
GVariant *ptyxis_preferences_window_index_to_string    (const GValue            *value,
                                                        const GVariantType      *type,
                                                        gpointer                 user_data);

G_END_DECLS

/*
 * ptyxis-terminal-surface.h
 *
 * Copyright 2023-2024 Christian Hergert <christian@sourceandstack.com>
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

#include "ptyxis-agent-ipc.h"
#include "ptyxis-profile.h"
#include "ptyxis-terminal.h"

G_BEGIN_DECLS

typedef enum _PtyxisProcessLeader
{
  PTYXIS_PROCESS_LEADER_KIND_UNKNOWN,
  PTYXIS_PROCESS_LEADER_KIND_SUPERUSER,
  PTYXIS_PROCESS_LEADER_KIND_REMOTE,
  PTYXIS_PROCESS_LEADER_KIND_CONTAINER,
} PtyxisProcessLeaderKind;

typedef enum _PtyxisZoomLevel
{
  PTYXIS_ZOOM_LEVEL_MINUS_14 = 1,
  PTYXIS_ZOOM_LEVEL_MINUS_13,
  PTYXIS_ZOOM_LEVEL_MINUS_12,
  PTYXIS_ZOOM_LEVEL_MINUS_11,
  PTYXIS_ZOOM_LEVEL_MINUS_10,
  PTYXIS_ZOOM_LEVEL_MINUS_9,
  PTYXIS_ZOOM_LEVEL_MINUS_8,
  PTYXIS_ZOOM_LEVEL_MINUS_7,
  PTYXIS_ZOOM_LEVEL_MINUS_6,
  PTYXIS_ZOOM_LEVEL_MINUS_5,
  PTYXIS_ZOOM_LEVEL_MINUS_4,
  PTYXIS_ZOOM_LEVEL_MINUS_3,
  PTYXIS_ZOOM_LEVEL_MINUS_2,
  PTYXIS_ZOOM_LEVEL_MINUS_1,
  PTYXIS_ZOOM_LEVEL_DEFAULT,
  PTYXIS_ZOOM_LEVEL_PLUS_1,
  PTYXIS_ZOOM_LEVEL_PLUS_2,
  PTYXIS_ZOOM_LEVEL_PLUS_3,
  PTYXIS_ZOOM_LEVEL_PLUS_4,
  PTYXIS_ZOOM_LEVEL_PLUS_5,
  PTYXIS_ZOOM_LEVEL_PLUS_6,
  PTYXIS_ZOOM_LEVEL_PLUS_7,
  PTYXIS_ZOOM_LEVEL_PLUS_8,
  PTYXIS_ZOOM_LEVEL_PLUS_9,
  PTYXIS_ZOOM_LEVEL_PLUS_10,
  PTYXIS_ZOOM_LEVEL_PLUS_11,
  PTYXIS_ZOOM_LEVEL_PLUS_12,
  PTYXIS_ZOOM_LEVEL_PLUS_13,
  PTYXIS_ZOOM_LEVEL_PLUS_14,
} PtyxisZoomLevel;

#define PTYXIS_ZOOM_LEVEL_LAST (PTYXIS_ZOOM_LEVEL_PLUS_14+1)

typedef enum _PtyxisTabProgress
{
  PTYXIS_TAB_PROGRESS_INDETERMINATE,
  PTYXIS_TAB_PROGRESS_ACTIVE,
  PTYXIS_TAB_PROGRESS_ERROR,
} PtyxisTabProgress;

typedef enum _PtyxisTerminalSurfaceState
{
  PTYXIS_TERMINAL_SURFACE_STATE_INITIAL,
  PTYXIS_TERMINAL_SURFACE_STATE_SPAWNING,
  PTYXIS_TERMINAL_SURFACE_STATE_RUNNING,
  PTYXIS_TERMINAL_SURFACE_STATE_EXITED,
  PTYXIS_TERMINAL_SURFACE_STATE_FAILED,
} PtyxisTerminalSurfaceState;

#define PTYXIS_TYPE_TERMINAL_SURFACE (ptyxis_terminal_surface_get_type())

G_DECLARE_FINAL_TYPE (PtyxisTerminalSurface,
                      ptyxis_terminal_surface,
                      PTYXIS, TERMINAL_SURFACE,
                      GObject)

PtyxisTerminalSurface * ptyxis_terminal_surface_new                       (PtyxisProfile            *profile);

/* Terminal & widgets */
PtyxisTerminal     * ptyxis_terminal_surface_get_terminal                  (PtyxisTerminalSurface    *self);
GtkScrolledWindow  * ptyxis_terminal_surface_get_scrolled_window           (PtyxisTerminalSurface    *self);

/* Profile */
PtyxisProfile      * ptyxis_terminal_surface_get_profile                   (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_apply_profile                  (PtyxisTerminalSurface    *self,
                                                                            PtyxisProfile            *new_profile);

/* Zoom */
PtyxisZoomLevel      ptyxis_terminal_surface_get_zoom                      (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_set_zoom                      (PtyxisTerminalSurface    *self,
                                                                            PtyxisZoomLevel           zoom);
void                 ptyxis_terminal_surface_zoom_in                       (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_zoom_out                      (PtyxisTerminalSurface    *self);
char               * ptyxis_terminal_surface_dup_zoom_label                (PtyxisTerminalSurface    *self);

/* Process */
PtyxisTerminalSurfaceState ptyxis_terminal_surface_get_state               (PtyxisTerminalSurface    *self);
gboolean             ptyxis_terminal_surface_is_running                    (PtyxisTerminalSurface    *self,
                                                                            char                    **cmdline);
gboolean             ptyxis_terminal_surface_has_foreground_process        (PtyxisTerminalSurface    *self,
                                                                            GPid                     *pid,
                                                                            char                    **cmdline);
void                 ptyxis_terminal_surface_force_quit                    (PtyxisTerminalSurface    *self);
PtyxisIpcProcess   * ptyxis_terminal_surface_get_process                   (PtyxisTerminalSurface    *self);
const char         * ptyxis_terminal_surface_get_command_line              (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_respawn                       (PtyxisTerminalSurface    *self);

/* Container / CWD */
PtyxisIpcContainer * ptyxis_terminal_surface_dup_container                 (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_set_container                 (PtyxisTerminalSurface    *self,
                                                                            PtyxisIpcContainer       *container);
char               * ptyxis_terminal_surface_dup_current_directory_uri     (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_set_initial_working_directory_uri (PtyxisTerminalSurface *self,
                                                                            const char               *uri);
char               * ptyxis_terminal_surface_dup_previous_working_directory_uri (PtyxisTerminalSurface *self);
void                 ptyxis_terminal_surface_set_previous_working_directory_uri (PtyxisTerminalSurface *self,
                                                                            const char               *uri);

/* Spawn configuration */
void                 ptyxis_terminal_surface_set_command                   (PtyxisTerminalSurface    *self,
                                                                            const char * const       *command);
void                 ptyxis_terminal_surface_set_environment               (PtyxisTerminalSurface    *self,
                                                                            const char * const       *env);

/* Title / icon */
char               * ptyxis_terminal_surface_dup_title                     (PtyxisTerminalSurface    *self);
char               * ptyxis_terminal_surface_dup_subtitle                  (PtyxisTerminalSurface    *self);
GIcon              * ptyxis_terminal_surface_dup_icon                      (PtyxisTerminalSurface    *self);
PtyxisProcessLeaderKind ptyxis_terminal_surface_get_process_leader_kind    (PtyxisTerminalSurface    *self);
const char         * ptyxis_terminal_surface_get_initial_title             (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_set_initial_title             (PtyxisTerminalSurface    *self,
                                                                            const char               *initial_title);
const char         * ptyxis_terminal_surface_get_title_prefix              (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_set_title_prefix              (PtyxisTerminalSurface    *self,
                                                                            const char               *title_prefix);
gboolean             ptyxis_terminal_surface_get_ignore_osc_title          (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_set_ignore_osc_title          (PtyxisTerminalSurface    *self,
                                                                            gboolean                  ignore_osc_title);

/* Progress */
PtyxisTabProgress    ptyxis_terminal_surface_get_progress                  (PtyxisTerminalSurface    *self);
double               ptyxis_terminal_surface_get_progress_fraction         (PtyxisTerminalSurface    *self);

/* Agent polling */
void                 ptyxis_terminal_surface_poll_agent_async              (PtyxisTerminalSurface    *self,
                                                                            GCancellable             *cancellable,
                                                                            GAsyncReadyCallback       callback,
                                                                            gpointer                  user_data);
gboolean             ptyxis_terminal_surface_poll_agent_finish             (PtyxisTerminalSurface    *self,
                                                                            GAsyncResult             *result,
                                                                            GError                  **error);

/* Other */
void                 ptyxis_terminal_surface_grab_focus                    (PtyxisTerminalSurface    *self);
void                 ptyxis_terminal_surface_open_uri                      (PtyxisTerminalSurface    *self,
                                                                            const char               *uri);
char               * ptyxis_terminal_surface_query_working_directory_from_agent (PtyxisTerminalSurface *self);
void                 ptyxis_terminal_surface_set_keep_terminal_open        (PtyxisTerminalSurface    *self,
                                                                            gboolean                  keep_terminal_open);
gboolean             ptyxis_terminal_surface_get_has_pressed_key           (PtyxisTerminalSurface    *self);

/* Session serialization */
void                 ptyxis_terminal_surface_serialize                     (PtyxisTerminalSurface    *self,
                                                                            GVariantBuilder          *builder);
void                 ptyxis_terminal_surface_deserialize                   (PtyxisTerminalSurface    *self,
                                                                            GVariant                 *variant);

G_END_DECLS

/* ptyxis-docker-container.h
 *
 * Copyright 2026 Eduard Tolosa <tolosaeduard@gmail.com>
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

#include <json-glib/json-glib.h>

#include "ptyxis-agent-ipc.h"

G_BEGIN_DECLS

#define PTYXIS_TYPE_DOCKER_CONTAINER (ptyxis_docker_container_get_type())

G_DECLARE_FINAL_TYPE (PtyxisDockerContainer, ptyxis_docker_container, PTYXIS, DOCKER_CONTAINER, PtyxisIpcContainerSkeleton)

gboolean    ptyxis_docker_container_deserialize  (PtyxisDockerContainer  *self,
                                                  JsonObject             *object,
                                                  GError                **error);
const char *ptyxis_docker_container_lookup_label (PtyxisDockerContainer  *self,
                                                  const char             *key);

G_END_DECLS

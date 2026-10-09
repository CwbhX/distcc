/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * Copyright 2026 Clement Hathaway
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

/* Mirrored-tree mode, daemon side (mirror_serve.c). */

#ifndef DISTCC_MIRROR_SERVE_H
#define DISTCC_MIRROR_SERVE_H

int dcc_mirror_serve(int in_fd, int out_fd,
                     const char *err_fname, const char *out_fname,
                     char ***argv_ret, char **input_ret, int *status_ret,
                     int *job_result);

#endif /* DISTCC_MIRROR_SERVE_H */

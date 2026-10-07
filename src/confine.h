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

/* Write confinement for mirror-mode compiles (confine.c). */

#ifndef DISTCC_CONFINE_H
#define DISTCC_CONFINE_H

#include <sys/types.h>

int dcc_confine_available(void);
int dcc_confine_probe(void);
int dcc_spawn_confined(char **argv, pid_t *pidptr,
                       const char *stdin_file, const char *stdout_file,
                       const char *stderr_file, const char *job_dir,
                       int *confine_failed);

#endif /* DISTCC_CONFINE_H */

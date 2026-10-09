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

/* Mirror mode: include search path and shadow candidates (mirror_search.c). */

#ifndef DISTCC_MIRROR_SEARCH_H
#define DISTCC_MIRROR_SEARCH_H

#include "mirror.h"

struct dcc_search_dir {
    char *spelled;      /* as given (argv, environment, compiler) */
    char *norm;         /* normalized, logical */
    int framework;
};

struct dcc_search_list {
    struct dcc_search_dir *d;
    int n;
};

/* Maps a path to the one to open on this side; returns malloc'd. */
typedef char *(*dcc_mirror_resolve_fn)(const void *ctx, const char *path);

/* Runs a compiler probe and returns its stderr (malloc'd). */
typedef int (*dcc_mirror_probe_fn)(void *ctx, char **argv, char **err_ret);

int dcc_mirror_check_search_options(char **argv);
int dcc_mirror_probe_local(void *ctx, char **argv, char **err_ret);
int dcc_mirror_search_list(char **argv, const char *input,
                           const struct dcc_mirror_rules *r,
                           dcc_mirror_probe_fn run, void *run_ctx,
                           dcc_mirror_resolve_fn fn, const void *fn_ctx,
                           struct dcc_search_list *sl);
void dcc_search_list_free(struct dcc_search_list *sl);
int dcc_mirror_shadow_candidates(const struct dcc_search_list *sl,
                                 char **files, int n_files,
                                 const char *input,
                                 const struct dcc_mirror_rules *r,
                                 dcc_mirror_resolve_fn fn, const void *ctx,
                                 struct dcc_strset *out);

#endif /* DISTCC_MIRROR_SEARCH_H */

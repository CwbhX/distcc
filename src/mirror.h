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

/* Mirrored-tree mode (protocol version 4).  See doc/mirrored-tree-design.md
 * and doc/protocol-4.txt. */

#ifndef DISTCC_MIRROR_H
#define DISTCC_MIRROR_H

#include <sys/types.h>
#include <sys/stat.h>

#include "sha256.h"

/* MIRR codes sent by the daemon before any compile result. */
enum dcc_mirror_code {
    DCC_MIRR_OK = 0,
    DCC_MIRR_DISABLED = 1,      /* no --mirror-root, or cwd/input not under one */
    DCC_MIRR_STALE = 2,         /* pre-check mismatch */
    DCC_MIRR_ARG_POLICY = 3,    /* an argument was refused */
    DCC_MIRR_MISSING = 4,       /* cwd or input missing */
    DCC_MIRR_NO_CONFINE = 5     /* write confinement could not be set up */
};

enum dcc_mirror_kind {
    DCC_MIRROR_ABSENT = 0,
    DCC_MIRROR_FILE = 1,        /* regular file */
    DCC_MIRROR_OTHER = 2        /* exists, not a regular file */
};

/* Identity of one file, as both sides describe it. */
struct dcc_mirror_ident {
    int present;                /* enum dcc_mirror_kind */
    off_t size;
    long long mtime;            /* seconds */
    char digest[DCC_SHA256_HEX_LEN + 1];  /* "" when not computed */
};

/* One entry of the check list the client sends (NCHK). */
struct dcc_mirror_check {
    char *path;
    struct dcc_mirror_ident ident;
};

struct dcc_mirror_checklist {
    struct dcc_mirror_check *items;
    int n, cap;
};

/* mirror.c: shared between client and daemon. */
int dcc_mirror_checklist_add(struct dcc_mirror_checklist *cl,
                             const char *path);
void dcc_mirror_checklist_free(struct dcc_mirror_checklist *cl);

int dcc_mirror_is_installed_path(const char *path);
int dcc_mirror_set_installed_prefixes(const char *colon_list);
int dcc_mirror_ident_of(const char *path, int want_digest,
                        struct dcc_mirror_ident *ident);
int dcc_mirror_ident_equal(const struct dcc_mirror_ident *a,
                           const struct dcc_mirror_ident *b,
                           int compare_digest);

int dcc_mirror_parse_dotd(const char *text, size_t len,
                          char ***paths_ret, int *n_ret);
void dcc_mirror_free_paths(char **paths, int n);

int dcc_mirror_format_dsta_line(const char *path,
                                const struct dcc_mirror_ident *ident,
                                char **line_ret);
int dcc_mirror_parse_dsta_line(char *line, char **path_ret,
                               struct dcc_mirror_ident *ident);

/* mirror_digest.c: per-user persistent digest cache. */
int dcc_mirror_digest(const char *path, const struct stat *st,
                      char hex[DCC_SHA256_HEX_LEN + 1]);
void dcc_mirror_digest_flush(void);

#endif /* DISTCC_MIRROR_H */

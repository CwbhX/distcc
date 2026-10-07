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
    DCC_MIRR_NO_CONFINE = 5,    /* write confinement could not be set up */
    DCC_MIRR_COMPILER = 6       /* the helper's compiler is not the client's */
};

enum dcc_mirror_kind {
    DCC_MIRROR_ABSENT = 0,
    DCC_MIRROR_FILE = 1,        /* regular file */
    DCC_MIRROR_OTHER = 2,       /* exists, not a regular file */
    DCC_MIRROR_DIR = 3          /* directory; digest is its listing hash */
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
int dcc_mirror_installed_prefixes(char ***prefixes, int *n);
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
int dcc_mirror_cache_get(const char *key, const struct stat *st,
                         char hex[DCC_SHA256_HEX_LEN + 1]);
void dcc_mirror_cache_put(const char *key, const struct stat *st,
                          const char hex[DCC_SHA256_HEX_LEN + 1]);
void dcc_mirror_digest_flush(void);

/* mirror_ident.c: string sets, directory identity, required sets,
 * compiler identity. */
struct dcc_strset {
    char **items;
    size_t cap, used;
};
int dcc_strset_add(struct dcc_strset *s, const char *str);
int dcc_strset_has(const struct dcc_strset *s, const char *str);
void dcc_strset_free(struct dcc_strset *s);

/* What the sync leaves out, which directory identity must ignore too. */
struct dcc_mirror_rules {
    char **roots;           /* synced source roots */
    int n_roots;
    char **excludes;        /* globs left out at the top of each root */
    int n_excludes;
    char **build_roots;     /* build trees: only build files are synced */
    int n_build_roots;
    char **installed;       /* installed-tree prefixes the client uses */
    int n_installed;
    char **map_phys;        /* DISTCC_MIRROR_PATHMAP: physical prefixes */
    char **map_logical;     /* ... and the logical names they map to */
    int n_maps;
    char *cwd;              /* logical cwd, for relative paths */
};
int dcc_mirror_rules_from_env(struct dcc_mirror_rules *r, const char *cwd);
int dcc_mirror_rules_add(struct dcc_mirror_rules *r, const char *rule);
int dcc_mirror_rules_list(const struct dcc_mirror_rules *r, char ***out,
                          int *n);
void dcc_mirror_rules_free(struct dcc_mirror_rules *r);

int dcc_mirror_is_build_file(const char *name);
int dcc_mirror_dir_ident(const char *path, const struct dcc_mirror_rules *r,
                         struct dcc_mirror_ident *ident,
                         char ***names_ret, int *n_names);
void dcc_mirror_free_names(char **names, int n);
int dcc_mirror_search_dirs(char **argv, char ***dirs_ret, int *n_ret);
int dcc_mirror_required(char **argv, char **files, int n_files,
                        struct dcc_strset *file_set,
                        struct dcc_strset *dir_set,
                        struct dcc_strset *comps);
struct dcc_strint {
    char **keys;
    int *vals;
    size_t cap, used;
};
void dcc_strint_free(struct dcc_strint *m);
int dcc_strint_get(const struct dcc_strint *m, const char *key);
char *dcc_mirror_normalize(const struct dcc_mirror_rules *r, const char *path);
int dcc_mirror_search_order(char **argv, const struct dcc_mirror_rules *r,
                            char ***dirs_ret, int *n_ret);
int dcc_mirror_component_positions(char **files, int n_files,
                                   char **dirs, int n_dirs,
                                   const struct dcc_mirror_rules *r,
                                   struct dcc_strint *m);
char *dcc_mirror_daemon_path(const struct dcc_mirror_rules *r,
                              char **roots, int n_roots, const char *path);
int dcc_mirror_write_overlay(const struct dcc_mirror_rules *r,
                             char **roots, int n_roots, const char *fname);
int dcc_mirror_compiler_ident(const char *argv0,
                              char hex[DCC_SHA256_HEX_LEN + 1]);
extern const char *const dcc_mirror_env_names[];

#endif /* DISTCC_MIRROR_H */

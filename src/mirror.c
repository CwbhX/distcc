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

/* Mirrored-tree mode: pieces shared by client and daemon.
 *
 * A file's identity is what both machines compare before an object built on
 * the helper is accepted.  Files in trees the sync copies with "rsync -a"
 * (sources, build outputs, PCHs) are compared by size and whole-second
 * mtime.  Files in separately installed trees (Command Line Tools,
 * Homebrew) are compared by size and SHA-256, because their mtimes need not
 * agree across machines even when their contents do.  Paths are always
 * stat()ed with symlinks followed: the compiler read the target. */

#include <config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "mirror.h"

static const char *const default_installed_prefixes =
    "/Library/Developer/CommandLineTools:/Applications/Xcode.app:"
    "/opt/homebrew";

static char **installed_prefixes = NULL;
static int n_installed_prefixes = -1;


int dcc_mirror_checklist_add(struct dcc_mirror_checklist *cl,
                             const char *path)
{
    int i;

    for (i = 0; i < cl->n; i++)
        if (strcmp(cl->items[i].path, path) == 0)
            return 0;
    if (cl->n == cl->cap) {
        int cap = cl->cap ? cl->cap * 2 : 8;
        struct dcc_mirror_check *n = realloc(cl->items, cap * sizeof *n);
        if (!n)
            return EXIT_OUT_OF_MEMORY;
        cl->items = n;
        cl->cap = cap;
    }
    memset(&cl->items[cl->n], 0, sizeof cl->items[cl->n]);
    if (!(cl->items[cl->n].path = strdup(path)))
        return EXIT_OUT_OF_MEMORY;
    cl->n++;
    return 0;
}


void dcc_mirror_checklist_free(struct dcc_mirror_checklist *cl)
{
    int i;

    for (i = 0; i < cl->n; i++)
        free(cl->items[i].path);
    free(cl->items);
    cl->items = NULL;
    cl->n = cl->cap = 0;
}


/**
 * Set the colon-separated list of installed-tree prefixes.  NULL restores
 * the default.
 **/
int dcc_mirror_set_installed_prefixes(const char *colon_list)
{
    char *copy, *p, *tok;
    int i;

    for (i = 0; i < n_installed_prefixes; i++)
        free(installed_prefixes[i]);
    free(installed_prefixes);
    installed_prefixes = NULL;
    n_installed_prefixes = 0;

    if (!colon_list)
        colon_list = default_installed_prefixes;
    if (!(copy = strdup(colon_list)))
        return EXIT_OUT_OF_MEMORY;
    for (p = copy; (tok = strsep(&p, ":")) != NULL; ) {
        size_t len = strlen(tok);
        char **n;
        while (len > 1 && tok[len - 1] == '/')
            tok[--len] = '\0';
        if (len == 0 || tok[0] != '/')
            continue;
        n = realloc(installed_prefixes,
                    (n_installed_prefixes + 1) * sizeof *n);
        if (!n || !(n[n_installed_prefixes] = strdup(tok))) {
            free(copy);
            return EXIT_OUT_OF_MEMORY;
        }
        installed_prefixes = n;
        n_installed_prefixes++;
    }
    free(copy);
    return 0;
}


/**
 * Is @p path under an installed tree?  Relative paths never are.
 **/
int dcc_mirror_is_installed_path(const char *path)
{
    int i;

    if (n_installed_prefixes < 0)
        dcc_mirror_set_installed_prefixes(getenv("DISTCC_MIRROR_INSTALLED"));
    if (path[0] != '/')
        return 0;
    for (i = 0; i < n_installed_prefixes; i++) {
        size_t len = strlen(installed_prefixes[i]);
        if (strncmp(path, installed_prefixes[i], len) == 0
            && (path[len] == '/' || path[len] == '\0'))
            return 1;
    }
    return 0;
}


/**
 * Describe @p path.  A missing path is reported as absent, not as an error.
 * The digest is computed (through the cache) only when @p want_digest is
 * set and the path is a regular file.
 **/
int dcc_mirror_ident_of(const char *path, int want_digest,
                        struct dcc_mirror_ident *ident)
{
    struct stat st;
    int ret;

    memset(ident, 0, sizeof *ident);
    if (stat(path, &st) == -1) {
        if (errno == ENOENT || errno == ENOTDIR) {
            ident->present = DCC_MIRROR_ABSENT;
            return 0;
        }
        rs_log_warning("stat %s failed: %s", path, strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (!S_ISREG(st.st_mode)) {
        ident->present = DCC_MIRROR_OTHER;
        return 0;
    }
    ident->present = DCC_MIRROR_FILE;
    ident->size = st.st_size;
    ident->mtime = (long long) st.st_mtime;
    if (want_digest) {
        if ((ret = dcc_mirror_digest(path, &st, ident->digest)))
            return ret;
    }
    return 0;
}


/**
 * Compare two identities.  With @p compare_digest, size and digest must
 * match and the mtime is ignored; otherwise size and mtime must match.
 **/
int dcc_mirror_ident_equal(const struct dcc_mirror_ident *a,
                           const struct dcc_mirror_ident *b,
                           int compare_digest)
{
    if (a->present != b->present)
        return 0;
    if (a->present != DCC_MIRROR_FILE)
        return 1;
    if (a->size != b->size)
        return 0;
    if (compare_digest)
        return a->digest[0] != '\0'
            && strcmp(a->digest, b->digest) == 0;
    return a->mtime == b->mtime;
}


void dcc_mirror_free_paths(char **paths, int n)
{
    int i;

    for (i = 0; i < n; i++)
        free(paths[i]);
    free(paths);
}


/**
 * Parse the first rule of a Make-style dependency file, as clang writes it,
 * into the list of prerequisite paths.
 *
 * Clang escapes a space as "\ ", '#' as "\#" and '$' as "$$", and continues
 * lines with a backslash-newline.  The target ends at the first ':' that is
 * followed by whitespace or the end of the text.  Prerequisites may contain
 * ':' (it is not escaped).  Only the first rule is read; later rules (from
 * -MP) only repeat prerequisites as phony targets.
 **/
int dcc_mirror_parse_dotd(const char *text, size_t len,
                          char ***paths_ret, int *n_ret)
{
    size_t i = 0;
    char **paths = NULL;
    int n = 0, cap = 0;
    char *tok = NULL;
    size_t tok_len = 0, tok_cap = 0;

    *paths_ret = NULL;
    *n_ret = 0;

    /* Skip the target. */
    for (; i < len; i++) {
        if (text[i] == '\\' && i + 1 < len) {
            i++;
            continue;
        }
        if (text[i] == ':' && (i + 1 == len || text[i+1] == ' '
                               || text[i+1] == '\t' || text[i+1] == '\n'
                               || text[i+1] == '\r' || text[i+1] == '\\')) {
            i++;
            break;
        }
        if (text[i] == '\n') {
            rs_log_error("dependency file has no target");
            return EXIT_PROTOCOL_ERROR;
        }
    }

    for (;;) {
        int end_token = 0, end_rule = 0;
        char c = 0;

        if (i >= len) {
            end_token = end_rule = 1;
        } else if (text[i] == '\\' && i + 1 < len
                   && (text[i+1] == '\n' || text[i+1] == '\r')) {
            /* Line continuation. */
            i += 2;
            if (text[i-1] == '\r' && i < len && text[i] == '\n')
                i++;
            end_token = 1;
        } else if (text[i] == '\\' && i + 1 < len
                   && (text[i+1] == ' ' || text[i+1] == '#'
                       || text[i+1] == '\\')) {
            c = text[i+1];
            i += 2;
        } else if (text[i] == '$' && i + 1 < len && text[i+1] == '$') {
            c = '$';
            i += 2;
        } else if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r') {
            i++;
            end_token = 1;
        } else if (text[i] == '\n') {
            i++;
            end_token = end_rule = 1;
        } else {
            c = text[i++];
        }

        if (c) {
            if (tok_len + 2 > tok_cap) {
                size_t ncap = tok_cap ? tok_cap * 2 : 256;
                char *nt = realloc(tok, ncap);
                if (!nt)
                    goto oom;
                tok = nt;
                tok_cap = ncap;
            }
            tok[tok_len++] = c;
        }
        if (end_token && tok_len) {
            tok[tok_len] = '\0';
            if (n == cap) {
                int ncap = cap ? cap * 2 : 256;
                char **np = realloc(paths, ncap * sizeof *np);
                if (!np)
                    goto oom;
                paths = np;
                cap = ncap;
            }
            if (!(paths[n] = strdup(tok)))
                goto oom;
            n++;
            tok_len = 0;
        }
        if (end_rule)
            break;
    }
    free(tok);
    *paths_ret = paths;
    *n_ret = n;
    return 0;

  oom:
    free(tok);
    dcc_mirror_free_paths(paths, n);
    return EXIT_OUT_OF_MEMORY;
}


/**
 * One DSTA line: "F <size> <mtime> <digest|-> <path>", "A <path>" (absent)
 * or "O <path>" (exists, not a regular file).  The path is the rest of the
 * line, so it may contain spaces; a path containing a newline cannot be
 * represented and is refused.
 **/
int dcc_mirror_format_dsta_line(const char *path,
                                const struct dcc_mirror_ident *ident,
                                char **line_ret)
{
    int r;

    *line_ret = NULL;
    if (strchr(path, '\n'))
        return EXIT_PROTOCOL_ERROR;
    if (ident->present == DCC_MIRROR_FILE)
        r = asprintf(line_ret, "F %lld %lld %s %s\n",
                     (long long) ident->size, ident->mtime,
                     ident->digest[0] ? ident->digest : "-", path);
    else
        r = asprintf(line_ret, "%c %s\n",
                     ident->present == DCC_MIRROR_ABSENT ? 'A' : 'O', path);
    return r < 0 ? EXIT_OUT_OF_MEMORY : 0;
}


/**
 * Parse one DSTA line (without its newline) in place.  @p path_ret points
 * into @p line.
 **/
int dcc_mirror_parse_dsta_line(char *line, char **path_ret,
                               struct dcc_mirror_ident *ident)
{
    char *p = line, *end;
    long long v;

    memset(ident, 0, sizeof *ident);
    *path_ret = NULL;
    if ((line[0] == 'A' || line[0] == 'O') && line[1] == ' ' && line[2]) {
        ident->present = line[0] == 'A' ? DCC_MIRROR_ABSENT : DCC_MIRROR_OTHER;
        *path_ret = line + 2;
        return 0;
    }
    if (line[0] != 'F' || line[1] != ' ')
        return EXIT_PROTOCOL_ERROR;
    p = line + 2;
    errno = 0;
    v = strtoll(p, &end, 10);
    if (errno || end == p || *end != ' ' || v < 0)
        return EXIT_PROTOCOL_ERROR;
    ident->size = (off_t) v;
    p = end + 1;
    v = strtoll(p, &end, 10);
    if (errno || end == p || *end != ' ')
        return EXIT_PROTOCOL_ERROR;
    ident->mtime = v;
    p = end + 1;
    if (p[0] == '-' && p[1] == ' ') {
        p += 2;
    } else {
        int i;
        for (i = 0; i < DCC_SHA256_HEX_LEN; i++)
            if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
                return EXIT_PROTOCOL_ERROR;
        if (p[DCC_SHA256_HEX_LEN] != ' ')
            return EXIT_PROTOCOL_ERROR;
        memcpy(ident->digest, p, DCC_SHA256_HEX_LEN);
        ident->digest[DCC_SHA256_HEX_LEN] = '\0';
        p += DCC_SHA256_HEX_LEN + 1;
    }
    if (!*p)
        return EXIT_PROTOCOL_ERROR;
    ident->present = DCC_MIRROR_FILE;
    *path_ret = p;
    return 0;
}

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

/* Mirrored-tree mode, client side.
 *
 * A mirrored job sends no source.  The helper has the client's tree at the
 * same paths and compiles in it; the client then checks, file by file, that
 * everything the compile read is identical to what the client has, and only
 * then writes the object and the .d into place.  See
 * doc/mirrored-tree-design.md. */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "rpc.h"
#include "bulk.h"
#include "util.h"
#include "hosts.h"
#include "mirror.h"
#include "mirror_client.h"

/* PCH-related options that do not load anything and are safe to pass. */
static const char *const harmless_pch_options[] = {
    "-Winvalid-pch",
    "-Wno-invalid-pch",
    "-fpch-instantiate-templates",
    "-fno-pch-instantiate-templates",
    "-fno-pch-timestamp",
    "-fpch-validate-input-files-content",
    "-fno-pch-validate-input-files-content",
    "-fpch-preprocess",         /* added by ccache; only affects -E */
    NULL
};


void dcc_mirror_job_init(struct dcc_mirror_job *job)
{
    memset(job, 0, sizeof *job);
}

void dcc_mirror_job_free(struct dcc_mirror_job *job)
{
    int i;

    free(job->cwd);
    dcc_mirror_checklist_free(&job->checks);
    for (i = 0; i < job->n_env; i++)
        free(job->env[i]);
    free(job->env);
    dcc_mirror_rules_free(&job->rules);
    memset(job, 0, sizeof *job);
}


/**
 * The cwd to send: the physical cwd with its prefix rewritten through
 * DISTCC_MIRROR_PATHMAP ("physical=logical" pairs separated by ':').  The
 * mapped name must be the same directory as the physical one.
 **/
static int mirror_cwd(char **cwd_ret)
{
    char phys[MAXPATHLEN + 1];
    const char *map = getenv("DISTCC_MIRROR_PATHMAP");
    char *copy, *p, *pair;
    int ret = 0;

    *cwd_ret = NULL;
    if (!getcwd(phys, sizeof phys)) {
        rs_log_warning("getcwd failed: %s", strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (map && *map) {
        if (!(copy = strdup(map)))
            return EXIT_OUT_OF_MEMORY;
        for (p = copy; (pair = strsep(&p, ":")) != NULL; ) {
            char *eq = strchr(pair, '=');
            size_t plen;
            if (!eq)
                continue;
            *eq = '\0';
            plen = strlen(pair);
            while (plen > 1 && pair[plen - 1] == '/')
                pair[--plen] = '\0';
            if (plen && strncmp(phys, pair, plen) == 0
                && (phys[plen] == '/' || phys[plen] == '\0')) {
                struct stat a, b;
                if (asprintf(cwd_ret, "%s%s", eq + 1, phys + plen) < 0) {
                    *cwd_ret = NULL;
                    ret = EXIT_OUT_OF_MEMORY;
                } else if (stat(*cwd_ret, &a) == -1 || stat(".", &b) == -1
                           || a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
                    rs_log_warning("DISTCC_MIRROR_PATHMAP maps %s to %s, which "
                                   "is not the same directory", phys, *cwd_ret);
                    free(*cwd_ret);
                    *cwd_ret = NULL;
                    ret = EXIT_DISTCC_FAILED;
                }
                break;
            }
        }
        free(copy);
        if (ret || *cwd_ret)
            return ret;
    }
    if (!(*cwd_ret = strdup(phys)))
        return EXIT_OUT_OF_MEMORY;
    return 0;
}


/**
 * Is @p cwd under one of DISTCC_MIRROR_ROOTS?  True when the variable is
 * unset.
 **/
static int under_mirror_roots(const char *cwd)
{
    const char *roots = getenv("DISTCC_MIRROR_ROOTS");
    char *copy, *p, *root;
    int found = 0;

    if (!roots || !*roots)
        return 1;
    if (!(copy = strdup(roots)))
        return 0;
    for (p = copy; !found && (root = strsep(&p, ":")) != NULL; ) {
        size_t len = strlen(root);
        while (len > 1 && root[len - 1] == '/')
            root[--len] = '\0';
        if (len && strncmp(cwd, root, len) == 0
            && (cwd[len] == '/' || cwd[len] == '\0'))
            found = 1;
    }
    free(copy);
    return found;
}


static int add_include(struct dcc_mirror_checklist *cl, const char *h)
{
    char *cand;
    int ret;

    if ((ret = dcc_mirror_checklist_add(cl, h)))
        return ret;
    /* The driver loads <h>.pch or <h>.gch in place of an -include'd header
     * when one exists.  Whether it exists must match on both sides. */
    if (asprintf(&cand, "%s.pch", h) < 0)
        return EXIT_OUT_OF_MEMORY;
    ret = dcc_mirror_checklist_add(cl, cand);
    free(cand);
    if (ret)
        return ret;
    if (asprintf(&cand, "%s.gch", h) < 0)
        return EXIT_OUT_OF_MEMORY;
    ret = dcc_mirror_checklist_add(cl, cand);
    free(cand);
    return ret;
}


static int is_harmless_pch_option(const char *a)
{
    int i;

    for (i = 0; harmless_pch_options[i]; i++)
        if (strcmp(a, harmless_pch_options[i]) == 0)
            return 1;
    return 0;
}

/* Does this option load a PCH or module in a way we do not track? */
static int is_untracked_pch_option(const char *a)
{
    if (is_harmless_pch_option(a))
        return 0;
    return strstr(a, "pch") != NULL || strstr(a, "-fmodule") != NULL
        || strstr(a, "-fprebuilt-module") != NULL
        || strstr(a, "-fimplicit-module") != NULL
        || strcmp(a, "-emit-pch") == 0 || strcmp(a, "-fmodules") == 0;
}


/**
 * Build the check list from argv: the input, every PCH operand, every
 * -include / -imacros operand and the implicit PCH candidates of each
 * -include.  Returns EXIT_DISTCC_FAILED if argv loads something we cannot
 * account for, in which case the job must not be mirrored.
 **/
int dcc_mirror_build_checklist(char **argv, const char *input_fname,
                               struct dcc_mirror_checklist *cl)
{
    int i, ret;

    if ((ret = dcc_mirror_checklist_add(cl, input_fname)))
        return ret;

    for (i = 0; argv[i]; i++) {
        const char *a = argv[i];
        const char *op = NULL;      /* option name after unwrapping */
        const char *val = NULL;     /* its operand */

        if (strcmp(a, "-Xclang") == 0 || strcmp(a, "-Xpreprocessor") == 0) {
            const char *inner = argv[i + 1];
            if (!inner)
                break;
            if (strcmp(inner, "-include-pch") == 0
                || strcmp(inner, "-include") == 0
                || strcmp(inner, "-imacros") == 0) {
                if (!argv[i + 2] || strcmp(argv[i + 2], a) != 0
                    || !argv[i + 3]) {
                    rs_log_info("mirror: unpaired %s %s; not mirroring",
                                a, inner);
                    return EXIT_DISTCC_FAILED;
                }
                op = inner;
                val = argv[i + 3];
                i += 3;
            } else {
                if (is_untracked_pch_option(inner)) {
                    rs_log_info("mirror: %s %s is not supported", a, inner);
                    return EXIT_DISTCC_FAILED;
                }
                i++;
                continue;
            }
        } else if (strcmp(a, "-include-pch") == 0
                   || strcmp(a, "-include") == 0
                   || strcmp(a, "--include") == 0
                   || strcmp(a, "-imacros") == 0
                   || strcmp(a, "--imacros") == 0) {
            if (!argv[i + 1])
                break;
            op = a[1] == '-' ? a + 1 : a;
            val = argv[++i];
        } else if (str_startswith("--include=", a)) {
            op = "-include";
            val = a + strlen("--include=");
        } else if (str_startswith("--imacros=", a)) {
            op = "-imacros";
            val = a + strlen("--imacros=");
        } else if (str_startswith("-include", a)
                   && !str_startswith("-include-pch", a)) {
            op = "-include";
            val = a + strlen("-include");
        } else if (str_startswith("-imacros", a)) {
            op = "-imacros";
            val = a + strlen("-imacros");
        } else if (a[0] == '-' && is_untracked_pch_option(a)) {
            rs_log_info("mirror: %s is not supported", a);
            return EXIT_DISTCC_FAILED;
        }

        if (!op)
            continue;
        if (!val || !*val)
            return EXIT_DISTCC_FAILED;
        if (strcmp(op, "-include") == 0)
            ret = add_include(cl, val);
        else
            ret = dcc_mirror_checklist_add(cl, val);
        if (ret)
            return ret;
    }
    return 0;
}


/**
 * Decide whether this job can be mirrored and prepare what to send.
 * Returns 0 if so; nonzero means "use the classic path".
 **/
int dcc_mirror_prepare(char **argv, const char *input_fname,
                       struct dcc_mirror_job *job)
{
    int ret, i;

    if (dcc_is_preprocessed(input_fname))
        return EXIT_DISTCC_FAILED;
    if ((ret = mirror_cwd(&job->cwd)))
        return ret;
    if (!under_mirror_roots(job->cwd)) {
        rs_trace("mirror: %s is outside DISTCC_MIRROR_ROOTS", job->cwd);
        return EXIT_DISTCC_FAILED;
    }
    if ((ret = dcc_mirror_build_checklist(argv, input_fname, &job->checks)))
        return ret;
    for (i = 0; i < job->checks.n; i++) {
        struct dcc_mirror_check *c = &job->checks.items[i];
        if (strchr(c->path, '\n'))
            return EXIT_DISTCC_FAILED;
        /* Metadata only: the daemon's pre-check compares size and mtime.
         * The digests of installed-tree paths are compared after the
         * compile, like every other file. */
        if ((ret = dcc_mirror_ident_of(c->path, 0, &c->ident)))
            return ret;
        /* The request carries sizes and mtimes as 32-bit tokens. */
        if (c->ident.size > (off_t) 0xffffffffu || c->ident.mtime < 0
            || c->ident.mtime > 0xffffffffLL)
            return EXIT_DISTCC_FAILED;
    }
    if ((ret = dcc_mirror_compiler_ident(argv[0], job->cver))) {
        rs_log_info("mirror: cannot identify compiler %s", argv[0]);
        return ret;
    }
    for (i = 0; dcc_mirror_env_names[i]; i++) {
        const char *v = getenv(dcc_mirror_env_names[i]);
        char **n;
        if (!v)
            continue;
        if (!(n = realloc(job->env, (job->n_env + 2) * sizeof *n)))
            return EXIT_OUT_OF_MEMORY;
        job->env = n;
        if (asprintf(&n[job->n_env], "%s=%s", dcc_mirror_env_names[i], v) < 0)
            return EXIT_OUT_OF_MEMORY;
        n[++job->n_env] = NULL;
    }
    if ((ret = dcc_mirror_rules_from_env(&job->rules, job->cwd)))
        return ret;
    return 0;
}


/**
 * Send a version 4 request.  The caller has already connected.
 **/
int dcc_mirror_send_request(int fd, char **argv, struct dcc_mirror_job *job)
{
    int i, ret;

    tcp_cork_sock(fd, 1);
    if ((ret = dcc_x_req_header(fd, DCC_VER_4))
        || (ret = dcc_x_token_string(fd, "CDIR", job->cwd))
        || (ret = dcc_x_argv(fd, "ARGC", "ARGV", argv))
        || (ret = dcc_x_token_int(fd, "NCHK", (unsigned) job->checks.n)))
        return ret;
    for (i = 0; i < job->checks.n; i++) {
        const struct dcc_mirror_check *c = &job->checks.items[i];
        if ((ret = dcc_x_token_string(fd, "CHKN", c->path))
            || (ret = dcc_x_token_int(fd, "CHKX", (unsigned) c->ident.present))
            || (ret = dcc_x_token_int(fd, "CHKS", (unsigned) c->ident.size))
            || (ret = dcc_x_token_int(fd, "CHKM", (unsigned) c->ident.mtime)))
            return ret;
    }
    if ((ret = dcc_x_token_string(fd, "CVER", job->cver))
        || (ret = dcc_x_argv(fd, "NENV", "ENVS", job->env ? job->env
                             : (char *[]) { NULL })))
        return ret;
    {
        char **rules;
        int n_rules;
        if ((ret = dcc_mirror_rules_list(&job->rules, &rules, &n_rules)))
            return ret;
        if (!(rules = realloc(rules, (n_rules + 1) * sizeof *rules)))
            return EXIT_OUT_OF_MEMORY;
        rules[n_rules] = NULL;
        ret = dcc_x_argv(fd, "NRUL", "RULE", rules);
        dcc_free_argv(rules);
        if (ret)
            return ret;
    }
    tcp_cork_sock(fd, 0);
    return 0;
}


/* Remote entries of the directory currently being read ("L" + "N"). */
struct pending_listing {
    char *path;
    char digest[DCC_SHA256_HEX_LEN + 1];
    struct dcc_strset names;
    int active;
};

/* What deciding about a differing installed directory needs; the search
 * order and component positions are only computed if one differs. */
struct listing_ctx {
    char **argv;
    char **files;
    int n_files;
    int ready;
    char **dirs;
    int n_dirs;
    struct dcc_strint positions;
};

static void listing_ctx_free(struct listing_ctx *c)
{
    int i;
    for (i = 0; i < c->n_dirs; i++)
        free(c->dirs[i]);
    free(c->dirs);
    dcc_strint_free(&c->positions);
}

/* Does an entry named @p name, present on one side only, matter for a
 * directory at search position @p d (-1: not a search directory)? */
static int name_matters(struct listing_ctx *c, int d, const char *name)
{
    int p = dcc_strint_get(&c->positions, name);

    if (p < 0)
        return 0;           /* no path the compile read goes through it */
    if (d < 0)
        return 1;           /* includer directory: searched first */
    return p > d;           /* could have been found here first */
}

/**
 * An installed directory differs from the helper's copy.  Accept it if no
 * entry present on only one side (or of a different kind) could have
 * changed what a path the compile read resolved to: its name must be a
 * component of such a path, and the directory must be searched before the
 * directory that path could have been found through.
 **/
static int listing_acceptable(struct dcc_mirror_job *job,
                              struct pending_listing *pl,
                              struct listing_ctx *c)
{
    char **local;
    int n_local, i, ok = 1, d = -1;
    struct dcc_mirror_ident id;
    struct dcc_strset local_set = { NULL, 0, 0 };
    size_t k;
    char *norm;

    if (dcc_mirror_dir_ident(pl->path, &job->rules, &id, &local, &n_local)
        || id.present != DCC_MIRROR_DIR)
        return 0;
    if (strcmp(id.digest, pl->digest) == 0) {
        dcc_mirror_free_names(local, n_local);
        return 1;
    }
    if (!c->ready) {
        if (dcc_mirror_search_order(c->argv, &job->rules, &c->dirs,
                                    &c->n_dirs)
            || dcc_mirror_component_positions(c->files, c->n_files, c->dirs,
                                              c->n_dirs, &job->rules,
                                              &c->positions)) {
            dcc_mirror_free_names(local, n_local);
            return 0;
        }
        c->ready = 1;
    }
    if (!(norm = dcc_mirror_normalize(&job->rules, pl->path))) {
        dcc_mirror_free_names(local, n_local);
        return 0;
    }
    for (i = 0; i < c->n_dirs; i++) {
        if (strcmp(c->dirs[i], norm) == 0) {
            d = i;
            break;
        }
    }
    free(norm);

    for (i = 0; i < n_local && ok; i++) {
        if (dcc_strset_add(&local_set, local[i]))
            ok = 0;
        else if (!dcc_strset_has(&pl->names, local[i])
                 && name_matters(c, d, local[i] + 1)) {
            rs_log_info("mirror: %s/%s exists only here", pl->path,
                        local[i] + 1);
            ok = 0;
        }
    }
    for (k = 0; k < pl->names.cap && ok; k++) {
        const char *e = pl->names.items[k];
        if (e && !dcc_strset_has(&local_set, e)
            && name_matters(c, d, e + 1)) {
            rs_log_info("mirror: %s/%s exists only on the helper", pl->path,
                        e + 1);
            ok = 0;
        }
    }
    dcc_strset_free(&local_set);
    dcc_mirror_free_names(local, n_local);
    return ok;
}

static void pending_reset(struct pending_listing *pl)
{
    free(pl->path);
    dcc_strset_free(&pl->names);
    memset(pl, 0, sizeof *pl);
}


/**
 * Compare the daemon's description of what its compile read (DSTA) with
 * this machine.  It must describe every prerequisite of the returned .d
 * and every check-list path (files), and every search directory, the
 * directory of each of those files and the cwd (directories).  Files must
 * be identical; synced directories must have the same entries; installed
 * directories may differ only in entries that no path the compile read
 * could have been spelled through.  Returns 0 on a full match.
 **/
int dcc_mirror_verify(struct dcc_mirror_job *job, char **argv,
                      char *dsta, size_t dsta_len,
                      const char *dotd_text, size_t dotd_len,
                      char **first_mismatch)
{
    char **dotd_paths = NULL, **files = NULL;
    int n_dotd = 0, n_files, i, ret = 0;
    struct dcc_strset file_set = { NULL, 0, 0 }, dir_set = { NULL, 0, 0 };
    struct dcc_strset comps = { NULL, 0, 0 };
    struct dcc_strset seen_files = { NULL, 0, 0 }, seen_dirs = { NULL, 0, 0 };
    struct pending_listing pl;
    struct listing_ctx lctx;
    char *line, *next, *end = dsta + dsta_len;
    size_t k;

    memset(&pl, 0, sizeof pl);
    memset(&lctx, 0, sizeof lctx);
    *first_mismatch = NULL;
    if (dotd_len && (ret = dcc_mirror_parse_dotd(dotd_text, dotd_len,
                                                 &dotd_paths, &n_dotd)))
        return ret;
    n_files = n_dotd + job->checks.n;
    if (!(files = calloc((size_t) n_files + 1, sizeof *files))) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }
    for (i = 0; i < n_dotd; i++)
        files[i] = dotd_paths[i];
    for (i = 0; i < job->checks.n; i++)
        files[n_dotd + i] = job->checks.items[i].path;
    if ((ret = dcc_mirror_required(argv, files, n_files, &file_set,
                                   &dir_set, &comps)))
        goto out;
    lctx.argv = argv;
    lctx.files = files;
    lctx.n_files = n_files;

#define MISMATCH(p) do { ret = EXIT_DISTCC_FAILED; \
        if (!*first_mismatch) *first_mismatch = strdup(p); goto out; } while (0)

    for (line = dsta; line < end; line = next) {
        struct dcc_mirror_ident remote, local;
        char *path, *nl = memchr(line, '\n', (size_t) (end - line));

        if (!nl) {
            ret = EXIT_PROTOCOL_ERROR;
            goto out;
        }
        *nl = '\0';
        next = nl + 1;

        if (line[0] == 'N' && line[1] == ' ') {
            if (!pl.active || !line[2]) {
                ret = EXIT_PROTOCOL_ERROR;
                goto out;
            }
            if ((ret = dcc_strset_add(&pl.names, line + 2)))
                goto out;
            continue;
        }
        if (pl.active) {
            if (!listing_acceptable(job, &pl, &lctx))
                MISMATCH(pl.path);
            pending_reset(&pl);
        }
        if (dcc_mirror_parse_dsta_line(line, &path, &remote) != 0) {
            rs_log_error("mirror: malformed DSTA line: %s", line);
            ret = EXIT_PROTOCOL_ERROR;
            goto out;
        }
        if (line[0] == 'L') {
            if (!(pl.path = strdup(path))) {
                ret = EXIT_OUT_OF_MEMORY;
                goto out;
            }
            memcpy(pl.digest, remote.digest, sizeof pl.digest);
            pl.active = 1;
            if ((ret = dcc_strset_add(&seen_dirs, path)))
                goto out;
            continue;
        }

        if (dcc_strset_has(&dir_set, path)
            && (remote.present != DCC_MIRROR_FILE
                || !dcc_strset_has(&file_set, path))) {
            if ((ret = dcc_mirror_dir_ident(path, &job->rules, &local,
                                            NULL, NULL)))
                goto out;
            if (!dcc_mirror_ident_equal(&remote, &local, 1))
                MISMATCH(path);
            if ((ret = dcc_strset_add(&seen_dirs, path)))
                goto out;
        }
        if (dcc_strset_has(&file_set, path) && remote.present != DCC_MIRROR_DIR) {
            int installed = dcc_mirror_is_installed_path(path);
            if (installed && remote.present == DCC_MIRROR_FILE
                && !remote.digest[0])
                MISMATCH(path);
            if ((ret = dcc_mirror_ident_of(path, installed
                                           && remote.present == DCC_MIRROR_FILE,
                                           &local)))
                goto out;
            if (!dcc_mirror_ident_equal(&remote, &local, installed))
                MISMATCH(path);
            if ((ret = dcc_strset_add(&seen_files, path)))
                goto out;
        }
    }
    if (pl.active) {
        if (!listing_acceptable(job, &pl, &lctx))
            MISMATCH(pl.path);
        pending_reset(&pl);
    }

    for (k = 0; k < file_set.cap; k++) {
        const char *f = file_set.items[k];
        if (f && !dcc_strset_has(&seen_files, f)) {
            rs_log_error("mirror: daemon did not describe file %s", f);
            ret = EXIT_PROTOCOL_ERROR;
            *first_mismatch = strdup(f);
            goto out;
        }
    }
    for (k = 0; k < dir_set.cap; k++) {
        const char *d = dir_set.items[k];
        if (d && !dcc_strset_has(&seen_dirs, d)) {
            rs_log_error("mirror: daemon did not describe directory %s", d);
            ret = EXIT_PROTOCOL_ERROR;
            *first_mismatch = strdup(d);
            goto out;
        }
    }
#undef MISMATCH

  out:
    pending_reset(&pl);
    listing_ctx_free(&lctx);
    free(files);
    dcc_mirror_free_paths(dotd_paths, n_dotd);
    dcc_strset_free(&file_set);
    dcc_strset_free(&dir_set);
    dcc_strset_free(&comps);
    dcc_strset_free(&seen_files);
    dcc_strset_free(&seen_dirs);
    return ret;
}


/* Read a token-introduced blob into memory. */
static int read_blob(int fd, const char *token, char **buf, size_t *len)
{
    unsigned n;
    int ret;

    *buf = NULL;
    *len = 0;
    if ((ret = dcc_r_token_int(fd, token, &n)))
        return ret;
    if (n > 256u * 1024 * 1024)
        return EXIT_PROTOCOL_ERROR;
    if (!(*buf = malloc((size_t) n + 1)))
        return EXIT_OUT_OF_MEMORY;
    if (n && (ret = dcc_readx(fd, *buf, n))) {
        free(*buf);
        *buf = NULL;
        return ret;
    }
    (*buf)[n] = '\0';
    *len = n;
    return 0;
}

/* Write @p len bytes to a new file @p fname. */
static int write_whole_file(const char *fname, const char *buf, size_t len)
{
    int fd, ret = 0;

    if ((fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666))
        == -1) {
        rs_log_error("failed to create %s: %s", fname, strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (len && dcc_writex(fd, buf, len))
        ret = EXIT_IO_ERROR;
    if (close(fd) == -1)
        ret = EXIT_IO_ERROR;
    return ret;
}

/* A temporary name next to @p fname, so that rename() stays on one file
 * system. */
static int sibling_tmpnam(const char *fname, char **tmp)
{
    if (asprintf(tmp, "%s.distcc-mirror-%ld.tmp", fname, (long) getpid()) < 0) {
        *tmp = NULL;
        return EXIT_OUT_OF_MEMORY;
    }
    if (dcc_add_cleanup(*tmp)) {
        free(*tmp);
        *tmp = NULL;
        return EXIT_OUT_OF_MEMORY;
    }
    return 0;
}


/**
 * Read a version 4 response.
 *
 * On return *mirr is the daemon's MIRR code.  If it is nonzero nothing else
 * was read and the job should take the classic path.  If it is zero,
 * *status is the compiler's status; on success the object and .d have been
 * verified and written, unless *verify_failed is set, in which case nothing
 * was written and the job must be redone elsewhere.
 **/
int dcc_mirror_retrieve_results(int fd, int *status,
                                const char *output_fname,
                                const char *deps_fname,
                                const char *server_stderr_fname,
                                struct dcc_mirror_job *job,
                                int *mirr, int *verify_failed)
{
    unsigned vers, code, len, o_len;
    char *obj = NULL, *dotd = NULL, *dsta = NULL, *mismatch = NULL;
    char *sout = NULL;
    size_t obj_len = 0, dotd_len = 0, dsta_len = 0, sout_len = 0;
    char *tmp_o = NULL, *tmp_d = NULL;
    struct timeval before, after;
    int ret;

    *mirr = DCC_MIRR_OK;
    *verify_failed = 0;
    *status = 0;

    if ((ret = dcc_r_token_int(fd, "DONE", &vers)))
        return ret;
    if (vers != DCC_VER_4) {
        rs_log_error("got version %u not 4 in response from server", vers);
        return EXIT_PROTOCOL_ERROR;
    }
    if ((ret = dcc_r_token_int(fd, "MIRR", &code)))
        return ret;
    if (code != DCC_MIRR_OK) {
        char *why = NULL;
        *mirr = (int) code;
        if (dcc_r_token_string(fd, "MIRM", &why) == 0) {
            rs_log(RS_LOG_INFO|RS_LOG_NONAME, "mirror: MIRR %u: %s",
                   code, why);
            free(why);
        }
        return 0;
    }

    dcc_note_state(DCC_PHASE_RECEIVE, NULL, NULL, DCC_REMOTE);

    /* Compiler stdout is held back until the result is accepted, so that
     * a rejected job does not print it twice. */
    if ((ret = dcc_r_cc_status(fd, status))
        || (ret = dcc_r_token_int(fd, "SERR", &len))
        || (ret = dcc_r_file(fd, server_stderr_fname, len, DCC_COMPRESS_NONE))
        || (ret = read_blob(fd, "SOUT", &sout, &sout_len)))
        goto out;

    if ((ret = dcc_r_token_int(fd, "DOTO", &o_len)))
        goto out;
    if (*status != 0) {
        if (o_len != 0)
            rs_log_error("remote compiler failed but also returned output");
        goto out;
    }
    if (o_len > 1024u * 1024 * 1024) {
        ret = EXIT_PROTOCOL_ERROR;
        goto out;
    }
    if (!(obj = malloc(o_len ? o_len : 1))) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }
    if (o_len && (ret = dcc_readx(fd, obj, o_len)))
        goto out;
    obj_len = o_len;
    if ((ret = read_blob(fd, "DOTD", &dotd, &dotd_len))
        || (ret = read_blob(fd, "DSTA", &dsta, &dsta_len)))
        goto out;

    gettimeofday(&before, NULL);
    ret = dcc_mirror_verify(job, job->argv, dsta, dsta_len, dotd, dotd_len,
                            &mismatch);
    gettimeofday(&after, NULL);
    rs_log_info("mirror: checked %lu bytes of file identities in %ldus",
                (unsigned long) dsta_len,
                (long) ((after.tv_sec - before.tv_sec) * 1000000
                        + (after.tv_usec - before.tv_usec)));
    if (ret) {
        rs_log(RS_LOG_WARNING|RS_LOG_NONAME,
               "mirror: %s differs from the helper's copy; not using the "
               "remote result", mismatch ? mismatch : "(unknown)");
        *verify_failed = 1;
        ret = 0;
        goto out;
    }

    /* Write next to the destination, then rename into place. */
    if ((ret = sibling_tmpnam(output_fname, &tmp_o))
        || (ret = write_whole_file(tmp_o, obj, obj_len)))
        goto out;
    if (deps_fname) {
        if ((ret = sibling_tmpnam(deps_fname, &tmp_d))
            || (ret = write_whole_file(tmp_d, dotd, dotd_len)))
            goto out;
    }
    if (rename(tmp_o, output_fname) == -1) {
        rs_log_error("rename %s failed: %s", output_fname, strerror(errno));
        ret = EXIT_IO_ERROR;
        goto out;
    }
    if (tmp_d && rename(tmp_d, deps_fname) == -1) {
        rs_log_error("rename %s failed: %s", deps_fname, strerror(errno));
        unlink(output_fname);
        ret = EXIT_IO_ERROR;
        goto out;
    }
    if (sout_len)
        dcc_writex(STDOUT_FILENO, sout, sout_len);

  out:
    if (tmp_o) unlink(tmp_o);
    if (tmp_d) unlink(tmp_d);
    free(tmp_o);
    free(tmp_d);
    free(obj);
    free(dotd);
    free(dsta);
    free(sout);
    free(mismatch);
    dcc_mirror_digest_flush();
    return ret;
}

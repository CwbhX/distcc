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
    NULL
};


void dcc_mirror_job_init(struct dcc_mirror_job *job)
{
    memset(job, 0, sizeof *job);
}

void dcc_mirror_job_free(struct dcc_mirror_job *job)
{
    free(job->cwd);
    dcc_mirror_checklist_free(&job->checks);
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
    tcp_cork_sock(fd, 0);
    return 0;
}


/* A set of paths, to check that DSTA covers what it must. */
struct path_set {
    char **items;
    int n;
    unsigned char *seen;
};

static int path_set_index(const struct path_set *s, const char *path)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (strcmp(s->items[i], path) == 0)
            return i;
    return -1;
}


/**
 * Compare the daemon's description of the files its compile read (DSTA)
 * with this machine's files.  Every path of the check list and every
 * prerequisite of the returned .d must be described, and every described
 * file must be identical here.  Returns 0 on a full match.
 **/
int dcc_mirror_verify(struct dcc_mirror_job *job, char *dsta, size_t dsta_len,
                      const char *dotd_text, size_t dotd_len,
                      char **first_mismatch)
{
    struct path_set dotd = { NULL, 0, NULL };
    unsigned char *check_seen = NULL;
    char *line, *next, *end = dsta + dsta_len;
    int ret = 0, i;

    *first_mismatch = NULL;
    if (dotd_text && dotd_len
        && (ret = dcc_mirror_parse_dotd(dotd_text, dotd_len,
                                        &dotd.items, &dotd.n)))
        return ret;
    dotd.seen = calloc(dotd.n + 1, 1);
    check_seen = calloc(job->checks.n + 1, 1);
    if (!dotd.seen || !check_seen) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }

    for (line = dsta; line < end; line = next) {
        struct dcc_mirror_ident remote, local;
        char *path, *nl = memchr(line, '\n', (size_t) (end - line));
        int installed, idx;

        if (!nl) {
            ret = EXIT_PROTOCOL_ERROR;
            break;
        }
        *nl = '\0';
        next = nl + 1;
        if (dcc_mirror_parse_dsta_line(line, &path, &remote) != 0) {
            rs_log_error("mirror: malformed DSTA line: %s", line);
            ret = EXIT_PROTOCOL_ERROR;
            break;
        }
        installed = dcc_mirror_is_installed_path(path);
        if (installed && remote.present == DCC_MIRROR_FILE
            && !remote.digest[0]) {
            ret = EXIT_DISTCC_FAILED;   /* installed tree needs a digest */
        } else if ((ret = dcc_mirror_ident_of(path,
                                              installed && remote.present
                                              == DCC_MIRROR_FILE,
                                              &local))) {
            ;
        } else if (!dcc_mirror_ident_equal(&remote, &local, installed)) {
            ret = EXIT_DISTCC_FAILED;
        }
        if (ret) {
            *first_mismatch = strdup(path);
            break;
        }
        for (i = 0; i < job->checks.n; i++)
            if (strcmp(job->checks.items[i].path, path) == 0)
                check_seen[i] = 1;
        if ((idx = path_set_index(&dotd, path)) >= 0)
            dotd.seen[idx] = 1;
    }

    if (!ret) {
        for (i = 0; i < job->checks.n && !ret; i++) {
            if (!check_seen[i]) {
                rs_log_error("mirror: daemon did not describe %s",
                             job->checks.items[i].path);
                *first_mismatch = strdup(job->checks.items[i].path);
                ret = EXIT_PROTOCOL_ERROR;
            }
        }
        for (i = 0; i < dotd.n && !ret; i++) {
            if (!dotd.seen[i]) {
                rs_log_error("mirror: daemon did not describe %s",
                             dotd.items[i]);
                *first_mismatch = strdup(dotd.items[i]);
                ret = EXIT_PROTOCOL_ERROR;
            }
        }
    }

  out:
    free(check_seen);
    free(dotd.seen);
    dcc_mirror_free_paths(dotd.items, dotd.n);
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
    size_t obj_len = 0, dotd_len = 0, dsta_len = 0;
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
        *mirr = (int) code;
        return 0;
    }

    dcc_note_state(DCC_PHASE_RECEIVE, NULL, NULL, DCC_REMOTE);

    if ((ret = dcc_r_cc_status(fd, status))
        || (ret = dcc_r_token_int(fd, "SERR", &len))
        || (ret = dcc_r_file(fd, server_stderr_fname, len, DCC_COMPRESS_NONE))
        || (ret = dcc_r_token_int(fd, "SOUT", &len))
        || (ret = dcc_r_bulk(STDOUT_FILENO, fd, len, DCC_COMPRESS_NONE)))
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
    ret = dcc_mirror_verify(job, dsta, dsta_len, dotd, dotd_len, &mismatch);
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

  out:
    if (tmp_o) unlink(tmp_o);
    if (tmp_d) unlink(tmp_d);
    free(tmp_o);
    free(tmp_d);
    free(obj);
    free(dotd);
    free(dsta);
    free(mismatch);
    dcc_mirror_digest_flush();
    return ret;
}

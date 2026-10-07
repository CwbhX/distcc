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

/* Mirrored-tree mode, daemon side: serve a protocol 4 request.
 *
 * The client sends its cwd, the compiler command and a check list; no
 * source.  The daemon compiles in its copy of the client's tree, with
 * writes confined to a fresh job directory, and returns the object, the
 * .d, and the identity of every file the compile read (DSTA) so that the
 * client can check them against its own before accepting the object. */

#include <config.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "rpc.h"
#include "bulk.h"
#include "util.h"
#include "exec.h"
#include "dopt.h"
#include "daemon.h"
#include "stats.h"
#include "mirror.h"
#include "mirror_serve.h"
#include "confine.h"

/* Options that write files or load code we do not track.  Refused even
 * though the confinement would stop the writes, so that the reason is clear
 * and the job goes back to the classic path straight away. */
static const char *const refused_prefixes[] = {
    "-save-temps", "--save-temps", "-ftime-trace", "-gsplit-dwarf",
    "-serialize-diagnostics", "--serialize-diagnostics", "-fmodules",
    "-fmodule-", "-fimplicit-module", "-fprebuilt-module",
    "-fcrash-diagnostics", "-emit-pch", "-MJ", "-fprofile", "-fdump",
    "-fplugin", "-load", "--analyze", "-fstack-usage", "-fsave-optimization-record",
    "-foptimization-record-file", "-fdiagnostics-format=sarif",
    NULL
};

/* Operands allowed after -Xclang / -Xpreprocessor (with their own operand). */
static const char *const allowed_x_operands[] = {
    "-include-pch", "-include", "-imacros", NULL
};


static int under_root(const char *resolved)
{
    int i;

    for (i = 0; i < opt_n_mirror_roots; i++) {
        size_t len = strlen(opt_mirror_roots[i]);
        if (strncmp(resolved, opt_mirror_roots[i], len) == 0
            && (resolved[len] == '/' || resolved[len] == '\0'
                || (len == 1 && opt_mirror_roots[i][0] == '/')))
            return 1;
    }
    return 0;
}


/**
 * Apply the argument policy.  Returns 0 if allowed; otherwise logs the
 * offending argument.
 **/
static int mirror_check_args(char **argv)
{
    int i, j;

    for (i = 1; argv[i]; i++) {
        const char *a = argv[i];
        for (j = 0; refused_prefixes[j]; j++) {
            if (str_startswith(refused_prefixes[j], a)) {
                rs_log_warning("mirror: refusing argument %s", a);
                return EXIT_BAD_ARGUMENTS;
            }
        }
        if (strcmp(a, "-x") == 0 && argv[i + 1]
            && strstr(argv[i + 1], "-header")) {
            rs_log_warning("mirror: refusing -x %s", argv[i + 1]);
            return EXIT_BAD_ARGUMENTS;
        }
        if (strcmp(a, "-Xclang") == 0 || strcmp(a, "-Xpreprocessor") == 0) {
            const char *op = argv[i + 1];
            int ok = 0;
            for (j = 0; op && allowed_x_operands[j]; j++)
                if (strcmp(op, allowed_x_operands[j]) == 0)
                    ok = 1;
            if (!ok || !argv[i + 2] || strcmp(argv[i + 2], a) != 0
                || !argv[i + 3] || argv[i + 3][0] == '-') {
                rs_log_warning("mirror: refusing %s %s", a, op ? op : "");
                return EXIT_BAD_ARGUMENTS;
            }
            i += 3;
        }
    }
    return 0;
}


/**
 * Point -o and the dependency output at the job directory: drop every -MF,
 * make sure -MD is present (a -MMD becomes -MD, so that system headers are
 * listed too and get checked), and set -o.
 **/
static int mirror_tweak_args(char ***argvp, const char *obj_fname,
                             const char *dotd_fname)
{
    char **argv = *argvp, **out;
    int i, n = 0, has_md = 0;

    if (!(out = calloc(dcc_argv_len(argv) + 6, sizeof *out)))
        return EXIT_OUT_OF_MEMORY;
    for (i = 0; argv[i]; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-MF") == 0) {
            if (argv[i + 1])
                i++;
            continue;
        }
        if (str_startswith("-MF", a))
            continue;
        if (strcmp(a, "-MD") == 0 || strcmp(a, "-MMD") == 0) {
            if (has_md)
                continue;
            has_md = 1;
            a = "-MD";
        }
        if (!(out[n++] = strdup(a)))
            goto oom;
    }
    if (!has_md && !(out[n++] = strdup("-MD")))
        goto oom;
    if (!(out[n++] = strdup("-MF")) || !(out[n++] = strdup(dotd_fname)))
        goto oom;
    out[n] = NULL;
    dcc_free_argv(argv);
    *argvp = out;
    return dcc_set_output(out, (char *) obj_fname);

  oom:
    dcc_free_argv(out);
    return EXIT_OUT_OF_MEMORY;
}


/* Append one DSTA line for @p path to the growing buffer. */
static int dsta_append(char **buf, size_t *len, size_t *cap,
                       const char *path)
{
    struct dcc_mirror_ident ident;
    char *line;
    size_t l;
    int ret;

    if ((ret = dcc_mirror_ident_of(path, dcc_mirror_is_installed_path(path),
                                   &ident)))
        return ret;
    if ((ret = dcc_mirror_format_dsta_line(path, &ident, &line)))
        return ret;
    l = strlen(line);
    if (*len + l + 1 > *cap) {
        size_t ncap = (*cap ? *cap * 2 : 65536);
        char *nb;
        while (ncap < *len + l + 1)
            ncap *= 2;
        if (!(nb = realloc(*buf, ncap))) {
            free(line);
            return EXIT_OUT_OF_MEMORY;
        }
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, line, l);
    *len += l;
    free(line);
    return 0;
}


/* Remove whatever the compiler left in the job directory (the directory
 * itself is removed by the daemon's cleanup list). */
static void empty_job_dir(const char *dir)
{
    DIR *d;
    struct dirent *de;
    char *p;

    if (!dir || !(d = opendir(dir)))
        return;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (asprintf(&p, "%s/%s", dir, de->d_name) >= 0) {
            unlink(p);
            free(p);
        }
    }
    closedir(d);
}


static int send_blob(int fd, const char *token, const char *buf, size_t len)
{
    int ret;

    if ((ret = dcc_x_token_int(fd, token, (unsigned) len)))
        return ret;
    if (len && (ret = dcc_writex(fd, buf, len)))
        return ret;
    return 0;
}


static int send_refusal(int out_fd, int code)
{
    int ret;

    if ((ret = dcc_x_result_header(out_fd, DCC_VER_4))
        || (ret = dcc_x_token_int(out_fd, "MIRR", (unsigned) code)))
        return ret;
    return 0;
}


/**
 * Serve a protocol 4 request whose header has been read.
 *
 * @p err_fname and @p out_fname receive the compiler's stderr and stdout.
 * On return *argv_ret and *input_ret describe the job for the daemon's log
 * and *job_result holds a STATS_ code.
 **/
int dcc_mirror_serve(int in_fd, int out_fd,
                     const char *err_fname, const char *out_fname,
                     char ***argv_ret, char **input_ret, int *status_ret,
                     int *job_result)
{
    char *cwd = NULL, **argv = NULL, **scanned = NULL;
    char *input_tmp, *output_tmp;
    char *job_dir = NULL, *obj_fname = NULL, *dotd_fname = NULL;
    char real[MAXPATHLEN + 1];
    struct dcc_mirror_checklist checks = { NULL, 0, 0 };
    unsigned n_checks, i;
    int code = DCC_MIRR_OK, ret = 0, status = 0, confine_failed = 0;
    int changed_dir = 0;
    char *dotd_text = NULL, *dsta = NULL, **dotd_paths = NULL;
    size_t dsta_len = 0, dsta_cap = 0;
    int n_dotd = 0;
    pid_t pid;
    char *saved_tmpdir = NULL;

    *status_ret = 0;
    *job_result = STATS_OTHER;

    /* Read the whole request first. */
    if ((ret = dcc_r_token_string(in_fd, "CDIR", &cwd))
        || (ret = dcc_r_argv(in_fd, "ARGC", "ARGV", &argv))
        || (ret = dcc_r_token_int(in_fd, "NCHK", &n_checks)))
        goto out;
    if (n_checks > 100000) {
        ret = EXIT_PROTOCOL_ERROR;
        goto out;
    }
    for (i = 0; i < n_checks; i++) {
        char *path = NULL;
        unsigned kind, size, mtime;
        if ((ret = dcc_r_token_string(in_fd, "CHKN", &path))
            || (ret = dcc_r_token_int(in_fd, "CHKX", &kind))
            || (ret = dcc_r_token_int(in_fd, "CHKS", &size))
            || (ret = dcc_r_token_int(in_fd, "CHKM", &mtime))) {
            free(path);
            goto out;
        }
        if ((ret = dcc_mirror_checklist_add(&checks, path))) {
            free(path);
            goto out;
        }
        free(path);
        checks.items[checks.n - 1].ident.present = (int) kind;
        checks.items[checks.n - 1].ident.size = (off_t) size;
        checks.items[checks.n - 1].ident.mtime = (long long) mtime;
    }
    dcc_trace_argv("mirror request", argv);

    if (dcc_scan_args(argv, &input_tmp, &output_tmp, &scanned) != 0) {
        rs_log_warning("mirror: command is not a single compile");
        code = DCC_MIRR_ARG_POLICY;
        goto refuse;
    }
    dcc_free_argv(argv);
    argv = scanned;
    scanned = NULL;
    if (!(*input_ret = strdup(input_tmp))) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }

    if (opt_n_mirror_roots == 0) {
        code = DCC_MIRR_DISABLED;
        goto refuse;
    }
    if (cwd[0] != '/' || !realpath(cwd, real)) {
        rs_log_warning("mirror: cwd %s: %s", cwd, strerror(errno));
        code = DCC_MIRR_MISSING;
        goto refuse;
    }
    if (!under_root(real)) {
        rs_log_warning("mirror: cwd %s is not under a --mirror-root", real);
        code = DCC_MIRR_DISABLED;
        goto refuse;
    }
    if (chdir(cwd) == -1) {
        code = DCC_MIRR_MISSING;
        goto refuse;
    }
    changed_dir = 1;
    if (!realpath(*input_ret, real)) {
        rs_log_warning("mirror: input %s: %s", *input_ret, strerror(errno));
        code = DCC_MIRR_MISSING;
        goto refuse;
    }
    if (!under_root(real)) {
        rs_log_warning("mirror: input %s is not under a --mirror-root", real);
        code = DCC_MIRR_DISABLED;
        goto refuse;
    }

    if (mirror_check_args(argv) != 0
        || dcc_check_compiler_and_args(argv) != 0) {
        code = DCC_MIRR_ARG_POLICY;
        goto refuse;
    }

    /* Pre-check: synced-tree files must have the client's size and mtime.
     * Installed-tree files are compared by digest after the compile. */
    for (i = 0; i < (unsigned) checks.n; i++) {
        struct dcc_mirror_ident here;
        const struct dcc_mirror_check *c = &checks.items[i];
        if (dcc_mirror_is_installed_path(c->path))
            continue;
        if ((ret = dcc_mirror_ident_of(c->path, 0, &here)))
            goto out;
        if (!dcc_mirror_ident_equal(&c->ident, &here, 0)) {
            rs_log_warning("mirror: %s differs from the client's copy",
                           c->path);
            code = DCC_MIRR_STALE;
            goto refuse;
        }
    }

    /* Job directory, the only place the compiler may write. */
    if ((ret = dcc_get_new_tmpdir(&job_dir)))
        goto out;
    if (!realpath(job_dir, real)) {
        ret = EXIT_IO_ERROR;
        goto out;
    }
    free(job_dir);
    if (!(job_dir = strdup(real))
        || asprintf(&obj_fname, "%s/out.o", job_dir) < 0
        || asprintf(&dotd_fname, "%s/out.d", job_dir) < 0) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }
    if ((ret = dcc_add_cleanup(obj_fname)) || (ret = dcc_add_cleanup(dotd_fname)))
        goto out;
    if ((ret = mirror_tweak_args(&argv, obj_fname, dotd_fname)))
        goto out;

    if (getenv("TMPDIR"))
        saved_tmpdir = strdup(getenv("TMPDIR"));
    setenv("TMPDIR", job_dir, 1);
    ret = dcc_spawn_confined(argv, &pid, "/dev/null", out_fname, err_fname,
                             job_dir, &confine_failed);
    if (saved_tmpdir)
        setenv("TMPDIR", saved_tmpdir, 1);
    else
        unsetenv("TMPDIR");
    if (confine_failed) {
        rs_log_error("mirror: could not confine the compiler; refusing");
        code = DCC_MIRR_NO_CONFINE;
        ret = 0;
        goto refuse;
    }
    if (ret)
        goto out;
    if ((ret = dcc_collect_child("cc", pid, &status, in_fd))) {
        status = W_EXITCODE(ret, 0);
        if (ret == EXIT_IO_ERROR || ret == EXIT_TIMEOUT)
            goto out;
        ret = 0;
    }
    *status_ret = status;

    /* Describe what the compile read, while still in its cwd. */
    if (!WIFSIGNALED(status) && WEXITSTATUS(status) == 0) {
        if ((ret = dcc_load_file_string(dotd_fname, &dotd_text))
            || (ret = dcc_mirror_parse_dotd(dotd_text, strlen(dotd_text),
                                            &dotd_paths, &n_dotd)))
            goto out;
        for (i = 0; i < (unsigned) n_dotd; i++)
            if ((ret = dsta_append(&dsta, &dsta_len, &dsta_cap,
                                   dotd_paths[i])))
                goto out;
        for (i = 0; i < (unsigned) checks.n; i++)
            if ((ret = dsta_append(&dsta, &dsta_len, &dsta_cap,
                                   checks.items[i].path)))
                goto out;
    }
    if (chdir(dcc_daemon_wd) == -1)
        rs_log_warning("chdir(%s) failed: %s", dcc_daemon_wd, strerror(errno));
    changed_dir = 0;

    if ((ret = dcc_x_result_header(out_fd, DCC_VER_4))
        || (ret = dcc_x_token_int(out_fd, "MIRR", DCC_MIRR_OK))
        || (ret = dcc_x_cc_status(out_fd, status))
        || (ret = dcc_x_file(out_fd, err_fname, "SERR", DCC_COMPRESS_NONE, NULL))
        || (ret = dcc_x_file(out_fd, out_fname, "SOUT", DCC_COMPRESS_NONE, NULL)))
        goto out;
    if (WIFSIGNALED(status) || WEXITSTATUS(status)) {
        ret = dcc_x_token_int(out_fd, "DOTO", 0);
        *job_result = STATS_COMPILE_ERROR;
        goto out;
    }
    if ((ret = dcc_x_file(out_fd, obj_fname, "DOTO", DCC_COMPRESS_NONE, NULL))
        || (ret = send_blob(out_fd, "DOTD", dotd_text, strlen(dotd_text)))
        || (ret = send_blob(out_fd, "DSTA", dsta, dsta_len)))
        goto out;
    *job_result = STATS_COMPILE_OK;
    goto out;

  refuse:
    rs_log_info("mirror: answering MIRR %d", code);
    ret = send_refusal(out_fd, code);
    *job_result = STATS_OTHER;

  out:
    if (changed_dir && chdir(dcc_daemon_wd) == -1)
        rs_log_warning("chdir(%s) failed: %s", dcc_daemon_wd, strerror(errno));
    free(saved_tmpdir);
    free(cwd);
    if (scanned)
        dcc_free_argv(scanned);
    *argv_ret = argv;
    dcc_mirror_checklist_free(&checks);
    empty_job_dir(job_dir);
    free(job_dir);
    free(obj_fname);
    free(dotd_fname);
    free(dotd_text);
    free(dsta);
    dcc_mirror_free_paths(dotd_paths, n_dotd);
    dcc_mirror_digest_flush();
    return ret;
}

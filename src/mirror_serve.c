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


static int buf_append(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t l = strlen(s);

    if (*len + l + 1 > *cap) {
        size_t ncap = (*cap ? *cap * 2 : 65536);
        char *nb;
        while (ncap < *len + l + 1)
            ncap *= 2;
        if (!(nb = realloc(*buf, ncap)))
            return EXIT_OUT_OF_MEMORY;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, s, l);
    *len += l;
    return 0;
}

static int dsta_append_ident(char **buf, size_t *len, size_t *cap,
                             const char *path,
                             const struct dcc_mirror_ident *ident)
{
    char *line;
    int ret;

    if ((ret = dcc_mirror_format_dsta_line(path, ident, &line)))
        return ret;
    ret = buf_append(buf, len, cap, line);
    free(line);
    return ret;
}

/* Test hooks, read from the daemon's own environment: leave a path out of
 * DSTA, or describe it as if the helper's copy were different. */
static int testing_hook(const char *var, const char *path)
{
    const char *v = getenv(var);
    return v && *v && strcmp(v, path) == 0;
}

static void testing_skew(struct dcc_mirror_ident *ident)
{
    ident->mtime += 7;
    if (ident->digest[0])
        ident->digest[0] = ident->digest[0] == '0' ? '1' : '0';
}

/* Append the DSTA line for file @p path. */
static int dsta_append(char **buf, size_t *len, size_t *cap,
                       const char *path)
{
    struct dcc_mirror_ident ident;
    int ret;

    if (testing_hook("DISTCC_TESTING_MIRROR_OMIT", path))
        return 0;
    if ((ret = dcc_mirror_ident_of(path, dcc_mirror_is_installed_path(path),
                                   &ident)))
        return ret;
    if (testing_hook("DISTCC_TESTING_MIRROR_SKEW", path))
        testing_skew(&ident);
    return dsta_append_ident(buf, len, cap, path, &ident);
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


static int send_refusal(int out_fd, int code, const char *why)
{
    int ret;

    if ((ret = dcc_x_result_header(out_fd, DCC_VER_4))
        || (ret = dcc_x_token_int(out_fd, "MIRR", (unsigned) code))
        || (ret = dcc_x_token_string(out_fd, "MIRM", why ? why : "")))
        return ret;
    return 0;
}


/* Set (or unset) the environment variables that change what the compiler
 * reads to the client's values; remember the old ones in @p saved. */
static int apply_client_env(char **env, char ***saved)
{
    int i, n = 0, ret = 0;

    for (i = 0; dcc_mirror_env_names[i]; i++)
        n++;
    if (!(*saved = calloc((size_t) n + 1, sizeof **saved)))
        return EXIT_OUT_OF_MEMORY;
    for (i = 0; dcc_mirror_env_names[i]; i++) {
        const char *name = dcc_mirror_env_names[i];
        const char *old = getenv(name);
        size_t len = strlen(name);
        int j, set = 0;
        if (old && asprintf(&(*saved)[i], "%s=%s", name, old) < 0)
            ret = EXIT_OUT_OF_MEMORY;
        for (j = 0; env && env[j]; j++) {
            if (strncmp(env[j], name, len) == 0 && env[j][len] == '=') {
                setenv(name, env[j] + len + 1, 1);
                set = 1;
            }
        }
        if (!set)
            unsetenv(name);
    }
    return ret;
}

static void restore_env(char **saved)
{
    int i;

    if (!saved)
        return;
    for (i = 0; dcc_mirror_env_names[i]; i++) {
        const char *name = dcc_mirror_env_names[i];
        if (saved[i])
            setenv(name, saved[i] + strlen(name) + 1, 1);
        else
            unsetenv(name);
        free(saved[i]);
    }
    free(saved);
}


/* Describe one directory: "L" with its entries for an installed tree (the
 * client tolerates harmless differences there), "D" otherwise. */
static int dsta_append_dir(char **buf, size_t *len, size_t *cap,
                           const char *path, const struct dcc_mirror_rules *r)
{
    struct dcc_mirror_ident ident;
    char **names = NULL, *line;
    int n = 0, i, ret, installed;
    char *abs = NULL;

    if (path[0] == '/')
        abs = strdup(path);
    else if (asprintf(&abs, "%s/%s", r->cwd, path) < 0)
        abs = NULL;
    if (!abs)
        return EXIT_OUT_OF_MEMORY;
    installed = dcc_mirror_is_installed_path(abs);
    free(abs);

    if (testing_hook("DISTCC_TESTING_MIRROR_OMIT", path))
        return 0;
    if ((ret = dcc_mirror_dir_ident(path, r, &ident,
                                    installed ? &names : NULL, &n)))
        return ret;
    if (testing_hook("DISTCC_TESTING_MIRROR_SKEW", path))
        testing_skew(&ident);
    {
        /* DISTCC_TESTING_MIRROR_HIDE="<dir>/<name>" hides one entry. */
        const char *hide = getenv("DISTCC_TESTING_MIRROR_HIDE");
        size_t pl = strlen(path);
        if (hide && strncmp(hide, path, pl) == 0 && hide[pl] == '/'
            && !strchr(hide + pl + 1, '/'))
            testing_skew(&ident);
    }
    if (ident.present != DCC_MIRROR_DIR || !installed)
        return dsta_append_ident(buf, len, cap, path, &ident);
    if (strchr(path, '\n'))
        ret = EXIT_PROTOCOL_ERROR;
    else if (asprintf(&line, "L %s %s\n", ident.digest, path) < 0)
        ret = EXIT_OUT_OF_MEMORY;
    else {
        ret = buf_append(buf, len, cap, line);
        free(line);
    }
    for (i = 0; i < n && !ret; i++) {
        const char *hide = getenv("DISTCC_TESTING_MIRROR_HIDE");
        if (strchr(names[i], '\n'))
            continue;   /* cannot match any spelling; counted in the hash */
        if (hide && *hide) {
            /* "<dir>/<name>": describe the directory without that entry. */
            size_t pl = strlen(path);
            if (strncmp(hide, path, pl) == 0 && hide[pl] == '/'
                && strcmp(hide + pl + 1, names[i] + 1) == 0)
                continue;
        }
        if (asprintf(&line, "N %s\n", names[i]) < 0)
            ret = EXIT_OUT_OF_MEMORY;
        else {
            ret = buf_append(buf, len, cap, line);
            free(line);
        }
    }
    dcc_mirror_free_names(names, n);
    return ret;
}


/* Refuse with @p code, logging and sending @p fmt as the reason. */
#define REFUSE(c, ...) do { code = (c); \
        snprintf(why, sizeof why, __VA_ARGS__); goto refuse; } while (0)

/* Remove the job directory's contents, recursing into anything the
 * compiler created (crash reports, for example). */
static void remove_tree_contents(const char *dir)
{
    DIR *d;
    struct dirent *de;
    char *p;

    if (!dir || !(d = opendir(dir)))
        return;
    while ((de = readdir(d)) != NULL) {
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (asprintf(&p, "%s/%s", dir, de->d_name) < 0)
            continue;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree_contents(p);
            rmdir(p);
        } else {
            unlink(p);
        }
        free(p);
    }
    closedir(d);
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
    char *cwd = NULL, **argv = NULL, **scanned = NULL, **client_argv = NULL;
    char **env = NULL, **rule_strs = NULL, **saved_env = NULL;
    char *cver = NULL, *input_tmp, *output_tmp;
    char *job_dir = NULL, *obj_fname = NULL, *dotd_fname = NULL;
    char real[MAXPATHLEN + 1], why[MAXPATHLEN + 128];
    char my_cver[DCC_SHA256_HEX_LEN + 1];
    struct dcc_mirror_checklist checks = { NULL, 0, 0 };
    struct dcc_mirror_rules rules;
    struct dcc_strset file_set = { NULL, 0, 0 }, dir_set = { NULL, 0, 0 };
    struct dcc_strset comps = { NULL, 0, 0 };
    unsigned n_checks, i;
    int code = DCC_MIRR_OK, ret = 0, status = 0, confine_failed = 0;
    int changed_dir = 0;
    char *dotd_text = NULL, *dsta = NULL, **dotd_paths = NULL, **files = NULL;
    size_t dsta_len = 0, dsta_cap = 0, k;
    int n_dotd = 0;
    pid_t pid;
    char *saved_tmpdir = NULL;

    memset(&rules, 0, sizeof rules);
    why[0] = '\0';
    *status_ret = 0;
    *job_result = STATS_OTHER;

    /* Read the whole request before answering anything, so that an early
     * refusal never leaves unread data behind (which would reset the
     * connection). */
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
        ret = dcc_mirror_checklist_add(&checks, path);
        free(path);
        if (ret)
            goto out;
        checks.items[checks.n - 1].ident.present = (int) kind;
        checks.items[checks.n - 1].ident.size = (off_t) size;
        checks.items[checks.n - 1].ident.mtime = (long long) mtime;
    }
    if ((ret = dcc_r_token_string(in_fd, "CVER", &cver))
        || (ret = dcc_r_argv(in_fd, "NENV", "ENVS", &env))
        || (ret = dcc_r_argv(in_fd, "NRUL", "RULE", &rule_strs)))
        goto out;
    dcc_trace_argv("mirror request", argv);

    if (!(rules.cwd = strdup(cwd))) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }
    for (i = 0; rule_strs[i]; i++)
        if ((ret = dcc_mirror_rules_add(&rules, rule_strs[i])))
            goto out;
    /* Classify installed trees as the client does, so that both sides
     * compare the same way; without a list, use --mirror-installed. */
    if (rules.n_installed) {
        size_t len = 1;
        char *list;
        for (i = 0; i < (unsigned) rules.n_installed; i++)
            len += strlen(rules.installed[i]) + 1;
        if (!(list = calloc(len, 1))) {
            ret = EXIT_OUT_OF_MEMORY;
            goto out;
        }
        for (i = 0; i < (unsigned) rules.n_installed; i++) {
            if (i)
                strcat(list, ":");
            strcat(list, rules.installed[i]);
        }
        ret = dcc_mirror_set_installed_prefixes(list);
        free(list);
    } else {
        ret = dcc_mirror_set_installed_prefixes(arg_mirror_installed);
    }
    if (ret)
        goto out;
    if ((ret = dcc_copy_argv(argv, &client_argv, 0)))
        goto out;

    if (dcc_scan_args(argv, &input_tmp, &output_tmp, &scanned) != 0)
        REFUSE(DCC_MIRR_ARG_POLICY, "command is not a single compile");
    dcc_free_argv(argv);
    argv = scanned;
    scanned = NULL;
    if (!(*input_ret = strdup(input_tmp))) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }

    if (opt_n_mirror_roots == 0)
        REFUSE(DCC_MIRR_DISABLED, "this daemon has no --mirror-root");
    if (cwd[0] != '/' || !realpath(cwd, real))
        REFUSE(DCC_MIRR_MISSING, "cwd %s: %s", cwd, strerror(errno));
    if (!under_root(real))
        REFUSE(DCC_MIRR_DISABLED, "cwd %s is not under a --mirror-root", real);
    if (chdir(cwd) == -1)
        REFUSE(DCC_MIRR_MISSING, "chdir %s: %s", cwd, strerror(errno));
    changed_dir = 1;
    if (!realpath(*input_ret, real))
        REFUSE(DCC_MIRR_MISSING, "input %s: %s", *input_ret, strerror(errno));
    if (!under_root(real))
        REFUSE(DCC_MIRR_DISABLED, "input %s is not under a --mirror-root",
               real);

    if (mirror_check_args(argv) != 0
        || dcc_check_compiler_and_args(argv) != 0)
        REFUSE(DCC_MIRR_ARG_POLICY, "an argument or the compiler was refused "
               "(see the helper's log)");

    if ((ret = apply_client_env(env, &saved_env)))
        goto out;
    if (dcc_mirror_compiler_ident(argv[0], my_cver) != 0
        || strcmp(my_cver, cver) != 0
        || getenv("DISTCC_TESTING_MIRROR_CVER"))
        REFUSE(DCC_MIRR_COMPILER, "compiler %s differs from the client's",
               argv[0]);

    /* Pre-check: synced-tree files must have the client's size and mtime.
     * Installed-tree files are compared by digest after the compile. */
    for (i = 0; i < (unsigned) checks.n; i++) {
        struct dcc_mirror_ident here;
        const struct dcc_mirror_check *c = &checks.items[i];
        if (dcc_mirror_is_installed_path(c->path))
            continue;
        if ((ret = dcc_mirror_ident_of(c->path, 0, &here)))
            goto out;
        if (!dcc_mirror_ident_equal(&c->ident, &here, 0))
            REFUSE(DCC_MIRR_STALE, "%s differs from the client's copy",
                   c->path);
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
    if ((ret = mirror_tweak_args(&argv, obj_fname, dotd_fname)))
        goto out;

    if (getenv("TMPDIR"))
        saved_tmpdir = strdup(getenv("TMPDIR"));
    setenv("TMPDIR", job_dir, 1);
    if (getenv("DISTCC_TESTING_MIRROR_NO_CONFINE")) {
        confine_failed = 1;
        ret = EXIT_DISTCC_FAILED;
    } else {
        ret = dcc_spawn_confined(argv, &pid, "/dev/null", out_fname,
                                 err_fname, job_dir, &confine_failed);
    }
    if (saved_tmpdir)
        setenv("TMPDIR", saved_tmpdir, 1);
    else
        unsetenv("TMPDIR");
    if (confine_failed) {
        ret = 0;
        REFUSE(DCC_MIRR_NO_CONFINE, "the compiler could not be confined");
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

    /* Describe what the compile read, while still in its cwd: every file
     * of the .d and the check list, and every directory that took part in
     * the include search. */
    if (!WIFSIGNALED(status) && WEXITSTATUS(status) == 0) {
        int n_files;
        if ((ret = dcc_load_file_string(dotd_fname, &dotd_text))
            || (ret = dcc_mirror_parse_dotd(dotd_text, strlen(dotd_text),
                                            &dotd_paths, &n_dotd)))
            goto out;
        n_files = n_dotd + checks.n;
        if (!(files = calloc((size_t) n_files + 1, sizeof *files))) {
            ret = EXIT_OUT_OF_MEMORY;
            goto out;
        }
        for (i = 0; i < (unsigned) n_dotd; i++)
            files[i] = dotd_paths[i];
        for (i = 0; i < (unsigned) checks.n; i++)
            files[n_dotd + i] = checks.items[i].path;
        if ((ret = dcc_mirror_required(client_argv, files, n_files,
                                       &file_set, &dir_set, &comps)))
            goto out;
        for (k = 0; k < file_set.cap; k++)
            if (file_set.items[k]
                && (ret = dsta_append(&dsta, &dsta_len, &dsta_cap,
                                      file_set.items[k])))
                goto out;
        for (k = 0; k < dir_set.cap; k++)
            if (dir_set.items[k]
                && (ret = dsta_append_dir(&dsta, &dsta_len, &dsta_cap,
                                          dir_set.items[k], &rules)))
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
        || (ret = send_blob(out_fd, "DSTA", dsta ? dsta : "", dsta_len)))
        goto out;
    *job_result = STATS_COMPILE_OK;
    goto out;

  refuse:
    rs_log_warning("mirror: refusing (MIRR %d): %s", code, why);
    ret = send_refusal(out_fd, code, why);
    *job_result = STATS_OTHER;

  out:
    if (changed_dir && chdir(dcc_daemon_wd) == -1)
        rs_log_warning("chdir(%s) failed: %s", dcc_daemon_wd, strerror(errno));
    restore_env(saved_env);
    free(saved_tmpdir);
    free(cwd);
    free(cver);
    if (env)
        dcc_free_argv(env);
    if (rule_strs)
        dcc_free_argv(rule_strs);
    if (client_argv)
        dcc_free_argv(client_argv);
    if (scanned)
        dcc_free_argv(scanned);
    *argv_ret = argv;
    dcc_mirror_checklist_free(&checks);
    dcc_mirror_rules_free(&rules);
    dcc_strset_free(&file_set);
    dcc_strset_free(&dir_set);
    dcc_strset_free(&comps);
    remove_tree_contents(job_dir);
    free(job_dir);
    free(obj_fname);
    free(dotd_fname);
    free(dotd_text);
    free(dsta);
    free(files);
    dcc_mirror_free_paths(dotd_paths, n_dotd);
    dcc_mirror_digest_flush();
    return ret;
}

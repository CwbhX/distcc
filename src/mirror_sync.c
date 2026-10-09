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

/* distcc --mirror-sync: copy the working tree to the mirror hosts.
 *
 * No version control is involved: the files are copied as they are on
 * disk, uncommitted edits included, with rsync over ssh.  Run it after
 * building the PCHs and generated headers and before the build:
 *
 *   ninja <PCH targets>; distcc --mirror-sync; ninja
 *
 * What is copied, to the same paths on each host:
 *
 *  - each DISTCC_MIRROR_ROOTS root, whole, except the names in
 *    DISTCC_MIRROR_EXCLUDE at its top and any build tree inside it;
 *  - each build tree (the logical sides of DISTCC_MIRROR_PATHMAP): every
 *    directory, and only sources, headers and PCHs;
 *  - each DISTCC_MIRROR_EXTRA tree, whole.
 *
 * Deleted files are deleted on the host too.  The selection is the one
 * directory identities are computed with (see mirror_ident.c), so after a
 * sync the directories compare equal.
 *
 * Hosts are the ssh destinations given as arguments, else
 * DISTCC_MIRROR_SSH (space-separated), else the address of every
 * ",mirror" host in the host list.  DISTCC_MIRROR_RSH replaces the ssh
 * command used for both directory creation and rsync. */

#include <config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "hosts.h"
#include "util.h"
#include "mirror.h"
#include "mirror_sync.h"

/* Mirrors build_suffixes in mirror_ident.c. */
static const char *const build_patterns[] = {
    "*.h", "*.hh", "*.hpp", "*.hxx", "*.h++", "*.inc", "*.inl", "*.ipp",
    "*.tpp", "*.tcc", "*.def", "*.c", "*.cc", "*.cpp", "*.cxx", "*.c++",
    "*.m", "*.mm", "*.pch", "*.gch", "cmake_pch.*", NULL
};

struct argv_buf {
    char **v;
    int n, cap;
};

static int push(struct argv_buf *a, const char *s)
{
    if (a->n + 2 > a->cap) {
        int cap = a->cap ? a->cap * 2 : 32;
        char **n = realloc(a->v, cap * sizeof *n);
        if (!n)
            return EXIT_OUT_OF_MEMORY;
        a->v = n;
        a->cap = cap;
    }
    if (!(a->v[a->n] = strdup(s)))
        return EXIT_OUT_OF_MEMORY;
    a->v[++a->n] = NULL;
    return 0;
}

static int pushf(struct argv_buf *a, const char *fmt, const char *s)
{
    char *t;
    int ret;
    if (asprintf(&t, fmt, s) < 0)
        return EXIT_OUT_OF_MEMORY;
    ret = push(a, t);
    free(t);
    return ret;
}

static void argv_free(struct argv_buf *a)
{
    int i;
    for (i = 0; i < a->n; i++)
        free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

static int run(char **argv)
{
    pid_t pid;
    int status;
    char *s = dcc_argv_tostr(argv);

    fprintf(stderr, "distcc --mirror-sync: %s\n", s);
    free(s);
    if ((pid = fork()) == -1)
        return EXIT_OUT_OF_MEMORY;
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "distcc: exec %s: %s\n", argv[0], strerror(errno));
        _exit(EXIT_COMPILER_MISSING);
    }
    while (waitpid(pid, &status, 0) == -1)
        if (errno != EINTR)
            return EXIT_DISTCC_FAILED;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "distcc --mirror-sync: %s failed (status %d)\n",
                argv[0], status);
        return EXIT_DISTCC_FAILED;
    }
    return 0;
}

static int rsync_base(struct argv_buf *a)
{
    const char *rsh = getenv("DISTCC_MIRROR_RSH");
    int ret;

    /* -a keeps the mtimes the post-check compares; -W because deltas of
     * PCHs cost more than sending them over a fast link. */
    if ((ret = push(a, "rsync")) || (ret = push(a, "-a"))
        || (ret = push(a, "-W")) || (ret = push(a, "--delete")))
        return ret;
    if (rsh && *rsh && ((ret = push(a, "-e")) || (ret = push(a, rsh))))
        return ret;
    return 0;
}

/* Tokenize like rsync's -e option: quotes group whitespace, and a
 * doubled quote inside a quoted argument represents one literal quote.
 * Do not invoke a local shell or expand shell metacharacters. */
static int remote_shell(struct argv_buf *a)
{
    const char *rsh = getenv("DISTCC_MIRROR_RSH"), *p;
    char *word;
    int ret = 0;

    if (!rsh || !*rsh)
        return push(a, "ssh");
    if (!(word = malloc(strlen(rsh) + 1)))
        return EXIT_OUT_OF_MEMORY;
    p = rsh;
    while (*p && !ret) {
        char quote = 0, *out = word;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            if (*p == quote) {
                if (p[1] == quote) {
                    *out++ = *p;
                    p += 2;
                } else {
                    quote = 0;
                    p++;
                }
            } else if (!quote && (*p == '\'' || *p == '"')) {
                quote = *p++;
            } else {
                *out++ = *p++;
            }
        }
        if (quote) {
            fprintf(stderr, "distcc --mirror-sync: unmatched quote in "
                    "DISTCC_MIRROR_RSH\n");
            ret = EXIT_BAD_ARGUMENTS;
            break;
        }
        *out = 0;
        ret = push(a, word);
    }
    free(word);
    if (!ret && (!a->n || !*a->v[0]))
        ret = EXIT_BAD_ARGUMENTS;
    return ret;
}

/* ssh joins remote command arguments with spaces.  Quote each path for
 * the remote shell, including any embedded single quotes. */
static int push_remote_path(struct argv_buf *a, const char *path)
{
    char *quoted, *out;
    const char *p;
    int ret;
    if (!(quoted = malloc(strlen(path) * 4 + 3)))
        return EXIT_OUT_OF_MEMORY;
    out = quoted;
    *out++ = '\'';
    for (p = path; *p; p++) {
        if (*p == '\'') {
            memcpy(out, "'\\''", 4);
            out += 4;
        } else {
            *out++ = *p;
        }
    }
    *out++ = '\'';
    *out = 0;
    ret = push(a, quoted);
    free(quoted);
    return ret;
}

static int is_under(const char *path, const char *root)
{
    size_t len = strlen(root);
    return strncmp(path, root, len) == 0 && path[len] == '/';
}

static int sync_one(const char *dest, const struct dcc_mirror_rules *r,
                    char **extra, int n_extra)
{
    struct argv_buf a = { NULL, 0, 0 };
    int i, j, ret = 0;

    /* Create the parent directories first: rsync makes only the last. */
    if ((ret = remote_shell(&a)) || (ret = push(&a, dest))
        || (ret = push(&a, "mkdir")) || (ret = push(&a, "-p")))
        goto out;
    for (i = 0; i < r->n_roots && !ret; i++)
        ret = push_remote_path(&a, r->roots[i]);
    for (i = 0; i < r->n_build_roots && !ret; i++)
        ret = push_remote_path(&a, r->build_roots[i]);
    for (i = 0; i < n_extra && !ret; i++)
        ret = push_remote_path(&a, extra[i]);
    if (ret || (ret = run(a.v)))
        goto out;
    argv_free(&a);

    /* Source roots, whole, minus top-level excludes and build trees. */
    for (i = 0; i < r->n_roots && !ret; i++) {
        if ((ret = rsync_base(&a)))
            break;
        for (j = 0; j < r->n_excludes && !ret; j++)
            ret = pushf(&a, "--exclude=/%s", r->excludes[j]);
        for (j = 0; j < r->n_build_roots && !ret; j++) {
            if (is_under(r->build_roots[j], r->roots[i])) {
                const char *rel = r->build_roots[j] + strlen(r->roots[i]);
                ret = pushf(&a, "--exclude=%s", rel);
            }
        }
        if (!ret && !(ret = pushf(&a, "%s/", r->roots[i]))) {
            char *d;
            if (asprintf(&d, "%s:%s/", dest, r->roots[i]) < 0)
                ret = EXIT_OUT_OF_MEMORY;
            else {
                ret = push(&a, d);
                free(d);
            }
        }
        if (!ret)
            ret = run(a.v);
        argv_free(&a);
    }

    /* Build trees: every directory, and only build files. */
    for (i = 0; i < r->n_build_roots && !ret; i++) {
        if ((ret = rsync_base(&a)) || (ret = push(&a, "--include=*/")))
            break;
        for (j = 0; build_patterns[j] && !ret; j++)
            ret = pushf(&a, "--include=%s", build_patterns[j]);
        if (!ret && !(ret = push(&a, "--exclude=*"))
            && !(ret = pushf(&a, "%s/", r->build_roots[i]))) {
            char *d;
            if (asprintf(&d, "%s:%s/", dest, r->build_roots[i]) < 0)
                ret = EXIT_OUT_OF_MEMORY;
            else {
                ret = push(&a, d);
                free(d);
            }
        }
        if (!ret)
            ret = run(a.v);
        argv_free(&a);
    }

    /* Extra trees, whole. */
    for (i = 0; i < n_extra && !ret; i++) {
        char *d;
        if ((ret = rsync_base(&a)) || (ret = pushf(&a, "%s/", extra[i])))
            break;
        if (asprintf(&d, "%s:%s/", dest, extra[i]) < 0)
            ret = EXIT_OUT_OF_MEMORY;
        else {
            ret = push(&a, d);
            free(d);
        }
        if (!ret)
            ret = run(a.v);
        argv_free(&a);
    }

  out:
    argv_free(&a);
    return ret;
}


int dcc_mirror_sync(int argc, char **argv)
{
    struct dcc_mirror_rules rules;
    struct dcc_hostdef *hosts = NULL, *h;
    char **dests = NULL, **extra = NULL;
    int n_dests = 0, n_extra = 0, n_hosts, i, ret = 0;
    char cwd[MAXPATHLEN + 1];
    struct timeval start, end;
    const char *extra_env = getenv("DISTCC_MIRROR_EXTRA");

    gettimeofday(&start, NULL);
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    if ((ret = dcc_mirror_rules_from_env(&rules, cwd)))
        return ret;
    if (rules.n_roots == 0 && rules.n_build_roots == 0
        && !(extra_env && *extra_env)) {
        fprintf(stderr, "distcc --mirror-sync: set DISTCC_MIRROR_ROOTS, "
                "DISTCC_MIRROR_PATHMAP or DISTCC_MIRROR_EXTRA\n");
        ret = EXIT_BAD_ARGUMENTS;
        goto out;
    }

    if (extra_env && *extra_env) {
        char *copy = strdup(extra_env), *p, *tok;
        for (p = copy; p && (tok = strsep(&p, ":")) != NULL; ) {
            char **n;
            size_t len = strlen(tok);
            while (len > 1 && tok[len - 1] == '/')
                tok[--len] = '\0';
            if (!len || tok[0] != '/')
                continue;
            if (!(n = realloc(extra, (n_extra + 1) * sizeof *n))
                || !(n[n_extra] = strdup(tok))) {
                free(copy);
                ret = EXIT_OUT_OF_MEMORY;
                goto out;
            }
            extra = n;
            n_extra++;
        }
        free(copy);
    }

    for (i = 0; i < argc; i++) {
        char **n = realloc(dests, (n_dests + 1) * sizeof *n);
        if (!n) {
            ret = EXIT_OUT_OF_MEMORY;
            goto out;
        }
        dests = n;
        dests[n_dests++] = strdup(argv[i]);
    }
    if (!n_dests && getenv("DISTCC_MIRROR_SSH")) {
        char *copy = strdup(getenv("DISTCC_MIRROR_SSH")), *p, *tok;
        for (p = copy; p && (tok = strsep(&p, " \t")) != NULL; ) {
            char **n;
            if (!*tok)
                continue;
            if (!(n = realloc(dests, (n_dests + 1) * sizeof *n))) {
                free(copy);
                ret = EXIT_OUT_OF_MEMORY;
                goto out;
            }
            dests = n;
            dests[n_dests++] = strdup(tok);
        }
        free(copy);
    }
    if (!n_dests && dcc_get_hostlist(&hosts, &n_hosts) == 0) {
        for (h = hosts; h; h = h->next) {
            char **n;
            if (h->cpp_where != DCC_CPP_MIRROR || !h->hostname)
                continue;
            if (!(n = realloc(dests, (n_dests + 1) * sizeof *n))) {
                ret = EXIT_OUT_OF_MEMORY;
                goto out;
            }
            dests = n;
            dests[n_dests++] = strdup(h->hostname);
        }
    }
    if (!n_dests) {
        fprintf(stderr, "distcc --mirror-sync: no destination: give ssh "
                "hosts as arguments, set DISTCC_MIRROR_SSH, or add a "
                "',mirror' host\n");
        ret = EXIT_BAD_ARGUMENTS;
        goto out;
    }

    for (i = 0; i < n_dests && !ret; i++)
        ret = sync_one(dests[i], &rules, extra, n_extra);

    gettimeofday(&end, NULL);
    if (!ret)
        fprintf(stderr, "distcc --mirror-sync: done in %.1fs\n",
                (double) (end.tv_sec - start.tv_sec)
                + (end.tv_usec - start.tv_usec) / 1e6);

  out:
    for (i = 0; i < n_dests; i++)
        free(dests[i]);
    free(dests);
    for (i = 0; i < n_extra; i++)
        free(extra[i]);
    free(extra);
    dcc_mirror_rules_free(&rules);
    return ret;
}

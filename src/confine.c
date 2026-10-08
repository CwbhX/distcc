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

/* Write confinement for mirror-mode compiles.
 *
 * A mirrored job runs the compiler in the helper's copy of the client's
 * tree.  The compiler and everything it starts may write only inside a
 * fresh per-job directory (plus /dev/null); reads are not restricted.  On
 * macOS this is a Seatbelt profile applied with sandbox_init() in the
 * forked child, just before exec.  Every other descriptor (the client's
 * socket included) is closed first.  If the profile cannot be applied, the
 * child reports it through a close-on-exec pipe and exits without running
 * anything, and the job is answered with MIRR 5.
 *
 * Platforms without such a mechanism have no mirror mode:
 * dcc_confine_available() returns false and the daemon refuses to start
 * with --mirror-root. */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "exec.h"
#include "util.h"
#include "confine.h"

#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  include <sandbox.h>
#  define HAVE_SEATBELT 1
#endif

/* Byte the child writes to the status pipe when confinement failed. */
#define CONFINE_FAILED 'S'

int dcc_confine_available(void)
{
#ifdef HAVE_SEATBELT
    return 1;
#else
    return 0;
#endif
}

#ifdef HAVE_SEATBELT
/**
 * The Seatbelt profile for a job whose writable directory is @p job_dir,
 * which must already be a resolved (realpath) path.
 **/
static int make_profile(const char *job_dir, char **profile_ret)
{
    const char *p;

    for (p = job_dir; *p; p++) {
        if (*p == '"' || *p == '\\' || *p == '\n') {
            rs_log_error("refusing job directory with special characters: %s",
                         job_dir);
            return EXIT_DISTCC_FAILED;
        }
    }
    if (asprintf(profile_ret,
                 "(version 1)\n"
                 "(allow default)\n"
                 "(deny file-write*)\n"
                 "(allow file-write* (subpath \"%s\"))\n"
                 "(allow file-write-data (literal \"/dev/null\")"
                 " (literal \"/dev/dtracehelper\"))\n"
                 "(deny network*)\n",
                 job_dir) < 0)
        return EXIT_OUT_OF_MEMORY;
    return 0;
}

static int apply_profile(const char *profile)
{
    char *err = NULL;

    if (sandbox_init(profile, 0, &err) != 0) {
        if (err) {
            rs_log_error("sandbox_init failed: %s", err);
            sandbox_free_error(err);
        }
        return -1;
    }
    return 0;
}
#endif

#ifdef HAVE_SEATBELT
/* Close every descriptor from 3 up except @p keep. */
static void close_other_fds(int keep)
{
    int fd, max = (int) sysconf(_SC_OPEN_MAX);

    if (max < 0 || max > 65536)
        max = 65536;
    for (fd = 3; fd < max; fd++)
        if (fd != keep)
            close(fd);
}
#endif

/**
 * Run @p argv in a child that may write only inside @p job_dir.
 *
 * Like dcc_spawn_child(): stdin, stdout and stderr are redirected, and the
 * child gets its own process group.  On return *confine_failed is 1 if the
 * child could not be confined; then nothing was run and the child has
 * already been reaped.
 **/
int dcc_spawn_confined(char **argv, pid_t *pidptr,
                       const char *stdin_file, const char *stdout_file,
                       const char *stderr_file, const char *job_dir,
                       int *confine_failed)
{
#ifdef HAVE_SEATBELT
    char *profile = NULL;
    int pipefd[2];
    pid_t pid;
    char c;
    ssize_t n;
    int ret;

    *confine_failed = 0;
    if ((ret = make_profile(job_dir, &profile))) {
        *confine_failed = 1;
        return ret;
    }
    if (pipe(pipefd) == -1) {
        free(profile);
        *confine_failed = 1;
        return EXIT_OUT_OF_MEMORY;
    }
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    dcc_trace_argv("forking confined child", argv);
    pid = fork();
    if (pid == -1) {
        rs_log_error("failed to fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        free(profile);
        return EXIT_OUT_OF_MEMORY;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dcc_ignore_sigpipe(0);
        dcc_increment_safeguard();
        if (dcc_new_pgrp() != 0)
            rs_trace("Unable to start a new group");
        if (dcc_redirect_fds(stdin_file, stdout_file, stderr_file) != 0)
            _exit(EXIT_DISTCC_FAILED);
        close_other_fds(pipefd[1]);
        if (apply_profile(profile) != 0) {
            c = CONFINE_FAILED;
            (void) write(pipefd[1], &c, 1);
            _exit(EXIT_DISTCC_FAILED);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "distccd: failed to exec %s: %s\n", argv[0],
                strerror(errno));
        _exit(EXIT_COMPILER_MISSING);
    }

    free(profile);
    close(pipefd[1]);
    do {
        n = read(pipefd[0], &c, 1);
    } while (n == -1 && errno == EINTR);
    close(pipefd[0]);
    if (n == 1 && c == CONFINE_FAILED) {
        int status;
        waitpid(pid, &status, 0);
        *confine_failed = 1;
        return EXIT_DISTCC_FAILED;
    }
    *pidptr = pid;
    return 0;
#else
    (void) argv; (void) pidptr; (void) stdin_file; (void) stdout_file;
    (void) stderr_file; (void) job_dir;
    *confine_failed = 1;
    return EXIT_DISTCC_FAILED;
#endif
}

/**
 * Check that confinement works: a confined child must be able to create a
 * file inside its job directory and must fail to create one next to it.
 * Returns 0 if so.
 **/
int dcc_confine_probe(void)
{
#ifdef HAVE_SEATBELT
    char *top = NULL, *job = NULL, *inside = NULL, *outside = NULL;
    char *profile = NULL;
    char real[MAXPATHLEN + 1];
    pid_t pid;
    int status, ret = EXIT_DISTCC_FAILED;

    if (dcc_get_new_tmpdir(&top) != 0)
        return EXIT_DISTCC_FAILED;
    if (asprintf(&job, "%s/job", top) < 0 || mkdir(job, 0700) == -1
        || !realpath(job, real)
        || asprintf(&inside, "%s/inside", real) < 0
        || asprintf(&outside, "%s/../outside", real) < 0
        || make_profile(real, &profile) != 0)
        goto out;

    pid = fork();
    if (pid == -1)
        goto out;
    if (pid == 0) {
        int fd;
        close_other_fds(-1);
        if (apply_profile(profile) != 0)
            _exit(2);
        if ((fd = open(inside, O_WRONLY | O_CREAT | O_EXCL, 0600)) == -1)
            _exit(3);
        close(fd);
        if ((fd = open(outside, O_WRONLY | O_CREAT | O_EXCL, 0600)) != -1)
            _exit(4);
        _exit(errno == EPERM || errno == EACCES ? 0 : 5);
    }
    if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)
        && WEXITSTATUS(status) == 0 && access(outside, F_OK) == -1
        && !getenv("DISTCC_TESTING_CONFINE_PROBE_FAIL"))
        ret = 0;
    else
        rs_log_error("write confinement probe failed (status %d)", status);

  out:
    if (inside) unlink(inside);
    if (outside) unlink(outside);
    if (job) rmdir(job);
    if (top) rmdir(top);
    free(top); free(job); free(inside); free(outside); free(profile);
    return ret;
#else
    rs_log_error("no write confinement mechanism on this platform");
    return EXIT_DISTCC_FAILED;
#endif
}

#ifdef HAVE_SEATBELT
#  pragma clang diagnostic pop
#endif

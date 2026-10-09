/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 * Copyright 2007 Google Inc.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


                /* Power is nothing without control
                 *      -- Pirelli tyre advertisement. */


/**
 * @file
 *
 * @brief Manage lockfiles.
 *
 * distcc uses a simple disk-based lockfile system to keep track of how many
 * jobs are queued on various machines.  These locks might be used for
 * something else in the future.
 *
 * We use locks rather than e.g. a database or a central daemon because we
 * want to make sure that the lock will be removed if the client terminates
 * unexpectedly.
 *
 * The files themselves (as opposed to the lock on them) are never cleaned up;
 * since locking & creation is nonatomic I can't think of a clean way to do
 * it.  There shouldn't be many of them, and dead ones will be caught by the
 * tmpreaper.  In any case they're zero bytes.
 *
 * Sys V semaphores might work well here, but the interface is a bit ugly and
 * they are probably not portable to Cygwin.  In particular they can leak if
 * the process is abruptly terminated, which is likely to happen to distcc.
 */


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/file.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "hosts.h"
#include "lock.h"
#include "exitcode.h"
#include "snprintf.h"

/* Local lock namespaces.
 *
 * There are two independent pools of local slots, each with its own set of
 * lock files:
 *
 *  - Local compile slots: "localhost/N" host entries and dcc_hostdef_local
 *    ("--localslots").  These lock <lockdir>/cpu_localhost_<n>.  The two
 *    deliberately share that one set of files, so a job that must run
 *    locally and a job that picked localhost from the host list are
 *    counted against the same limit.
 *
 *  - Local preprocessor slots: dcc_hostdef_local_cpp ("--localslots_cpp").
 *    These lock <lockdir>/cpp_localhost_<n>.  They used to share the
 *    cpu_localhost_<n> files with the compile slots, which meant a job that
 *    already held a remote slot could not start preprocessing while every
 *    local compile slot was busy, and the remote slot sat idle until a local
 *    compile finished.  Now --localslots_cpp bounds the number of
 *    concurrent local preprocessors on its own, independent of local
 *    compiles.
 *
 * The cpp pool is recognised by pointer identity (host ==
 * dcc_hostdef_local_cpp) in dcc_make_lock_filename(), so callers keep
 * passing the lockname "cpu" for every slot.  Remote lock names are
 * unchanged: <lockname>_tcp_<host>_<port>_<n> and <lockname>_ssh_<host>_<n>.
 *
 * Extreme care with lock ordering is required in order to avoid
 * deadlocks.  In particular, the following invariants apply:
 *
 *  - Each distcc process should hold no more than two locks at a time;
 *    one local lock (either a compile slot or a preprocessor slot, never
 *    both), and one remote lock.
 *
 *  - When acquiring more than one lock, a strict lock ordering discipline
 *    must be observed: the remote lock must be acquired first, before the
 *    local lock; and conversely the local lock must be released first,
 *    before the remote lock.
 *
 *  - A process never waits for a local lock while holding another local
 *    lock.  Separate namespaces do not change this: the only process that
 *    waits for a cpp slot while holding something is one that holds a
 *    remote slot, and cpp slot holders never wait on remote slots.
 */

struct dcc_hostdef _dcc_local = {
    DCC_MODE_LOCAL,
    NULL,
    (char *) "localhost",
    0,
    NULL,
    1,                          /* host is_up */
    4,                          /* number of tasks */
    (char *)"localhost",        /* verbatim string */
    DCC_VER_1,                  /* protocol (ignored) */
    DCC_COMPRESS_NONE,          /* compression (ignored) */
    DCC_CPP_ON_CLIENT,          /* where to cpp (ignored) */
#ifdef HAVE_GSSAPI
    0,                          /* Authentication? */
    NULL,                       /* Authentication name */
#endif
    NULL
};

struct dcc_hostdef *dcc_hostdef_local = &_dcc_local;

struct dcc_hostdef _dcc_local_cpp = {
    DCC_MODE_LOCAL,
    NULL,
    (char *) "localhost",
    0,
    NULL,
    1,                          /* host is_up */
    8,                          /* number of tasks */
    (char *)"localhost",        /* verbatim string */
    DCC_VER_1,                  /* protocol (ignored) */
    DCC_COMPRESS_NONE,          /* compression (ignored) */
    DCC_CPP_ON_CLIENT,          /* where to cpp (ignored) */
#ifdef HAVE_GSSAPI
    0,                          /* Authentication? */
    NULL,                       /* Authentication name */
#endif
    NULL
};

struct dcc_hostdef *dcc_hostdef_local_cpp = &_dcc_local_cpp;



/**
 * Build the name of the lock file for slot @p iter of @p host.
 *
 * Local compile slots use <lockdir>/<lockname>_localhost_<n>; local
 * preprocessor slots (dcc_hostdef_local_cpp) use <lockdir>/cpp_localhost_<n>
 * when @p lockname is "cpu".
 *
 * Returns a newly allocated buffer.
 **/
int dcc_make_lock_filename(const char *lockname,
                           const struct dcc_hostdef *host,
                           int iter,
                           char **filename_ret)
{
    char * buf;
    int ret;
    char *lockdir;

    if ((ret = dcc_get_lock_dir(&lockdir)))
        return ret;

    if (host->mode == DCC_MODE_LOCAL) {
        const char *prefix = lockname;
        const char *suffix = "";

        if (host == dcc_hostdef_local_cpp) {
            /* Preprocessor slots have their own namespace; see the comment
             * at the top of this file.  "cpu" becomes "cpp"; any other
             * lockname gets a "_cpp" suffix so it cannot collide either. */
            if (strcmp(lockname, "cpu") == 0)
                prefix = "cpp";
            else
                suffix = "_cpp";
        }
        if (asprintf(&buf, "%s/%s%s_localhost_%d", lockdir, prefix, suffix,
                     iter) == -1)
            return EXIT_OUT_OF_MEMORY;
    } else if (host->mode == DCC_MODE_TCP) {
        if (asprintf(&buf, "%s/%s_tcp_%s_%d_%d", lockdir, lockname,
                     host->hostname,
                     host->port, iter) == -1)
            return EXIT_OUT_OF_MEMORY;
    } else if (host->mode == DCC_MODE_SSH) {
        if (asprintf(&buf, "%s/%s_ssh_%s_%d", lockdir, lockname,
                     host->hostname, iter) == -1)
            return EXIT_OUT_OF_MEMORY;
    } else {
        rs_log_crit("oops");
        return EXIT_PROTOCOL_ERROR;
    }

    *filename_ret = buf;
    return 0;
}


/**
 * Get an exclusive, non-blocking lock on a file using whatever method
 * is available on this system.
 *
 * @retval 0 if we got the lock
 * @retval -1 with errno set if the file is already locked.
 **/
static int sys_lock(int fd, int block)
{
#if defined(F_SETLK)
    struct flock lockparam;

    lockparam.l_type = F_WRLCK;
    lockparam.l_whence = SEEK_SET;
    lockparam.l_start = 0;
    lockparam.l_len = 0;        /* whole file */

    return fcntl(fd, block ? F_SETLKW : F_SETLK, &lockparam);
#elif defined(HAVE_FLOCK)
    return flock(fd, LOCK_EX | (block ? 0 : LOCK_NB));
#elif defined(HAVE_LOCKF)
    return lockf(fd, block ? F_LOCK : F_TLOCK, 0);
#else
#  error "No supported lock method.  Please port this code."
#endif
}



int dcc_unlock(int lock_fd)
{
#if defined(F_SETLK)
    struct flock lockparam;

    lockparam.l_type = F_UNLCK;
    lockparam.l_whence = SEEK_SET;
    lockparam.l_start = 0;
    lockparam.l_len = 0;

    if (fcntl(lock_fd, F_SETLK, &lockparam) == -1) {
        rs_log_error("fcntl(fd%d, F_SETLK, F_UNLCK) failed: %s",
                     lock_fd, strerror(errno));
        close(lock_fd);
        return EXIT_IO_ERROR;
    }
#elif defined (HAVE_FLOCK)
    /* flock() style locks are released when the fd is closed */
#elif defined (HAVE_LOCKF)
    if (lockf(lock_fd, F_ULOCK, 0) == -1) {
        rs_log_error("lockf(fd%d, F_ULOCK, 0) failed: %s",
                     lock_fd, strerror(errno));
        close(lock_fd);
        return EXIT_IO_ERROR;
    }
#endif
    rs_trace("release lock fd%d", lock_fd);
    /* All our current locks can just be closed */
    if (close(lock_fd)) {
        rs_log_error("close failed: %s", strerror(errno));
        return EXIT_IO_ERROR;
    }
    return 0;
}


/**
 * Open a lockfile, creating if it does not exist.
 **/
int dcc_open_lockfile(const char *fname, int *plockfd)
{
    /* Create if it doesn't exist.  We don't actually do anything with
     * the file except lock it.
     *
     * The file is created with the loosest permissions allowed by the user's
     * umask, to give the best chance of avoiding problems if they should
     * happen to use a shared lock dir. */
    /* FIXME: If we fail to open with EPERM or something similar, try deleting
     * the file and try again.  That might fix problems with root-owned files
     * in user home directories. */
    *plockfd = open(fname, O_WRONLY|O_CREAT, 0666);
    if (*plockfd == -1 && errno != EEXIST) {
        rs_log_error("failed to create %s: %s", fname, strerror(errno));
        return EXIT_IO_ERROR;
    }

    return 0;
}


/**
 * Lock a server slot, in either blocking or nonblocking mode.
 *
 * In blocking mode, this function will not return until either the lock has
 * been acquired, or an error occurred.  In nonblocking mode, it will instead
 * return EXIT_BUSY if some other process has this slot locked.
 *
 * @param slot 0-based index of available slots on this host.
 * @param block True for blocking mode.
 *
 * @param lock_fd On return, contains the lock file descriptor to allow
 * it to be closed.
 **/
int dcc_lock_host(const char *lockname,
                  const struct dcc_hostdef *host,
                  int slot, int block,
                  int *lock_fd)
{
    char *fname;
    int ret;

    /* if host is down, return EXIT_BUSY */
    if (!host->is_up)
    return EXIT_BUSY;

    if ((ret = dcc_make_lock_filename(lockname, host, slot, &fname)))
        return ret;

    if ((ret = dcc_open_lockfile(fname, lock_fd)) != 0) {
        free(fname);
        return ret;
    }

    if (sys_lock(*lock_fd, block) == 0) {
        rs_trace("got %s lock on %s slot %d as fd%d (%s)", lockname,
                 host->hostdef_string, slot, *lock_fd, fname);
        free(fname);
        return 0;
    } else {
        switch (errno) {
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EAGAIN:
        case EACCES: /* HP-UX and Cygwin give this for exclusion */
            rs_trace("%s is busy", fname);
            ret = EXIT_BUSY;
            break;
        default:
            if (block && errno == EINTR) {
                /* A blocking wait was cut short by a signal, normally the
                 * timeout in where.c; the lock is still held by someone. */
                rs_trace("%s: wait interrupted", fname);
                ret = EXIT_BUSY;
                break;
            }
            rs_log_error("lock %s failed: %s", fname, strerror(errno));
            ret = EXIT_IO_ERROR;
            break;
        }

        dcc_close(*lock_fd);
        free(fname);
        return ret;
    }
}

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


                /* I put the shotgun in an Adidas bag and padded it
                 * out with four pairs of tennis socks, not my style
                 * at all, but that was what I was aiming for: If they
                 * think you're crude, go technical; if they think
                 * you're technical, go crude.  I'm a very technical
                 * boy.  So I decided to get as crude as possible.
                 * These days, though, you have to be pretty technical
                 * before you can even aspire to crudeness.
                 *              -- William Gibson, "Johnny Mnemonic" */


/**
 * @file
 *
 * Routines to decide on which machine to run a distributable job.
 *
 * The current algorithm (new in 1.2 and subject to change) is as follows.
 *
 * CPU lock is held until the job is complete.
 *
 * Once the request has been transmitted, the lock is released and a second
 * job can be sent.
 *
 * Servers which wish to limit their load can defer accepting jobs, and the
 * client will block with that lock held.
 *
 * cpp is probably cheap enough that we can allow it to run unlocked.  However
 * that is not true for local compilation or linking.
 *
 * @todo Write a test harness for the host selection algorithm.  Perhaps a
 * really simple simulation of machines taking different amounts of time to
 * build stuff?
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/file.h>
#include <sys/time.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "hosts.h"
#include "lock.h"
#include "where.h"
#include "exitcode.h"


static int dcc_lock_one(struct dcc_hostdef *hostlist,
                        struct dcc_hostdef **buildhost,
                        int *cpu_lock_fd);


void dcc_read_localslots_configuration(void)
{
    struct dcc_hostdef *hostlist;
    int ret;
    int n_hosts;

    if ((ret = dcc_get_hostlist(&hostlist, &n_hosts)) == 0) {
        while (hostlist) {
            struct dcc_hostdef *l = hostlist;
            hostlist = hostlist->next;
            dcc_free_hostdef(l);
        }
    }
}


int dcc_pick_host_from_list_and_lock_it(struct dcc_hostdef **buildhost,
                            int *cpu_lock_fd)
{
    struct dcc_hostdef *hostlist;
    int ret;
    int n_hosts;

    if ((ret = dcc_get_hostlist(&hostlist, &n_hosts)) != 0) {
        return EXIT_NO_HOSTS;
    }

    if ((ret = dcc_remove_disliked(&hostlist)))
        return ret;

    if (!hostlist) {
        return EXIT_NO_HOSTS;
    }

    return dcc_lock_one(hostlist, buildhost, cpu_lock_fd);

    /* FIXME: Host list is leaked? */
}


/*
 * Waiting for a slot
 * ==================
 *
 * dcc_lock_one() first tries every slot of every host with non-blocking
 * locks.  If they are all busy, it has to wait for one to come free.
 *
 * It used to sleep for a fixed DISTCC_PAUSE_TIME_MSEC (default 1000 ms) and
 * rescan.  With more jobs than slots that left a freed slot idle for up to the
 * whole pause: on a 25-slot pool, -j40 ran at 9.4 jobs/s against 12.1 at
 * -j25.  Shortening the pause recovers the throughput but turns the wait into
 * a busy loop (about 1000 sleeps per 150 one-second jobs at 20 ms).
 *
 * So instead the waiter blocks in the kernel on one busy slot (F_SETLKW, or
 * the flock/lockf equivalent; see sys_lock() in lock.c).  When the holder
 * releases it, the kernel hands it to a waiter straight away, with no polling
 * delay.  A process can only block on one lock file at a time, though, and a
 * slot on some other host (or another slot on the same host) may free up
 * first.  The blocking wait is therefore bounded by DISTCC_PAUSE_TIME_MSEC,
 * after which the process rescans every slot without blocking and, if it is
 * still unlucky, blocks again.
 *
 * Waiters spread themselves over the busy slots, choosing one by a hash of
 * their pid and the round number, so that concurrent waiters do not all queue
 * on slot 0 and two that collide in one round probably don't in the next.
 * A slot that somebody is blocked on is handed over the moment it is
 * released.  A slot that nobody is blocked on (likely when there are fewer
 * waiters than slots) is picked up by the next waiter whose timeout expires,
 * so it can sit idle for up to DISTCC_PAUSE_TIME_MSEC, and on average for
 * a fraction of that which shrinks as the number of waiters grows.
 *
 * The timeout is an interval timer delivering SIGALRM to a handler installed
 * without SA_RESTART, so the blocked lock call fails with EINTR.  The timer
 * repeats, so that if it fires just before the process enters the lock call
 * the next expiry still interrupts it.  SIGALRM is kept blocked except
 * during the lock call itself, so that an expiry generated around the end
 * of the wait is swallowed rather than delivered to the previous
 * disposition (usually SIG_DFL, which would kill the process) once that
 * has been restored and the timer cancelled.  SIGINT, SIGTERM and SIGHUP
 * still reach dcc_client_signalled(), which never returns, so a blocked wait
 * does not delay them.
 *
 * At no point does the wait hold more than the one lock it is trying to get,
 * so the lock ordering rules in lock.c are unaffected.  The wait for a local
 * cpp slot does happen while the caller holds a remote slot, which is the
 * permitted order (remote first, then local).
 *
 * If blocking is unavailable (no interval timer on this platform, an interval
 * timer already in use, or the blocking call failing for a reason other than
 * our timeout), that round falls back to the old timed sleep.
 *
 * We don't use exponential backoff, because that would tend to prefer later
 * arrivals and penalize jobs that have been waiting for a long time.  This
 * would mean more compiler processes hanging around than is really necessary,
 * and also by making jobs complete very-out-of-order is more likely to find
 * Makefile bugs.
 */

/** Default for DISTCC_PAUSE_TIME_MSEC.
 *
 * Long enough that a waiting process rescans at most ten times a second
 * (each rescan is one open and one non-blocking lock per slot); short enough
 * that a slot freed where nobody is blocked sits idle for at most a tenth of
 * a one-second compile, and usually much less when several jobs are
 * waiting. */
#define DCC_DEFAULT_PAUSE_TIME_MSEC 100


/**
 * Return the longest time to block on one slot (or to sleep, when blocking
 * is not available) before rescanning, from DISTCC_PAUSE_TIME_MSEC.
 *
 * 0 means do not wait at all and rescan immediately, as it always has.
 **/
static unsigned dcc_get_pause_time_ms(void)
{
    char *pt = getenv("DISTCC_PAUSE_TIME_MSEC");
    int val;

    if (!pt || !*pt)
        return DCC_DEFAULT_PAUSE_TIME_MSEC;
    val = atoi(pt);
    if (val < 0)
        return DCC_DEFAULT_PAUSE_TIME_MSEC;
    return (unsigned) val;
}


/**
 * Old-style wait: sleep for the pause time and let the caller rescan.  Used
 * when the pause time is 0, when no busy slot is worth blocking on (every
 * host is down), and when a blocking wait is not possible.
 **/
static void dcc_lock_pause(unsigned pause_time_ms)
{
	/*	This call to dcc_note_state() is made before the host is known, so it
		does not make sense and does nothing useful as far as I can tell.	*/
    /*	dcc_note_state(DCC_PHASE_BLOCKED, NULL, NULL, DCC_UNKNOWN);	*/

    rs_trace("nothing available, sleeping %ums...", pause_time_ms);

    if (pause_time_ms > 0)
        usleep(pause_time_ms * 1000);
}


#if defined(ITIMER_REAL)
#  define DCC_LOCK_CAN_BLOCK 1

static volatile sig_atomic_t dcc_lock_wait_timed_out;

static void dcc_lock_wait_alarm(int UNUSED(whichsig))
{
    dcc_lock_wait_timed_out = 1;
}
#endif


/**
 * Block for at most about @p timeout_ms trying to lock slot @p slot of
 * @p host.
 *
 * @retval 0 got the lock; *cpu_lock_fd holds it.
 * @retval EXIT_BUSY the timeout expired first.
 * @retval other the blocking wait is not possible or failed; the caller
 * should fall back to sleeping.
 **/
static int dcc_lock_wait(struct dcc_hostdef *host, int slot,
                         unsigned timeout_ms, int *cpu_lock_fd)
{
#if defined(DCC_LOCK_CAN_BLOCK)
    struct sigaction act, old_act;
    struct itimerval timer, old_timer, zero_timer;
    sigset_t alarm_set, old_mask, pending;
    int ret;
    int cleanup_ok = 1;

    memset(&zero_timer, 0, sizeof zero_timer);
    sigemptyset(&alarm_set);
    sigaddset(&alarm_set, SIGALRM);

    /* Keep SIGALRM blocked while we set up and tear down, so that a timer
     * expiry can only be delivered while we are inside the lock call and
     * never after the old disposition has been put back.  If SIGALRM was
     * already blocked, the lock call could not be interrupted at all, so
     * let the caller fall back to sleeping. */
    if (sigprocmask(SIG_BLOCK, &alarm_set, &old_mask) != 0)
        return EXIT_IO_ERROR;
    if (sigismember(&old_mask, SIGALRM)) {
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return EXIT_IO_ERROR;
    }

    /* Don't take over a real-time interval timer that someone else set, or
     * an alarm that is already on its way. */
    if ((getitimer(ITIMER_REAL, &old_timer) == 0
         && (old_timer.it_value.tv_sec || old_timer.it_value.tv_usec))
        || (sigpending(&pending) == 0 && sigismember(&pending, SIGALRM))) {
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return EXIT_IO_ERROR;
    }

    memset(&act, 0, sizeof act);
    act.sa_handler = dcc_lock_wait_alarm;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;           /* no SA_RESTART: interrupt the lock call */
    if (sigaction(SIGALRM, &act, &old_act) != 0) {
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return EXIT_IO_ERROR;
    }

    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_sec = timeout_ms / 1000;
    timer.it_value.tv_usec = (timeout_ms % 1000) * 1000;
    /* Repeat, in case the first expiry lands just before the lock call
     * starts and so does not interrupt it. */
    timer.it_interval = timer.it_value;

    dcc_lock_wait_timed_out = 0;
    if (setitimer(ITIMER_REAL, &timer, NULL) != 0) {
        sigaction(SIGALRM, &old_act, NULL);
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return EXIT_IO_ERROR;
    }

    /* dcc_lock_host() maps the EINTR from our timer to EXIT_BUSY, so an
     * expected timeout does not log an error.  SIGALRM is unblocked only
     * for the duration of this call. */
    sigprocmask(SIG_UNBLOCK, &alarm_set, NULL);
    ret = dcc_lock_host("cpu", host, slot, 1, cpu_lock_fd);
    sigprocmask(SIG_BLOCK, &alarm_set, NULL);

    /* Tear down in this order: stop the timer, swallow any expiry that was
     * generated but not yet delivered (it is held back by the block above),
     * then put the old handler and the old mask back. */
    if (setitimer(ITIMER_REAL, &zero_timer, NULL) != 0)
        cleanup_ok = 0;
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGALRM)) {
        int sig;
        if (sigwait(&alarm_set, &sig) == 0)
            dcc_lock_wait_timed_out = 1;
        else
            cleanup_ok = 0;
    }
    if (sigaction(SIGALRM, &old_act, NULL) != 0)
        cleanup_ok = 0;
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0)
        cleanup_ok = 0;
    if (!cleanup_ok)
        rs_log_warning("failed to restore SIGALRM state after waiting for a slot: %s",
                       strerror(errno));

    if (ret == 0) {
        rs_trace("got cpu lock on %s slot %d as fd%d after waiting",
                 host->hostdef_string, slot, *cpu_lock_fd);
        return 0;
    }
    if (dcc_lock_wait_timed_out) {
        rs_trace("%s slot %d still busy after %ums, rescanning",
                 host->hostdef_string, slot, timeout_ms);
        return EXIT_BUSY;
    }
    rs_trace("blocking wait on %s slot %d failed, falling back to polling",
             host->hostdef_string, slot);
    return EXIT_IO_ERROR;
#else
    (void) host;
    (void) slot;
    (void) timeout_ms;
    (void) cpu_lock_fd;
    return EXIT_IO_ERROR;
#endif
}


/**
 * Choose which of @p n_busy busy slots to block on in round @p n_round.
 *
 * The choice is a hash of our pid and the round number, so that concurrent
 * waiters spread over the slots instead of all queueing on slot 0, and so
 * that two waiters that happen to pick the same slot in one round are
 * unlikely to pick the same one again in the next.
 **/
static unsigned dcc_pick_wait_slot(unsigned n_round, unsigned n_busy)
{
    uint32_t x = (uint32_t) getpid() ^ ((uint32_t) n_round * 0x9e3779b9U);

    /* Finalizer from MurmurHash3. */
    x ^= x >> 16;
    x *= 0x85ebca6bU;
    x ^= x >> 13;
    x *= 0xc2b2ae35U;
    x ^= x >> 16;
    return x % n_busy;
}


/**
 * Find a host that can run a distributed compilation by examining local state.
 * It can be either a remote server or localhost (if that is in the list).
 *
 * This function does not return (except for errors) until a host has been
 * selected.  If necessary it waits until one is free; see "Waiting for a slot"
 * above.
 *
 * @todo We don't need transmit locks for local operations.
 **/
static int dcc_lock_one(struct dcc_hostdef *hostlist,
                        struct dcc_hostdef **buildhost,
                        int *cpu_lock_fd)
{
    struct dcc_hostdef *h;
    int i_cpu;
    int ret;
    unsigned n_rounds = 0;

    while (1) {
        /* Number of busy slots on hosts that are up, ie. ones worth
         * blocking on. */
        unsigned n_busy = 0;
        unsigned target;
        unsigned pause_time_ms;
        struct dcc_hostdef *wait_host = NULL;
        int wait_slot = 0;

        for (i_cpu = 0; i_cpu < 10000; i_cpu++) {
            char i_cpu_is_usable = 0;

            for (h = hostlist; h; h = h->next) {
                if (i_cpu >= h->n_slots)
                    continue;

                i_cpu_is_usable = 1;

                ret = dcc_lock_host("cpu", h, i_cpu, 0, cpu_lock_fd);

                if (ret == 0) {
                    *buildhost = h;
                    dcc_note_state_slot(i_cpu, strcmp(h->hostname, "localhost") == 0 ? DCC_LOCAL : DCC_REMOTE);
                    return 0;
                } else if (ret == EXIT_BUSY) {
                    if (h->is_up)
                        n_busy++;
                    continue;
                } else {
                    rs_log_error("failed to lock");
                    return ret;
                }
            }

            if (!i_cpu_is_usable)
                break;
        }

        pause_time_ms = dcc_get_pause_time_ms();

        if (pause_time_ms == 0 || n_busy == 0) {
            dcc_lock_pause(pause_time_ms);
            continue;
        }

        /* Pick the busy slot to block on, numbered in scan order. */
        target = dcc_pick_wait_slot(n_rounds++, n_busy);
        for (i_cpu = 0; !wait_host && i_cpu < 10000; i_cpu++) {
            char i_cpu_is_usable = 0;

            for (h = hostlist; h; h = h->next) {
                if (i_cpu >= h->n_slots)
                    continue;
                i_cpu_is_usable = 1;
                if (!h->is_up)
                    continue;
                if (target-- == 0) {
                    wait_host = h;
                    wait_slot = i_cpu;
                    break;
                }
            }

            if (!i_cpu_is_usable)
                break;
        }

        if (!wait_host) {
            /* Can't happen, but don't spin if it does. */
            dcc_lock_pause(pause_time_ms);
            continue;
        }

        /* Same prefix as the old poll's message, so logs that count
         * "nothing available, sleeping" still count waits. */
        rs_trace("nothing available, sleeping: waiting up to %ums for %s slot %d",
                 pause_time_ms, wait_host->hostdef_string, wait_slot);

        ret = dcc_lock_wait(wait_host, wait_slot, pause_time_ms, cpu_lock_fd);
        if (ret == 0) {
            *buildhost = wait_host;
            dcc_note_state_slot(wait_slot, strcmp(wait_host->hostname, "localhost") == 0 ? DCC_LOCAL : DCC_REMOTE);
            return 0;
        } else if (ret != EXIT_BUSY) {
            /* Blocking is not working; wait the old way this round. */
            dcc_lock_pause(pause_time_ms);
        }
    }
}



/**
 * Lock localhost.  Used to get the right balance of jobs when some of
 * them must be local.
 **/
int dcc_lock_local(int *cpu_lock_fd)
{
    struct dcc_hostdef *chosen;

    return dcc_lock_one(dcc_hostdef_local, &chosen, cpu_lock_fd);
}

int dcc_lock_local_cpp(int *cpu_lock_fd)
{
    int ret;
    struct dcc_hostdef *chosen;
    ret = dcc_lock_one(dcc_hostdef_local_cpp, &chosen, cpu_lock_fd);
    if (ret == 0) {
        dcc_note_state(DCC_PHASE_CPP, NULL, chosen->hostname, DCC_LOCAL);
    }
    return ret;
}

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

/* Persistent SHA-256 cache for mirror mode.
 *
 * The client is one process per compile and the daemon forks per job, so
 * the cache lives on disk: $DISTCC_DIR/mirror-digests, one line per file,
 *
 *   <dev> <ino> <size> <mtime_s> <mtime_ns> <ctime_s> <ctime_ns> <sha256> <path>
 *
 * A line is only used when every stat field matches the file now.  Any
 * write to a file changes its ctime (which cannot be set from user space),
 * and replacing it changes the inode, so a hit always describes the current
 * content.  New lines are appended with O_APPEND in a single write; readers
 * ignore any line that does not parse.  When the file grows well past the
 * number of live entries it is rewritten through a temporary file and an
 * atomic rename. */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "mirror.h"

#if defined(__APPLE__)
#  define ST_MTIME_NS(st) ((long long) (st)->st_mtimespec.tv_nsec)
#  define ST_CTIME_NS(st) ((long long) (st)->st_ctimespec.tv_nsec)
#  define ST_CTIME_S(st)  ((long long) (st)->st_ctimespec.tv_sec)
#else
#  define ST_MTIME_NS(st) ((long long) (st)->st_mtim.tv_nsec)
#  define ST_CTIME_NS(st) ((long long) (st)->st_ctim.tv_nsec)
#  define ST_CTIME_S(st)  ((long long) (st)->st_ctim.tv_sec)
#endif

struct digest_entry {
    char *path;
    unsigned long long dev, ino, size;
    long long mtime_s, mtime_ns, ctime_s, ctime_ns;
    char hex[DCC_SHA256_HEX_LEN + 1];
};

static struct digest_entry *table = NULL;   /* open addressing */
static size_t table_cap = 0, table_used = 0;
static size_t file_lines = 0;
static int loaded = 0;
static char *cache_fname = NULL;

static unsigned long long hash_str(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static struct digest_entry *lookup(const char *path, int create)
{
    size_t i;

    if (table_cap == 0) {
        if (!create)
            return NULL;
        table_cap = 16384;
        if (!(table = calloc(table_cap, sizeof *table)))
            return NULL;
    }
    if (create && (table_used + 1) * 2 > table_cap) {
        struct digest_entry *old = table;
        size_t old_cap = table_cap, j;
        table_cap *= 2;
        if (!(table = calloc(table_cap, sizeof *table))) {
            table = old;
            table_cap = old_cap;
            return NULL;
        }
        for (j = 0; j < old_cap; j++) {
            if (old[j].path) {
                i = hash_str(old[j].path) & (table_cap - 1);
                while (table[i].path)
                    i = (i + 1) & (table_cap - 1);
                table[i] = old[j];
            }
        }
        free(old);
    }
    i = hash_str(path) & (table_cap - 1);
    while (table[i].path) {
        if (strcmp(table[i].path, path) == 0)
            return &table[i];
        i = (i + 1) & (table_cap - 1);
    }
    if (!create)
        return NULL;
    if (!(table[i].path = strdup(path)))
        return NULL;
    table_used++;
    return &table[i];
}

static int parse_line(char *line, struct digest_entry *e, char **path)
{
    int n = 0, i;

    if (sscanf(line, "%llu %llu %llu %lld %lld %lld %lld %64s %n",
               &e->dev, &e->ino, &e->size, &e->mtime_s, &e->mtime_ns,
               &e->ctime_s, &e->ctime_ns, e->hex, &n) != 8 || n == 0)
        return -1;
    if (strlen(e->hex) != DCC_SHA256_HEX_LEN)
        return -1;
    for (i = 0; i < DCC_SHA256_HEX_LEN; i++)
        if (!((e->hex[i] >= '0' && e->hex[i] <= '9')
              || (e->hex[i] >= 'a' && e->hex[i] <= 'f')))
            return -1;
    if (line[n] != '/' && line[n] != 'D')
        return -1;
    *path = line + n;
    return 0;
}

static void load_cache(void)
{
    char *dir = NULL;
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    loaded = 1;
    if (dcc_get_top_dir(&dir) != 0)
        return;
    /* dcc_get_top_dir() returns its cached string: do not free it. */
    if (asprintf(&cache_fname, "%s/mirror-digests", dir) < 0)
        cache_fname = NULL;
    if (!cache_fname || !(f = fopen(cache_fname, "r")))
        return;
    while ((len = getline(&line, &cap, f)) > 0) {
        struct digest_entry e, *slot;
        char *path;
        if (line[len - 1] != '\n')
            break;              /* torn final line */
        line[len - 1] = '\0';
        memset(&e, 0, sizeof e);
        if (parse_line(line, &e, &path) != 0)
            continue;
        file_lines++;
        if (!(slot = lookup(path, 1)))
            break;
        e.path = slot->path;
        *slot = e;
    }
    free(line);
    fclose(f);
}

static int entry_matches(const struct digest_entry *e, const struct stat *st)
{
    return e->dev == (unsigned long long) st->st_dev
        && e->ino == (unsigned long long) st->st_ino
        && e->size == (unsigned long long) st->st_size
        && e->mtime_s == (long long) st->st_mtime
        && e->mtime_ns == ST_MTIME_NS(st)
        && e->ctime_s == ST_CTIME_S(st)
        && e->ctime_ns == ST_CTIME_NS(st);
}

static void append_line(const struct digest_entry *e)
{
    char *line = NULL;
    int fd, len;

    if (!cache_fname || strchr(e->path, '\n'))
        return;
    len = asprintf(&line, "%llu %llu %llu %lld %lld %lld %lld %s %s\n",
                   e->dev, e->ino, e->size, e->mtime_s, e->mtime_ns,
                   e->ctime_s, e->ctime_ns, e->hex, e->path);
    if (len < 0)
        return;
    if ((fd = open(cache_fname, O_WRONLY | O_APPEND | O_CREAT, 0644)) != -1) {
        if (write(fd, line, (size_t) len) != len)
            rs_trace("short write to %s", cache_fname);
        close(fd);
        file_lines++;
    }
    free(line);
}

/**
 * Look up @p key; succeeds only if the entry was made for a file whose
 * stat() result is exactly @p st.
 **/
int dcc_mirror_cache_get(const char *key, const struct stat *st,
                         char hex[DCC_SHA256_HEX_LEN + 1])
{
    struct digest_entry *e;

    if (!loaded)
        load_cache();
    if ((e = lookup(key, 0)) && entry_matches(e, st)) {
        memcpy(hex, e->hex, sizeof e->hex);
        return 0;
    }
    return -1;
}

void dcc_mirror_cache_put(const char *key, const struct stat *st,
                          const char hex[DCC_SHA256_HEX_LEN + 1])
{
    struct digest_entry *e;

    if (!loaded)
        load_cache();
    if ((e = lookup(key, 1)) != NULL) {
        e->dev = (unsigned long long) st->st_dev;
        e->ino = (unsigned long long) st->st_ino;
        e->size = (unsigned long long) st->st_size;
        e->mtime_s = (long long) st->st_mtime;
        e->mtime_ns = ST_MTIME_NS(st);
        e->ctime_s = ST_CTIME_S(st);
        e->ctime_ns = ST_CTIME_NS(st);
        memcpy(e->hex, hex, sizeof e->hex);
        append_line(e);
    }
}

/**
 * The SHA-256 of @p path, whose stat() result is @p st, as lowercase hex.
 **/
int dcc_mirror_digest(const char *path, const struct stat *st,
                      char hex[DCC_SHA256_HEX_LEN + 1])
{
    struct stat after;
    char abs_buf[MAXPATHLEN + 1];
    const char *key = path;
    int ret;

    /* Cache keys are absolute so the cache can be shared across cwds. */
    if (path[0] != '/') {
        if (!getcwd(abs_buf, sizeof abs_buf - strlen(path) - 2))
            key = NULL;
        else {
            strcat(abs_buf, "/");
            strcat(abs_buf, path);
            key = abs_buf;
        }
    }

    if (key && dcc_mirror_cache_get(key, st, hex) == 0)
        return 0;

    if ((ret = dcc_sha256_file_hex(path, hex)))
        return ret;

    /* Only record the digest if the file did not change while it was read.
     * A file changing under us is still returned (the comparison will
     * simply fail or succeed on what was read), but not cached. */
    if (!key || stat(path, &after) == -1)
        return 0;
    if (after.st_ino != st->st_ino || after.st_size != st->st_size
        || after.st_mtime != st->st_mtime
        || ST_MTIME_NS(&after) != ST_MTIME_NS(st)
        || ST_CTIME_S(&after) != ST_CTIME_S(st)
        || ST_CTIME_NS(&after) != ST_CTIME_NS(st))
        return 0;
    dcc_mirror_cache_put(key, st, hex);
    return 0;
}

/**
 * Rewrite the cache file without stale or duplicate lines when it has grown
 * to more than four times the live entries.  Cheap to call after each job.
 **/
void dcc_mirror_digest_flush(void)
{
    char *tmp = NULL;
    FILE *f;
    size_t i;

    if (!cache_fname || file_lines < 4 * table_used + 4096)
        return;
    if (asprintf(&tmp, "%s.%ld.tmp", cache_fname, (long) getpid()) < 0)
        return;
    if (!(f = fopen(tmp, "w"))) {
        free(tmp);
        return;
    }
    for (i = 0; i < table_cap; i++) {
        const struct digest_entry *e = &table[i];
        if (!e->path || strchr(e->path, '\n'))
            continue;
        fprintf(f, "%llu %llu %llu %lld %lld %lld %lld %s %s\n",
                e->dev, e->ino, e->size, e->mtime_s, e->mtime_ns,
                e->ctime_s, e->ctime_ns, e->hex, e->path);
    }
    if (fclose(f) == 0 && rename(tmp, cache_fname) == 0)
        file_lines = table_used;
    else
        unlink(tmp);
    free(tmp);
}

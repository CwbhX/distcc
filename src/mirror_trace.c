/* Compiler filesystem observation for mirrored jobs on macOS.
 *
 * This is loaded only into a capability-tested compiler. It records actual
 * filesystem lookups, including unsuccessful __has_include searches, during
 * the ordinary compilation. It does not attempt to interpret source text.
 * Observer loading is not a general guarantee of syscall coverage: callers
 * must reject unsupported compiler binaries/options and incomplete traces.
 */
#ifdef __APPLE__

#include "mirror_trace.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>

static int trace_fd = -1;
static int trace_broken;
static __thread int observing;
static char job_directory[PATH_MAX];
static const char *absolute_path(int, const char *, char [PATH_MAX]);

static int private_output(int dirfd, const char *path)
{
    char storage[PATH_MAX];
    const char *absolute;
    size_t length = strlen(job_directory);
    absolute = absolute_path(dirfd, path, storage);
    return length && absolute && !strncmp(absolute, job_directory, length)
        && absolute[length] == '/' && !strstr(absolute, "/../")
        && strcmp(absolute + strlen(absolute) - 3, "/..") != 0;
}

/* One append write keeps records from concurrent compiler threads/processes
 * together. A short write leaves a truncated record, which the reader rejects.
 * Never use stdio here: it may enter an interposed lookup recursively.
 */
static void emit(uint32_t type, int error, mode_t mode, const char *path)
{
    unsigned char bytes[sizeof(struct dcc_mirror_trace_record) + PATH_MAX];
    struct dcc_mirror_trace_record record;
    size_t length = path ? strlen(path) : 0;
    ssize_t written;
    if (trace_fd < 0 || __atomic_load_n(&trace_broken, __ATOMIC_RELAXED))
        return;
    if (length >= PATH_MAX) {
        type = DCC_MIRROR_TRACE_ERROR;
        error = ENAMETOOLONG;
        length = 0;
    }
    record.magic = DCC_MIRROR_TRACE_MAGIC;
    record.type = type;
    record.pid = (uint32_t)getpid();
    record.error = (uint32_t)error;
    record.mode = (uint32_t)mode;
    record.path_length = (uint32_t)length;
    memcpy(bytes, &record, sizeof(record));
    if (length)
        memcpy(bytes + sizeof(record), path, length);
    do {
        written = write(trace_fd, bytes, sizeof(record) + length);
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)(sizeof(record) + length))
        __atomic_store_n(&trace_broken, 1, __ATOMIC_RELAXED);
}

/* Resolve relative *at() queries against the directory actually consulted.
 * F_GETPATH is necessary: resolving against cwd would be wrong for openat.
 * Keep spelling (including symlinks and '..') so validation repeats the same
 * filesystem query rather than accidentally changing its interpretation.
 */
static const char *absolute_path(int dirfd, const char *path,
                                 char result[PATH_MAX])
{
    char base[PATH_MAX];
    size_t n, m;
    if (!path || !*path)
        return NULL;
    if (path[0] == '/')
        return path;
    if (dirfd == AT_FDCWD) {
        if (!getcwd(base, sizeof(base)))
            return NULL;
    } else if (fcntl(dirfd, F_GETPATH, base) < 0) {
        return NULL;
    }
    n = strlen(base);
    m = strlen(path);
    if (n + 1 + m >= PATH_MAX)
        return NULL;
    memcpy(result, base, n);
    result[n] = '/';
    memcpy(result + n + 1, path, m + 1);
    return result;
}

static void query_kind(uint32_t type, int dirfd, const char *path,
                       int result, int error, mode_t mode)
{
    char storage[PATH_MAX];
    const char *absolute;
    if (trace_fd < 0 || observing)
        return;
    observing = 1;
    absolute = absolute_path(dirfd, path, storage);
    if (!absolute || (result < 0 && error != ENOENT && error != ENOTDIR))
        emit(DCC_MIRROR_TRACE_ERROR, error ? error : EINVAL, 0, NULL);
    else
        emit(type, result < 0 ? error : 0, mode, absolute);
    observing = 0;
}

static void query(int dirfd, const char *path, int result, int error, mode_t mode)
{
    query_kind(DCC_MIRROR_TRACE_QUERY, dirfd, path, result, error, mode);
}

static void check_identity(const struct stat *st)
{
    /* File identity affects #pragma once. Content and type alone cannot
     * distinguish two hardlinked headers from independent identical files.
     * The client performs the same check on its positive regular queries. */
    if (trace_fd >= 0 && !observing && S_ISREG(st->st_mode) && st->st_nlink > 1)
        emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
}

static int trace_stat(const char *path, struct stat *buf)
{
    int r = stat(path, buf), e = errno;
    if (r == 0)
        check_identity(buf);
    query(AT_FDCWD, path, r, e, r == 0 ? buf->st_mode : 0);
    errno = e;
    return r;
}

static int trace_lstat(const char *path, struct stat *buf)
{
    int r = lstat(path, buf), e = errno;
    if (r == 0)
        check_identity(buf);
    /* A symlink itself is observable; preserve its type rather than silently
     * converting it into a successful regular-file query. */
    query_kind(DCC_MIRROR_TRACE_LQUERY, AT_FDCWD, path, r, e,
               r == 0 ? buf->st_mode : 0);
    errno = e;
    return r;
}

static int trace_fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    int r = fstatat(dirfd, path, buf, flags), e = errno;
    if (r == 0)
        check_identity(buf);
    if ((flags & ~AT_SYMLINK_NOFOLLOW) && !observing)
        emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
    else
        query_kind(flags & AT_SYMLINK_NOFOLLOW ? DCC_MIRROR_TRACE_LQUERY
                                              : DCC_MIRROR_TRACE_QUERY,
                   dirfd, path, r, e, r == 0 ? buf->st_mode : 0);
    errno = e;
    return r;
}

static void query_with_stat(int dirfd, const char *path, int result, int error)
{
    struct stat buf;
    mode_t mode = 0;
    if (result >= 0 && !observing && trace_fd >= 0) {
        observing = 1;
        if (fstatat(dirfd, path, &buf, 0) == 0) {
            mode = buf.st_mode;
            if (S_ISREG(buf.st_mode) && buf.st_nlink > 1)
                emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
        } else
            emit(DCC_MIRROR_TRACE_ERROR, errno, 0, NULL);
        observing = 0;
    }
    query(dirfd, path, result, error, mode);
}

static int trace_access(const char *path, int mode)
{
    int r = access(path, mode), e = errno;
    query_with_stat(AT_FDCWD, path, r, e);
    errno = e;
    return r;
}

static int trace_faccessat(int dirfd, const char *path, int mode, int flags)
{
    int r = faccessat(dirfd, path, mode, flags), e = errno;
    if (flags && !observing)
        emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
    else
        query_with_stat(dirfd, path, r, e);
    errno = e;
    return r;
}

static void query_open(int dirfd, const char *path, int flags, int fd, int error)
{
    struct stat buf;
    mode_t mode = 0;
    /* Clang writes PCHs through O_RDWR|O_CREAT. Only its private output
     * directory may be excluded from read/write dependencies; accepting
     * O_CREAT everywhere could conceal reads of an existing source file. */
    if ((flags & O_ACCMODE) == O_WRONLY)
        return;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
        if (!observing && !private_output(dirfd, path))
            emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
        return;
    }
    if (fd >= 0 && !observing && trace_fd >= 0) {
        observing = 1;
        if (fstat(fd, &buf) == 0) {
            mode = buf.st_mode;
            if (S_ISREG(buf.st_mode) && buf.st_nlink > 1)
                emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
        } else
            emit(DCC_MIRROR_TRACE_ERROR, errno, 0, NULL);
        observing = 0;
    }
    query(dirfd, path, fd, error, mode);
}

static int trace_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    int r, e;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    r = open(path, flags, mode);
    e = errno;
    query_open(AT_FDCWD, path, flags, r, e);
    errno = e;
    return r;
}

static int trace_openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    int r, e;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    r = openat(dirfd, path, flags, mode);
    e = errno;
    query_open(dirfd, path, flags, r, e);
    errno = e;
    return r;
}

static FILE *trace_fopen(const char *path, const char *mode)
{
    FILE *r = fopen(path, mode);
    int e = errno;
    if (mode && mode[0] == 'r' && !strchr(mode, '+'))
        query_open(AT_FDCWD, path, O_RDONLY, r ? fileno(r) : -1, e);
    else if (mode && strchr(mode, '+') && !observing)
        emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
    errno = e;
    return r;
}

static char *trace_realpath(const char *path, char *resolved)
{
    char *r = realpath(path, resolved);
    int e = errno;
    query_with_stat(AT_FDCWD, path, r ? 0 : -1, e);
    /* realpath's returned spelling can affect subsequent include search and
     * source locations. Verify every link along the original path rather
     * than only the final file reached through those links. */
    if (r && trace_fd >= 0 && !observing) {
        char storage[PATH_MAX], prefixes[PATH_MAX];
        const char *absolute;
        size_t i, length;
        struct stat st;
        observing = 1;
        absolute = absolute_path(AT_FDCWD, path, storage);
        if (!absolute || (length = strlen(absolute)) >= sizeof(prefixes)) {
            emit(DCC_MIRROR_TRACE_ERROR, ENAMETOOLONG, 0, NULL);
        } else {
            memcpy(prefixes, absolute, length + 1);
            for (i = 1; i <= length; i++) {
                if (prefixes[i] == '/' || prefixes[i] == '\0') {
                    char saved = prefixes[i];
                    prefixes[i] = '\0';
                    if (lstat(prefixes, &st) == 0) {
                        emit(DCC_MIRROR_TRACE_LQUERY, 0, st.st_mode, prefixes);
                        if (S_ISREG(st.st_mode) && st.st_nlink > 1)
                            emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
                    } else
                        emit(DCC_MIRROR_TRACE_ERROR, errno, 0, NULL);
                    prefixes[i] = saved;
                }
            }
        }
        observing = 0;
    }
    errno = e;
    return r;
}

static ssize_t trace_readlink(const char *path, char *buf, size_t size)
{
    ssize_t r = readlink(path, buf, size);
    int e = errno;
    /* readlink on ordinary files legitimately returns EINVAL. No dependency
     * is lost: validate that file's type/content as a successful stat query. */
    if (r < 0 && e == EINVAL)
        query_with_stat(AT_FDCWD, path, 0, 0);
    else if (r >= 0)
        query_kind(DCC_MIRROR_TRACE_LQUERY, AT_FDCWD, path, 0, 0, S_IFLNK);
    else
        query(AT_FDCWD, path, (int)r, e, 0);
    errno = e;
    return r;
}

static DIR *trace_opendir(const char *path)
{
    DIR *r = opendir(path);
    int e = errno;
    query_with_stat(AT_FDCWD, path, r ? 0 : -1, e);
    errno = e;
    return r;
}

static struct dirent *trace_readdir(DIR *directory)
{
    struct dirent *r = readdir(directory);
    int e = errno;
    /* A directory's existence is not its listing. Until listing identities
     * are in the protocol, do not accept compilations that consume one. */
    if (!observing)
        emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
    errno = e;
    return r;
}

static int trace_statfs(const char *path, struct statfs *buf)
{
    int r = statfs(path, buf), e = errno;
    /* Filesystem flags can affect compiler behavior (for example case
     * sensitivity); the current manifest does not carry those flags. */
    if (!observing)
        emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
    errno = e;
    return r;
}

/* Darwin exports both the legacy and extended fopen ABI. Clang 21 imports
 * both; interposing only the one selected by the SDK's declaration misses
 * queries through the other symbol. */
extern FILE *original_fopen_extsn(const char *, const char *)
    __asm("_fopen$DARWIN_EXTSN");
static FILE *trace_fopen_extsn(const char *path, const char *mode)
{
    FILE *r = original_fopen_extsn(path, mode);
    int e = errno;
    if (mode && mode[0] == 'r' && !strchr(mode, '+'))
        query_open(AT_FDCWD, path, O_RDONLY, r ? fileno(r) : -1, e);
    else if (mode && strchr(mode, '+') && !observing)
        emit(DCC_MIRROR_TRACE_ERROR, ENOTSUP, 0, NULL);
    errno = e;
    return r;
}

#define INTERPOSE(replacement_fn, original_fn) \
    __attribute__((used)) static struct { \
        const void *replacement; const void *original; \
    } interpose_##original_fn __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)&replacement_fn, (const void *)&original_fn \
    }

INTERPOSE(trace_stat, stat);
INTERPOSE(trace_lstat, lstat);
INTERPOSE(trace_fstatat, fstatat);
INTERPOSE(trace_access, access);
INTERPOSE(trace_faccessat, faccessat);
INTERPOSE(trace_open, open);
INTERPOSE(trace_openat, openat);
INTERPOSE(trace_fopen, fopen);
INTERPOSE(trace_fopen_extsn, original_fopen_extsn);
INTERPOSE(trace_realpath, realpath);
INTERPOSE(trace_readlink, readlink);
INTERPOSE(trace_opendir, opendir);
INTERPOSE(trace_readdir, readdir);
INTERPOSE(trace_statfs, statfs);

__attribute__((constructor)) static void trace_start(void)
{
    const char *path = getenv(DCC_MIRROR_TRACE_ENV);
    int saved = errno;
    observing = 1;
    if (path && path[0] == '/') {
        const char *slash = strrchr(path, '/');
        size_t length = (size_t)(slash - path);
        if (length > 0 && length < sizeof(job_directory)) {
            memcpy(job_directory, path, length);
            job_directory[length] = '\0';
        }
        trace_fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
        if (trace_fd >= 0) {
            struct stat st;
            if (fstat(trace_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                close(trace_fd);
                trace_fd = -1;
            } else {
                emit(DCC_MIRROR_TRACE_START, 0, 0, NULL);
            }
        }
    }
    observing = 0;
    errno = saved;
}

__attribute__((destructor)) static void trace_end(void)
{
    int saved = errno;
    observing = 1;
    emit(DCC_MIRROR_TRACE_END, 0, 0, NULL);
    if (trace_fd >= 0)
        close(trace_fd);
    trace_fd = -1;
    observing = 0;
    errno = saved;
}

#endif /* __APPLE__ */

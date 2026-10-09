#!/usr/bin/env python3
"""Exercise the macOS observer against actual filesystem calls and Clang."""

import collections
import errno
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == "darwin", "macOS compiler observer")
class MirrorTrace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="distcc-observer-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name).resolve()
        cls.compiler = subprocess.check_output(
            ["xcrun", "--find", "clang"], text=True).strip()
        cls.sdk = subprocess.check_output(
            ["xcrun", "--show-sdk-path"], text=True).strip()
        configured = os.environ.get("DISTCC_MIRROR_TRACE_LIBRARY")
        if configured:
            cls.library = Path(configured).resolve()
            if not cls.library.is_file():
                raise AssertionError("configured observer was not built: "
                                     + str(cls.library))
        elif Path("distcc-mirror-trace.dylib").is_file():
            cls.library = Path("distcc-mirror-trace.dylib").resolve()
        else:
            cls.library = cls.directory / "observer.dylib"
            source = Path(__file__).resolve().parents[1] / "src/mirror_trace.c"
            cls.compile(["-Wall", "-Wextra", "-Werror", "-dynamiclib",
                         str(source), "-o", str(cls.library)])

    @classmethod
    def compile(cls, arguments, env=None):
        return subprocess.run([cls.compiler, "-isysroot", cls.sdk] + arguments,
                              env=env, check=True, capture_output=True, text=True)

    def trace(self, program, arguments, allow_errors=False):
        trace = self.directory / (program.name + ".trace")
        trace.write_bytes(b"")
        env = dict(os.environ, DISTCC_MIRROR_TRACE=str(trace),
                   DYLD_INSERT_LIBRARIES=str(self.library))
        subprocess.run([str(program)] + arguments, env=env, check=True,
                       capture_output=True, text=True)
        data = trace.read_bytes()
        records = []
        offset = 0
        lifecycle = collections.defaultdict(list)
        while offset < len(data):
            self.assertGreaterEqual(len(data) - offset, 24)
            magic, kind, pid, error, mode, length = struct.unpack_from(
                "=6I", data, offset)
            offset += 24
            self.assertEqual(magic, 0x44545231)
            self.assertLessEqual(length, len(data) - offset)
            path = os.fsdecode(data[offset:offset + length])
            offset += length
            records.append((kind, error, mode, path))
            if kind in (1, 2):
                lifecycle[pid].append(kind)
            elif kind in (3, 5):
                self.assertTrue(path.startswith("/"))
            if not allow_errors:
                self.assertNotEqual(kind, 4, "observer rejected a query")
        self.assertTrue(lifecycle)
        self.assertTrue(all(value == [1, 2] for value in lifecycle.values()),
                        lifecycle)
        return records

    def test_unrepresented_identity_and_directory_listing_fail_closed(self):
        source = self.directory / "unsupported.c"
        source.write_text(r'''
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <unistd.h>
#include <assert.h>
int main(int argc, char **argv) {
    struct stat st; struct statfs fs; DIR *directory;
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(stat("linked_a", &st) == 0);
    assert(statfs(".", &fs) == 0);
    directory = opendir("."); assert(directory != NULL);
    (void)readdir(directory); closedir(directory);
    return 0;
}
''')
        (self.directory / "linked_a").write_text("shared inode")
        os.link(self.directory / "linked_a", self.directory / "linked_b")
        binary = self.directory / "unsupported"
        self.compile([str(source), "-o", str(binary)])
        records = self.trace(binary, [str(self.directory)], allow_errors=True)
        self.assertGreaterEqual(sum(r[0] == 4 for r in records), 3)

    def test_filesystem_apis_preserve_errors_and_capture_types(self):
        source = self.directory / "probe.c"
        source.write_text(r'''
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
static void *worker(void *unused) {
    struct stat st; int i;
    (void)unused;
    for (i=0; i<100; ++i) {
        assert(stat("missing", &st) == -1);
        assert(errno == ENOENT);
    }
    return NULL;
}
int main(int argc, char **argv) {
    struct stat st; int fd, i; char link[1024]; FILE *f;
    pthread_t workers[4];
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(stat("missing", &st) == -1 && errno == ENOENT);
    assert(lstat("missing", &st) == -1 && errno == ENOENT);
    assert(access("missing", F_OK) == -1 && errno == ENOENT);
    assert(open("missing", O_RDONLY) == -1 && errno == ENOENT);
    assert(fopen("missing", "r") == NULL && errno == ENOENT);
    assert(realpath("missing", NULL) == NULL && errno == ENOENT);
    assert(stat("regular/child", &st) == -1 && errno == ENOTDIR);
    assert(stat("regular", &st) == 0 && S_ISREG(st.st_mode));
    assert(stat("directory", &st) == 0 && S_ISDIR(st.st_mode));
    assert(lstat("link", &st) == 0 && S_ISLNK(st.st_mode));
    assert(readlink("link", link, sizeof link) > 0);
    f = fopen("regular", "r"); assert(f != NULL); fclose(f);
    fd = open("directory", O_RDONLY); assert(fd >= 0);
    assert(openat(fd, "missing", O_RDONLY) == -1 && errno == ENOENT);
    assert(fstatat(fd, "missing", &st, 0) == -1 && errno == ENOENT);
    assert(faccessat(fd, "missing", F_OK, 0) == -1 && errno == ENOENT);
    close(fd);
    for (i=0; i<4; ++i) assert(pthread_create(&workers[i], NULL, worker, NULL)==0);
    for (i=0; i<4; ++i) assert(pthread_join(workers[i], NULL)==0);
    return 0;
}
''')
        (self.directory / "regular").write_text("content")
        (self.directory / "directory").mkdir(exist_ok=True)
        (self.directory / "link").symlink_to("regular")
        binary = self.directory / "probe"
        self.compile([str(source), "-pthread", "-o", str(binary)])
        records = self.trace(binary, [str(self.directory)])
        missing = str(self.directory / "missing")
        self.assertGreaterEqual(sum(r[0] == 3 and r[1] == errno.ENOENT
                                    and r[3] == missing for r in records), 404)
        self.assertTrue(any(r[1] == errno.ENOTDIR for r in records))
        self.assertTrue(any(r[3] == str(self.directory / "directory/missing")
                            and r[1] == errno.ENOENT for r in records))
        for filename, kind, filetype in (("regular", 3, stat.S_IFREG),
                                        ("directory", 3, stat.S_IFDIR),
                                        ("link", 5, stat.S_IFLNK)):
            self.assertTrue(any(r[0] == kind and not r[1]
                                and stat.S_IFMT(r[2]) == filetype
                                and r[3] == str(self.directory / filename)
                                for r in records), filename)

    def test_actual_clang_macro_optional_queries_and_pch(self):
        source = self.directory / "optional.h"
        source.write_text('''
#define CAT_INNER(a,b) a##b
#define CAT(a,b) CAT_INNER(a,b)
#define HEADER "observer-absent-header.h"
#if CAT(__has_,include)(HEADER)
int answer = 1;
#else
int answer = 0;
#endif
''')
        pch = self.directory / "optional.pch"
        args = ["-isysroot", self.sdk, "-x", "c-header", str(source),
                "-o", str(pch)]
        records = self.trace(Path(self.compiler), args)
        self.assertTrue(any(r[0] == 3 and r[1] == errno.ENOENT
                            and r[3].endswith("observer-absent-header.h")
                            for r in records))
        use = self.directory / "use.c"
        use.write_text("int get_answer(void) { return answer; }\n")
        records = self.trace(Path(self.compiler),
                             ["-isysroot", self.sdk, "-include-pch", str(pch),
                              "-c", str(use), "-o", str(self.directory / "use.o")])
        self.assertFalse(any(r[3].endswith("observer-absent-header.h")
                             for r in records))
        self.assertTrue(any(r[0] == 3 and r[3] == str(pch)
                            and stat.S_ISREG(r[2]) for r in records))


if __name__ == "__main__":
    unittest.main()

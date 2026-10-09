#!/usr/bin/env python3
"""Regression tests for the mirror CLI's compile/comparison exit status."""

import contextlib
import io
import os
from pathlib import Path
import runpy
import subprocess
import types
import unittest
from unittest import mock


class MirrorTestStatus(unittest.TestCase):
    def run_test_command(self, results, no_compare=False):
        namespace = runpy.run_path(str(Path(__file__).resolve().parents[1]
                                      / "contrib" / "distcc-mirror"))
        command = namespace["cmd_test"]
        globals_ = command.__globals__
        helper = types.SimpleNamespace(ssh="helper", addr="localhost", port=3634)
        cfg = types.SimpleNamespace(build_dir=".", helpers=[helper], roots=[],
                                    full_env=lambda: {}, distcc=lambda: "distcc")
        args = types.SimpleNamespace(build_dir=None, match=None, seed=1,
                                     count=100, helper=None, no_compare=no_compare)
        # Each result is one job: distributed status/stderr/object and local
        # status/stderr/object. Successful subprocesses create real objects so
        # the production comparison and cleanup code run unchanged.
        queue = []
        for remote, local in results:
            queue.append(remote)
            if not no_compare and remote[0] == 0:
                queue.append(local)

        def run(argv, **kwargs):
            status, stderr, contents = queue.pop(0)
            if status == 0:
                Path(argv[argv.index("-o") + 1]).write_bytes(contents)
            if not kwargs.get("text"):
                stderr = stderr.encode()
            return subprocess.CompletedProcess(argv, status, stderr=stderr)

        entries = [(os.getcwd(), "file%d.c" % i,
                    ["cc", "-c", "file%d.c" % i, "-o", "original.o"])
                   for i in range(len(results))]
        output = io.StringIO()
        with mock.patch.dict(globals_, load_config=lambda _: cfg,
                             compile_entries=lambda _: entries,
                             port_open=lambda *_: True), \
                mock.patch.object(subprocess, "run", side_effect=run), \
                contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output):
            try:
                command(args)
                status = 0
            except SystemExit as exc:
                status = exc.code
        self.assertEqual(queue, [])
        return status, output.getvalue()

    def test_distcc_failure_without_diagnostics(self):
        status, output = self.run_test_command([((7, "", b""), None)])
        self.assertEqual(status, 1)
        self.assertIn("status 7 (no diagnostics)", output)
        self.assertIn("1 distcc compile(s) failed", output)
        self.assertNotIn("Some jobs compiled successfully using classic distcc", output)

    def test_no_compare_failure(self):
        status, output = self.run_test_command(
            [((1, "error: remote failure\n", b""), None)], no_compare=True)
        self.assertEqual(status, 1)
        self.assertIn("error: remote failure", output)

    def test_local_failure_with_and_without_diagnostics(self):
        for stderr in ("", "error: local failure\n"):
            with self.subTest(stderr=stderr):
                status, output = self.run_test_command(
                    [((0, "compiled in the mirror on helper", b"object"),
                      (2, stderr, b""))])
                self.assertEqual(status, 1)
                self.assertIn("LOCAL COMPILE FAILED", output)
                self.assertIn("1 local comparison compile(s) failed", output)
                self.assertNotIn("identical", output)
                self.assertIn(stderr.strip() or "status 2 (no diagnostics)", output)

    def test_mixed_failures_are_all_counted(self):
        status, output = self.run_test_command([
            ((1, "failure", b""), None),
            ((0, "compiled in the mirror on helper", b"a"), (1, "", b"")),
            ((0, "compiled in the mirror on helper", b"a"), (0, "", b"b")),
        ])
        self.assertEqual(status, 1)
        self.assertIn("1 distcc compile(s) failed", output)
        self.assertIn("1 local comparison compile(s) failed", output)
        self.assertIn("1 object(s) differ", output)
        self.assertNotIn("Some jobs compiled successfully using classic distcc", output)

    def test_successful_fallback_and_identical_comparison(self):
        status, output = self.run_test_command(
            [((0, "", b"object"), (0, "", b"object"))])
        self.assertEqual(status, 0)
        self.assertIn("identical", output)
        self.assertIn("Some jobs compiled successfully using classic distcc", output)

    def test_successful_no_compare(self):
        status, _ = self.run_test_command(
            [((0, "compiled in the mirror on helper", b"object"), None)],
            no_compare=True)
        self.assertEqual(status, 0)


if __name__ == "__main__":
    unittest.main()

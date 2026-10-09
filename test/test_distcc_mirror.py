#!/usr/bin/env python3
"""Regression tests for mirror CLI failures, capacity detection and builds."""

import contextlib
import io
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import types
import unittest
from unittest import mock


TOOL = Path(__file__).resolve().parents[1] / "contrib" / "distcc-mirror"


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


class MirrorCapacity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="distcc-capacity-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "build").mkdir()
        self.path = self.root / "project.conf"
        self.namespace = runpy.run_path(str(TOOL))
        self.command = self.namespace["cmd_init"]
        self.globals = self.command.__globals__

    def init(self, main_cores=8, remote_cores="20", local_jobs=None,
             helper="render-node", client_addr=""):
        args = types.SimpleNamespace(
            root=str(self.root), build="build", name="project", force=True,
            helper=[helper], port=None, local_jobs=local_jobs,
            client_addr=client_addr, exclude=[], extra=[], prefix=None, set=[])
        reply = "ncpu=%s\nclient=198.51.100.1\n" % remote_cores
        response = subprocess.CompletedProcess([], 0, reply, "")
        output = io.StringIO()
        with mock.patch.dict(self.globals,
                             config_path_for=lambda _: str(self.path),
                             ssh_hostname=lambda _: "198.51.100.2",
                             cmake_launcher=lambda _: None), \
                mock.patch.object(os, "cpu_count", return_value=main_cores), \
                mock.patch.dict(self.globals, remote=mock.Mock(
                    return_value=response)), \
                contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output):
            remote = self.globals["remote"]
            try:
                self.command(args)
                status = 0
            except SystemExit as exc:
                status = exc.code
        return status, output.getvalue(), remote

    def test_unequal_machines_detect_independent_budgets(self):
        for main, remote in ((8, 20), (20, 4), (1, 1)):
            with self.subTest(main=main, remote=remote):
                status, output, probe = self.init(main, str(remote))
                self.assertEqual(status, 0, output)
                config = self.namespace["Config"](str(self.path))
                self.assertEqual(config.local_jobs, main + 2)
                self.assertEqual(config.helpers[0].jobs, remote + 2)
                self.assertEqual(config.total_jobs(), main + remote + 4)
                self.assertEqual(config.env()["DISTCC_HOSTS"],
                                 "198.51.100.2:3634/%d,mirror localhost/%d"
                                 % (remote + 2, main + 2))
                self.assertIn("main Mac: %d slots" % (main + 2), output)
                self.assertIn("remote Mac render-node", output)
                self.assertIn("combined build budget: %d jobs"
                              % (main + remote + 4), output)
                self.assertEqual(probe.call_args.args[1], "render-node")

    def test_explicit_slot_overrides_need_no_core_detection(self):
        status, output, probe = self.init(
            main_cores=None, remote_cores="", local_jobs=5,
            helper="other-computer=198.51.100.2/7",
            client_addr="198.51.100.1")
        self.assertEqual(status, 0, output)
        probe.assert_not_called()
        config = self.namespace["Config"](str(self.path))
        self.assertEqual(config.total_jobs(), 12)
        self.assertIn("manual override", output)

    def test_missing_or_invalid_remote_cores_are_not_guessed(self):
        for cores in ("", "unavailable", "0", "-1"):
            with self.subTest(cores=cores):
                status, output, _ = self.init(remote_cores=cores)
                self.assertEqual(status, 1)
                self.assertIn("cannot detect the core count for remote Mac", output)
                self.assertFalse(self.path.exists())

    def test_missing_main_cores_are_not_guessed(self):
        status, output, probe = self.init(main_cores=None)
        self.assertEqual(status, 1)
        self.assertIn("cannot detect the main machine's core count", output)
        probe.assert_not_called()
        self.assertFalse(self.path.exists())

    def test_zero_slot_overrides_are_rejected(self):
        for args in ({"local_jobs": 0}, {"helper": "render-node/0"}):
            with self.subTest(args=args):
                status, output, _ = self.init(**args)
                self.assertEqual(status, 1)
                self.assertIn("positive compile slot count", output)
                self.assertFalse(self.path.exists())

    def test_handwritten_config_detects_slots_with_its_ssh_transport(self):
        self.path.write_text("ROOTS=%s\nBUILD_DIR=%s\nHELPERS=render-node\n"
                             "RSH='ssh -p 2222'\n" %
                             (self.root, self.root / "build"))
        response = subprocess.CompletedProcess([], 0, "ncpu=10\n", "")
        with mock.patch.object(os, "cpu_count", return_value=6), \
                mock.patch.dict(self.globals,
                                ssh_hostname=lambda _: "198.51.100.2"), \
                mock.patch.dict(self.globals, remote=mock.Mock(
                    return_value=response)):
            config = self.namespace["Config"](str(self.path))
            self.assertEqual(config.total_jobs(), 20)
            probe = self.globals["remote"]
            self.assertEqual(probe.call_args.args[:2],
                             ("ssh -p 2222", "render-node"))

    def test_build_uses_combined_capacity_and_respects_override(self):
        (self.root / "build" / "build.ninja").touch()
        config = types.SimpleNamespace(
            build_dir=str(self.root / "build"), full_env=lambda: {},
            total_jobs=lambda: 34)
        command = self.namespace["cmd_build"]
        for extra, expected in (([], ["-j34"]),
                                (["-j7", "target"], ["-j7", "target"])):
            with self.subTest(extra=extra):
                args = types.SimpleNamespace(build_dir=None, ninja_args=extra,
                                             no_pch=True, no_sync=True)
                with mock.patch.dict(self.globals, load_config=lambda _: config), \
                        mock.patch.object(subprocess, "run", return_value=
                                          subprocess.CompletedProcess([], 0)) as run, \
                        contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(SystemExit) as exit_:
                        command(args)
                self.assertEqual(exit_.exception.code, 0)
                self.assertEqual(run.call_args.args[0],
                                 ["ninja", "-C", config.build_dir] + expected)


class MirrorBuildScript(unittest.TestCase):
    def test_combined_distcc_slots_and_manual_override(self):
        with tempfile.TemporaryDirectory(prefix="distcc-budget-") as directory:
            root = Path(directory)
            log = root / "ninja.log"
            ninja = root / "ninja"
            ninja.write_text("#!/bin/sh\ncase \"$*\" in *'-t targets all'*) "
                             "exit 0;; esac\nprintf '%s\\n' \"$*\" "
                             "> \"$NINJA_TEST_LOG\"\n")
            ninja.chmod(0o755)
            distcc = root / "distcc"
            distcc.write_text("#!/bin/sh\ncase \"$1\" in -j) echo 34;; "
                              "--mirror-sync) exit 0;; *) exit 1;; esac\n")
            distcc.chmod(0o755)
            script = TOOL.parent / "mirror-build.sh"
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                       DISTCC=str(distcc), NINJA_TEST_LOG=str(log))
            for flags, expected in (([], "-C build -j 34"),
                                    (["-j7", "target"], "-C build -j7 target")):
                result = subprocess.run(["/bin/bash", str(script), "build"] + flags,
                                        env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(log.read_text().strip(), expected)


if __name__ == "__main__":
    unittest.main()

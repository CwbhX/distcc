#!/usr/bin/env python3
"""Exercise mirror-sync remote shell selection without a remote host."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


DISTCC = Path(os.environ.get('DISTCC_TEST_BINARY', './distcc')).resolve()


class MirrorSyncRemoteShellTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.log = self.base / 'calls.jsonl'
        self.root = self.base / "source tree's files"
        self.root.mkdir()
        self.env = os.environ.copy()
        for key in list(self.env):
            if key.startswith('DISTCC_'):
                del self.env[key]
        self.env.update(PATH=str(self.base) + os.pathsep + self.env['PATH'],
                        DISTCC_MIRROR_ROOTS=str(self.root), CALL_LOG=str(self.log))
        for name in ('ssh', 'custom rsh', 'rsync'):
            script = self.base / name
            script.write_text('#!' + os.sys.executable + '\n'
                              'import json, os, sys\n'
                              'with open(os.environ["CALL_LOG"], "a") as f:\n'
                              '    f.write(json.dumps(sys.argv) + "\\n")\n')
            script.chmod(0o755)

    def run_sync(self):
        return subprocess.run([str(DISTCC), '--mirror-sync', 'mirror-host'],
                              env=self.env, capture_output=True, text=True)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_custom_shell_options_and_remote_path_quoting(self):
        # ssh must never be reached when the custom transport is selected.
        (self.base / 'ssh').write_text('#!/bin/sh\nexit 73\n')
        command = '"{}" -p 2222 -i "identity file"'.format(self.base / 'custom rsh')
        self.env['DISTCC_MIRROR_RSH'] = command
        result = self.run_sync()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0][1:7],
                         ['-p', '2222', '-i', 'identity file', 'mirror-host', 'mkdir'])
        self.assertEqual(shlex.split(' '.join(calls[0][6:])),
                         ['mkdir', '-p', str(self.root)])
        self.assertEqual(calls[1][calls[1].index('-e') + 1], command)

    def test_doubled_quotes_in_shell_options(self):
        self.env['DISTCC_MIRROR_RSH'] = 'ssh -i "identity ""quoted"" file"'
        result = self.run_sync()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0][1:3], ['-i', 'identity "quoted" file'])

    def test_default_ssh(self):
        result = self.run_sync()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertEqual(Path(calls[0][0]).name, 'ssh')
        self.assertNotIn('-e', calls[1])

    def test_unmatched_quote_rejected_before_execution(self):
        self.env['DISTCC_MIRROR_RSH'] = 'ssh -i "unterminated'
        result = self.run_sync()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unmatched quote', result.stderr)
        self.assertFalse(self.log.exists())


if __name__ == '__main__':
    unittest.main()

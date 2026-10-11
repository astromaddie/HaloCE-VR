"""Exercise the SSH-only deployment path without contacting a Frame."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / "tools" / "steam_frame" / "deploy.sh"


class DeployScriptTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stub_bin = self.root / "bin"
        self.stub_bin.mkdir()
        self.call_log = self.root / "calls.txt"

    def stub(self, name, body):
        path = self.stub_bin / name
        path.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8")
        path.chmod(0o755)
        return path

    def env(self, frame_host):
        env = os.environ.copy()
        env.update(
            {
                "PATH": f"{self.stub_bin}{os.pathsep}{env['PATH']}",
                "CALL_LOG": str(self.call_log),
                "ADB": str(self.stub_bin / "adb"),
            }
        )
        if frame_host is None:
            env.pop("FRAME_HOST", None)
        else:
            env["FRAME_HOST"] = frame_host
        return env

    def run_deploy(self, env, *args):
        return subprocess.run(
            ["bash", str(DEPLOY), *args],
            cwd=ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_no_adb_updates_title_and_maps_over_stubbed_ssh(self):
        """`--no-adb` still copies persistent files and never invokes ADB."""
        append = 'printf "%s\\n" "$0 $*" >> "$CALL_LOG"\n'
        for name in ("ssh", "rsync", "scp"):
            self.stub(name, append + "exit 0\n")
        self.stub("adb", append + "exit 99\n")
        maps = self.root / "maps"
        maps.mkdir()
        (maps / "ui.map").write_bytes(b"test map marker")

        result = self.run_deploy(
            self.env("steamos@frame.invalid"), "--maps", str(maps), "--no-adb"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.call_log.read_text(encoding="utf-8")
        self.assertIn("rsync -a --partial", calls)
        self.assertIn("HaloCE-VR.apk", calls)
        self.assertFalse(
            any(Path(line.partition(" ")[0]).name == "adb" for line in calls.splitlines())
        )

    def test_deploy_requires_an_explicit_frame_ssh_target(self):
        """The helper must not fall back to the author's private headset address."""
        for name in ("ssh", "rsync", "scp", "adb"):
            self.stub(name, 'printf "%s\\n" "$0 $*" >> "$CALL_LOG"\nexit 0\n')
        maps = self.root / "maps"
        maps.mkdir()
        (maps / "ui.map").write_bytes(b"test map marker")

        result = self.run_deploy(
            self.env(None), "--maps", str(maps), "--no-adb"
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Set FRAME_HOST", result.stderr)
        self.assertFalse(self.call_log.exists())


if __name__ == "__main__":
    unittest.main()

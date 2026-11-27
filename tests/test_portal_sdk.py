import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RECIPE = Path(__file__).resolve().parents[1] / "src/humblenet/webrtc/portal-sdk"


class PortalSdkTests(unittest.TestCase):
    def test_locked_patch_and_header(self):
        lock = json.loads((RECIPE / "inputs.lock.json").read_text())
        for name, key in (
            ("sdk-tx-ownership.patch", "patch_sha256"),
            ("portal_tx_snapshot_v2.h", "public_header_sha256"),
        ):
            self.assertEqual(hashlib.sha256((RECIPE / name).read_bytes()).hexdigest(), lock[key])

    def test_rejects_unpinned_compiler_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = subprocess.run(
                [sys.executable, str(RECIPE / "build.py"),
                 "--sdk-root", str(root / "sdk"),
                 "--source-root", str(root / "sources"),
                 "--output-dir", str(root / "output"),
                 "--cxx", sys.executable, "--ar", sys.executable],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Pinned input mismatch:", result.stderr)
            self.assertFalse((root / "output").exists())

    def test_rejects_output_inside_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = subprocess.run(
                [sys.executable, str(RECIPE / "build.py"),
                 "--sdk-root", str(root / "sdk"),
                 "--source-root", str(root / "sources"),
                 "--output-dir", str(root / "sdk/output"),
                 "--cxx", sys.executable, "--ar", sys.executable],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Output must be fresh and separate", result.stderr)
            self.assertFalse((root / "sdk").exists())


if __name__ == "__main__":
    unittest.main()

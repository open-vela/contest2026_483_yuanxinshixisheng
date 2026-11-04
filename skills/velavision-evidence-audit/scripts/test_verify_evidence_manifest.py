"""Contract tests use temporary synthetic data, never real firmware."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from verify_evidence_manifest import verify


class ManifestChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.payload = self.root / "receipt.json"
        self.payload.write_bytes(b'{"accepted":true}\n')
        self.item = {"path": "receipt.json", "sha256": hashlib.sha256(self.payload.read_bytes()).hexdigest(), "bytes": self.payload.stat().st_size}

    def check(self, item=None):
        return verify(self.root, {"schema_version": 1, "files": [item or self.item]})

    def test_matching_identity_and_no_rewrite(self):
        original = self.payload.stat().st_mtime_ns
        code, report = self.check()
        self.assertEqual(code, 0)
        self.assertTrue(report["passed"])
        self.assertEqual(self.payload.stat().st_mtime_ns, original)

    def test_tamper_rejected(self):
        self.payload.write_bytes(b'{"accepted":false}\n')
        self.assertEqual(self.check()[0], 1)

    def test_missing_file_rejected(self):
        self.payload.unlink()
        self.assertEqual(self.check()[0], 1)

    def test_wrong_size_rejected(self):
        self.assertEqual(self.check(dict(self.item, bytes=1))[0], 1)

    def test_traversal_and_absolute_rejected(self):
        for name in ("../receipt.json", "/receipt.json", "C:/receipt.json", "x\\receipt.json"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.check(dict(self.item, path=name))

    def test_duplicate_paths_rejected(self):
        with self.assertRaises(ValueError):
            verify(self.root, {"schema_version": 1, "files": [self.item, self.item]})

    def test_invalid_digest_rejected(self):
        with self.assertRaises(ValueError):
            self.check(dict(self.item, sha256="not-a-digest"))

    def test_mutation_during_read_rejected(self):
        with patch("verify_evidence_manifest.stamp", side_effect=[(1, 1, 1, 1), (1, 1, 1, 2)]):
            self.assertEqual(self.check()[0], 1)

    def test_cli_positive_tamper_and_bad_input(self):
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps({"schema_version": 1, "files": [self.item]}), encoding="utf-8")
        command = [sys.executable, "-B", str(Path(__file__).with_name("verify_evidence_manifest.py")), "--root", str(self.root), "--manifest", str(manifest)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        self.payload.write_bytes(b"PRIVATE_SYNTHETIC_DO_NOT_ECHO")
        failed = subprocess.run(command, capture_output=True)
        self.assertEqual(failed.returncode, 1)
        self.assertNotIn(b"PRIVATE_SYNTHETIC_DO_NOT_ECHO", failed.stdout + failed.stderr)
        manifest.write_text("[]", encoding="utf-8")
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()

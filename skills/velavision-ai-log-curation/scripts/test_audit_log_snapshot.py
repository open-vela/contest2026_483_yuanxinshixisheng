"""Synthetic positive/negative snapshot checks; no live collector is touched."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from audit_log_snapshot import audit, AuditError

SID = "00000000-0000-0000-0000-000000000001"


class SnapshotChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scope = {"schema_version": 1, "member": "example", "team_id": "team", "tool": "codex", "session_ids": [SID], "require_partial_disclosure": True}
        self.relative = "logs/example/2026-11-03/codex__" + SID + ".jsonl"
        self.path = self.root / self.relative
        self.path.parent.mkdir(parents=True)
        self.events = [{"session_id": SID, "tool": "codex", "team_id": "team", "github_login": "example", "seq": seq} for seq in (0, 1)]
        self.item = {"session_id": SID, "tool": "codex", "collection_mode": "vscode_extension_partial", "health": "degraded", "data_completeness_warning": "Source incomplete", "files": []}
        self.manifest = {"team_id": "team", "github_login": "example", "sessions": [self.item]}
        self.manifest_path = self.root / "logs/example/manifest.json"
        self.write_events()

    def save(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def write_events(self):
        blob = b"".join((json.dumps(event) + "\n").encode() for event in self.events)
        self.path.write_bytes(blob)
        digest = hashlib.sha256(blob).hexdigest()
        self.item.update(event_count=len(self.events), sha256=digest, files=[{"file_path": self.relative, "sha256": digest, "event_count": len(self.events)}])
        self.save()

    def test_positive_remains_explicitly_partial(self):
        code, report = audit(self.root, self.scope)
        self.assertEqual(code, 0)
        self.assertFalse(report["source_completeness_proven"])
        self.assertFalse(report["atomic_snapshot_proven"])
        self.assertFalse(report["official_schema_validation_run"])

    def test_tampered_partition_rejected(self):
        self.path.write_bytes(self.path.read_bytes() + b"\n")
        self.assertEqual(audit(self.root, self.scope)[0], 1)

    def test_missing_selected_session_rejected(self):
        self.manifest["sessions"] = []
        self.save()
        self.assertEqual(audit(self.root, self.scope)[0], 1)

    def test_missing_partition_rejected(self):
        self.path.unlink()
        with self.assertRaises(AuditError):
            audit(self.root, self.scope)

    def test_invalid_event_shape_rejected(self):
        self.events[0] = ["not-an-event"]
        self.write_events()
        self.assertEqual(audit(self.root, self.scope)[0], 1)

    def test_untargeted_session_is_retained(self):
        self.manifest["sessions"].append({"session_id": "other", "tool": "codex"})
        self.save()
        before = self.manifest_path.read_bytes()
        self.assertEqual(audit(self.root, self.scope)[0], 0)
        self.assertEqual(before, self.manifest_path.read_bytes())

    def test_partial_warning_required(self):
        del self.item["data_completeness_warning"]
        self.save()
        self.assertEqual(audit(self.root, self.scope)[0], 1)

    def test_unproven_upgrade_to_full_rejected(self):
        self.item.update(collection_mode="cli", health="ok")
        self.save()
        self.assertEqual(audit(self.root, self.scope)[0], 1)

    def test_duplicate_seq_rejected_even_with_updated_hash(self):
        self.events[1]["seq"] = 0
        self.write_events()
        self.assertEqual(audit(self.root, self.scope)[0], 1)

    def test_wrong_identity_rejected_even_with_updated_hash(self):
        self.events[0]["team_id"] = "another-team"
        self.write_events()
        self.assertEqual(audit(self.root, self.scope)[0], 1)

    def test_traversal_rejected(self):
        self.item["files"][0]["file_path"] = "../private.jsonl"
        self.save()
        with self.assertRaises(ValueError):
            audit(self.root, self.scope)

    def test_validator_tamper_rejected(self):
        path = self.root / "validator.py"
        path.write_bytes(b"original")
        scope = copy.deepcopy(self.scope)
        scope["official_validator_pins"] = [{"path": "validator.py", "sha256": hashlib.sha256(b"other").hexdigest()}]
        self.assertEqual(audit(self.root, scope)[0], 1)

    def test_unstable_read_rejected(self):
        with patch("audit_log_snapshot.stamp", side_effect=[(1, 1, 1, 1), (1, 1, 1, 2)]):
            with self.assertRaises(AuditError):
                audit(self.root, self.scope)

    def test_cli_positive_tamper_and_bad_input(self):
        scope_path = self.root / "scope.json"
        scope_path.write_text(json.dumps(self.scope), encoding="utf-8")
        command = [sys.executable, "-B", str(Path(__file__).with_name("audit_log_snapshot.py")), "--root", str(self.root), "--scope", str(scope_path)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        self.events[0]["text"] = "PRIVATE_SYNTHETIC_DO_NOT_ECHO"
        self.write_events()
        self.path.write_bytes(self.path.read_bytes() + b"\n")
        failed = subprocess.run(command, capture_output=True)
        self.assertEqual(failed.returncode, 1)
        self.assertNotIn(b"PRIVATE_SYNTHETIC_DO_NOT_ECHO", failed.stdout + failed.stderr)
        scope_path.write_text("[]", encoding="utf-8")
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()

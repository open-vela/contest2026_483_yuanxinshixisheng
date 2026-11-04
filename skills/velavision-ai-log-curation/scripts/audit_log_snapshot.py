"""Read-only supplementary checks for a stable exported-log project snapshot."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re


class AuditError(Exception):
    pass


def safe_path(root: Path, value: str, prefix: str | None = None) -> Path:
    if not isinstance(value, str) or not value or "\\" in value:
        raise ValueError("invalid_relative_path")
    rel = PurePosixPath(value)
    if rel.is_absolute() or PureWindowsPath(value).drive or ".." in rel.parts:
        raise ValueError("path_outside_root")
    if prefix and not value.startswith(prefix + "/"):
        raise ValueError("outside_member_logs")
    path = (root / value).resolve()
    if not path.is_relative_to(root):
        raise ValueError("path_outside_root")
    return path


def stamp(path: Path) -> tuple[int, int, int, int]:
    value = path.stat()
    return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns


def audit(root: Path, scope: dict) -> tuple[int, dict]:
    root = root.resolve(strict=True)
    if not isinstance(scope, dict):
        raise ValueError("invalid_scope")
    ids = scope.get("session_ids")
    if scope.get("schema_version") != 1 or not isinstance(ids, list) or not ids or len(set(ids)) != len(ids):
        raise ValueError("invalid_scope")
    if any(not isinstance(sid, str) or not re.fullmatch(r"[0-9a-fA-F-]{36}", sid) for sid in ids):
        raise ValueError("invalid_session_id")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", scope["member"]):
        raise ValueError("invalid_member")
    prefix = "logs/" + scope["member"]
    tracked = {}

    def read(path: Path) -> bytes:
        try:
            before = stamp(path)
            blob = path.read_bytes()
            if stamp(path) != before:
                raise AuditError("snapshot_changed")
        except OSError:
            raise AuditError("missing_or_unreadable_artifact") from None
        tracked[path] = before
        return blob

    rows, problems = [], []
    manifest_path = safe_path(root, prefix + "/manifest.json")
    original = read(manifest_path)
    manifest = json.loads(original)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("sessions"), list) or any(not isinstance(item, dict) for item in manifest["sessions"]):
        raise ValueError("invalid_manifest")
    if manifest.get("team_id") != scope["team_id"] or manifest.get("github_login") != scope["member"]:
        raise AuditError("manifest_identity_mismatch")
    for pin in scope.get("official_validator_pins", []):
        if not re.fullmatch(r"[0-9a-fA-F]{64}", pin.get("sha256", "")):
            raise ValueError("invalid_validator_pin")
        if hashlib.sha256(read(safe_path(root, pin["path"]))).hexdigest() != pin["sha256"].lower():
            problems.append("official_validator_pin_mismatch")
    for sid in ids:
        matches = [s for s in manifest["sessions"] if s.get("session_id") == sid and s.get("tool") == scope["tool"]]
        if len(matches) != 1:
            problems.append("selected_session_missing_or_ambiguous")
            continue
        item = matches[0]
        mode, health = item.get("collection_mode", ""), item.get("health", "")
        warning = item.get("data_completeness_warning", "")
        if ("partial" in mode or health in ("degraded", "error")) and not (isinstance(warning, str) and warning.strip()):
            problems.append("missing_completeness_warning")
        if scope.get("require_partial_disclosure") and ("partial" not in mode or health != "degraded"):
            problems.append("partial_disclosure_removed")
        files = item.get("files")
        if not isinstance(files, list) or not files:
            problems.append("selected_session_has_no_file_inventory")
            continue
        total, previous, joined, seen = 0, None, hashlib.sha256(), set()
        for part in files:
            relative = part["file_path"]
            path = safe_path(root, relative, prefix)
            if path in seen or path.name != scope["tool"] + "__" + sid + ".jsonl":
                raise AuditError("file_identity_or_duplicate_mismatch")
            seen.add(path)
            blob = read(path)
            if hashlib.sha256(blob).hexdigest() != part.get("sha256"):
                problems.append("partition_sha256_mismatch")
            joined.update(blob)
            lines = blob.splitlines()
            if len(lines) != part.get("event_count"):
                problems.append("partition_event_count_mismatch")
            for line in lines:
                try:
                    event = json.loads(line)
                except ValueError:
                    problems.append("invalid_jsonl_record")
                    continue
                if not isinstance(event, dict):
                    problems.append("invalid_event_shape")
                    continue
                if any(event.get(key) != expected for key, expected in (("session_id", sid), ("tool", scope["tool"]), ("team_id", scope["team_id"]), ("github_login", scope["member"]))):
                    problems.append("event_identity_mismatch")
                seq = event.get("seq")
                if type(seq) is not int or (previous is not None and seq <= previous):
                    problems.append("nonmonotonic_seq")
                if type(seq) is int:
                    previous = seq
            total += len(lines)
        if total != item.get("event_count") or joined.hexdigest() != item.get("sha256"):
            problems.append("session_count_or_sha256_mismatch")
        rows.append({"session_id": sid, "files": len(files), "events": total, "collection_mode": mode, "health": health, "completeness_warning_present": bool(warning)})
    try:
        if manifest_path.read_bytes() != original or any(stamp(path) != value for path, value in tracked.items()):
            problems.append("snapshot_changed")
    except OSError:
        problems.append("snapshot_changed")
    return (1 if problems else 0), {"passed": not problems, "read_only": True, "observed_files_unchanged": "snapshot_changed" not in problems, "atomic_snapshot_proven": False, "source_completeness_proven": False, "official_schema_validation_run": False, "manifest_sha256": hashlib.sha256(original).hexdigest(), "selected_sessions": rows, "problems": sorted(set(problems))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--scope", type=Path, required=True)
    args = parser.parse_args()
    try:
        raw = args.scope.read_bytes()
        code, report = audit(args.root, json.loads(raw))
        if args.scope.read_bytes() != raw:
            raise AuditError("scope_changed")
    except AuditError as error:
        code, report = 1, {"passed": False, "reason": str(error)}
    except (OSError, ValueError, KeyError, TypeError):
        code, report = 2, {"passed": False, "reason": "invalid_or_unreadable_input"}
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())

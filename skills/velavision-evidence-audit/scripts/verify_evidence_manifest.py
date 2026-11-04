"""Offline, read-only file identity checks. No payloads or absolute paths in output."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re


def local_file(root: Path, value: str) -> Path:
    if not isinstance(value, str) or not value or "\\" in value:
        raise ValueError("invalid_relative_path")
    rel = PurePosixPath(value)
    if rel.is_absolute() or PureWindowsPath(value).drive or ".." in rel.parts:
        raise ValueError("path_outside_root")
    target = (root / value).resolve()
    if not target.is_relative_to(root):
        raise ValueError("path_outside_root")
    return target


def stamp(path: Path) -> tuple[int, int, int, int]:
    stat = path.stat()
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def verify(root: Path, manifest: dict) -> tuple[int, dict]:
    root = root.resolve(strict=True)
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1 or not isinstance(manifest.get("files"), list) or not manifest["files"]:
        raise ValueError("invalid_manifest")
    planned, seen = [], set()
    for item in manifest["files"]:
        if not isinstance(item, dict):
            raise ValueError("invalid_file_item")
        path = local_file(root, item["path"])
        key = str(path).casefold()
        if key in seen or not re.fullmatch(r"[0-9a-fA-F]{64}", item.get("sha256", "")):
            raise ValueError("duplicate_path_or_invalid_sha256")
        size = item.get("bytes")
        if size is not None and (type(size) is not int or size < 0):
            raise ValueError("invalid_size")
        seen.add(key)
        planned.append((item, path))
    checks = []
    for item, path in planned:
        row = {"path": item["path"], "passed": False}
        try:
            before = stamp(path)
            if not path.is_file():
                raise OSError("not_regular_file")
            digest, size = hashlib.sha256(), 0
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
                    size += len(chunk)
            actual, stable = digest.hexdigest(), stamp(path) == before
            row.update(bytes=size, sha256=actual, stable_read=stable)
            row["passed"] = stable and actual == item["sha256"].lower() and ("bytes" not in item or size == item["bytes"])
            if not row["passed"]:
                row["reason"] = "changed_during_read" if not stable else "identity_mismatch"
        except OSError:
            row["reason"] = "missing_or_unreadable"
        checks.append(row)
    passed = all(row["passed"] for row in checks)
    return (0 if passed else 1), {"passed": passed, "read_only": True, "coverage": "listed_file_identity_only", "files_checked": len(checks), "checks": checks}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        raw = args.manifest.read_bytes()
        code, report = verify(args.root, json.loads(raw))
        if args.manifest.read_bytes() != raw:
            code, report = 1, {"passed": False, "reason": "manifest_changed_during_audit"}
    except (OSError, ValueError, KeyError, TypeError):
        code, report = 2, {"passed": False, "reason": "invalid_or_unreadable_input"}
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())

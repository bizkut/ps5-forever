#!/usr/bin/env python3
"""Verify repository-contained miniz bytes against their pinned provenance."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent / "third_party/miniz"
record = json.loads((root / "SOURCE.json").read_text())
expected = record.get("files", {})
actual = {path.name for path in root.iterdir() if path.is_file() and path.name != "SOURCE.json"}
if set(expected) != actual:
    raise SystemExit(
        f"miniz source inventory differs: added={sorted(actual-set(expected))} "
        f"missing={sorted(set(expected)-actual)}"
    )
for name, digest in expected.items():
    observed = hashlib.sha256((root / name).read_bytes()).hexdigest()
    if observed != digest:
        raise SystemExit(f"miniz source digest differs: {name}")
print(f"ADDON_WORKER_MINIZ_VERIFIED files={len(expected)} commit={record['commit']}")

#!/usr/bin/env python3
"""Fail closed if the lock file is incomplete or a SHA-256 is malformed."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

SHA256 = re.compile(r"^[0-9a-f]{64}$")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: scan_lock.py dependencies.lock.json", file=sys.stderr)
        return 2
    lock = json.loads(Path(sys.argv[1]).read_text())
    fetched = lock.get("fetched")
    if not isinstance(fetched, list) or not fetched:
        print("dependencies.lock.json has no fetched entries", file=sys.stderr)
        return 1
    for entry in fetched:
        for field in ("name", "version", "url", "sha256"):
            if field not in entry:
                print(f"missing {field} in {entry}", file=sys.stderr)
                return 1
        if not SHA256.match(entry["sha256"]):
            print(f"invalid sha256 for {entry['name']}", file=sys.stderr)
            return 1
        if "github.com" in entry["url"] and "/archive/refs/tags/" not in entry["url"]:
            print(f"{entry['name']} is not pinned to a tagged archive",
                  file=sys.stderr)
            return 1
    print(f"scanned {len(fetched)} pinned dependencies")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

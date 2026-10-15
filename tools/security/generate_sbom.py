#!/usr/bin/env python3
"""Emit a CycloneDX-like SBOM from the pinned dependency lock file."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: generate_sbom.py dependencies.lock.json sbom.json",
              file=sys.stderr)
        return 2
    lock_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    lock = json.loads(lock_path.read_text())
    components = []
    for entry in lock.get("fetched", []):
        components.append({
            "type": "library",
            "name": entry["name"],
            "version": entry.get("version", ""),
            "purl": f"pkg:generic/{entry['name']}@{entry.get('version', '')}",
            "hashes": [{"alg": "SHA-256", "content": entry["sha256"]}],
            "externalReferences": [{"type": "distribution", "url": entry["url"]}],
        })
    for entry in lock.get("system", []):
        components.append({
            "type": "library",
            "name": entry["name"],
            "version": entry.get("minimum_version", entry.get("ci_version", "")),
            "description": entry.get("notes", "system dependency"),
        })
    document = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "LanAudio",
                "version": "0.1.0",
            }
        },
        "components": components,
    }
    output_path.write_text(json.dumps(document, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

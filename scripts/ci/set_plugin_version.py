#!/usr/bin/env python3
"""Set plugin.json version from a git tag (e.g. v1.0.0 -> 1.0.0)."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: set_plugin_version.py <tag>", file=sys.stderr)
        return 2

    tag = sys.argv[1].strip()
    version = re.sub(r"^v", "", tag, count=1)
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[\w.]+)?", version):
        print(f"invalid semver tag: {tag!r}", file=sys.stderr)
        return 2

    manifest = Path(__file__).resolve().parents[2] / "plugin.json"
    data = json.loads(manifest.read_text(encoding="utf-8"))
    data["version"] = version
    manifest.write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"plugin.json version -> {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Pulls contracts/device-api/*.schema.json from the AgrumyService repo (the generated source
of truth) into this repo's copy, and stamps the source commit into contracts/device-api/README.md.

  python tools/contract-check/sync_schemas.py                 # from GitHub, branch master
  python tools/contract-check/sync_schemas.py --ref <sha|branch>
  python tools/contract-check/sync_schemas.py --dest <dir>    # download somewhere else, README untouched (CI)
  python tools/contract-check/sync_schemas.py --from ../AgrumyService/contracts/device-api   # local checkout instead of GitHub

Exit code 0 always on success; run check_contract.py afterwards - a new upstream field the
firmware does not send/read yet is exactly the drift this exists to surface.
"""
from __future__ import annotations

import json
import re
import shutil
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from firmware_fields import CONTRACT  # noqa: E402

REPO = "dopiskur/AgrumyService"
DEFAULT_DEST = HERE.parent.parent / "contracts" / "device-api"


def arg(name: str, default: str | None = None) -> str | None:
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def resolve_sha(ref: str) -> str:
    with urllib.request.urlopen(f"https://api.github.com/repos/{REPO}/commits/{ref}", timeout=30) as r:
        return json.load(r)["sha"]


def main() -> int:
    ref = arg("--ref", "master")
    dest = Path(arg("--dest", str(DEFAULT_DEST)))
    local = arg("--from")
    dest.mkdir(parents=True, exist_ok=True)

    if local:
        src = Path(local)
        for name in CONTRACT:
            shutil.copyfile(src / name, dest / name)
        stamp = f"local copy of {src}"
    else:
        sha = resolve_sha(ref)
        for name in CONTRACT:
            url = f"https://raw.githubusercontent.com/{REPO}/{sha}/contracts/device-api/{name}"
            with urllib.request.urlopen(url, timeout=30) as r:
                (dest / name).write_bytes(r.read())
        stamp = sha[:7]
    print(f"synced {len(CONTRACT)} schemas from {stamp} into {dest}")

    readme = DEFAULT_DEST / "README.md"
    if dest == DEFAULT_DEST and not local and readme.exists():
        text = readme.read_text(encoding="utf-8")
        new = re.sub(r"^> \*\*Copied from commit:\*\* `[0-9a-f]+`.*$",
                     f"> **Copied from commit:** `{stamp}` (`{ref}`) - synced by tools/contract-check/sync_schemas.py",
                     text, count=1, flags=re.M)
        if new != text:
            readme.write_text(new, encoding="utf-8")
            print("README source-commit line updated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

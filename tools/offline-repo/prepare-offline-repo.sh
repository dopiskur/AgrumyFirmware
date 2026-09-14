#!/usr/bin/env bash
# Roadmap #94 (part C2): prepare an offline firmware repository (typically a USB stick) on an
# INTERNET-CONNECTED machine, to be imported on an offline Agrumy server via the Firmware page's
# "Import from a directory on the server" (#94-2b). Fallback for browsers without the File System
# Access API (Safari/Firefox) and for scripted provisioning - the browser "Build offline repo"
# button on the Firmware page is the primary, nicer path.
#
# Output layout (the exact contract the API's import scanner expects):
#   <target>/agrumy-<board>-v<version>.bin   one per board per release
#   <target>/manifest.json                    schemaVersion 1, SHA-256 per file (no url field -
#                                             files sit next to the manifest)
#
# Usage:
#   prepare-offline-repo.sh <target-dir> [--repo owner/name] [--limit N] [--token TOKEN]
#     --repo   GitHub repository (default dopiskur/AgrumyFirmware)
#     --limit  only the N newest releases (default: all)
#     --token  GitHub API token (optional; public repos need none)
#
# Needs: bash, curl, python3 (JSON + SHA-256; present on macOS and every mainstream Linux).
set -euo pipefail

TARGET="${1:-}"
REPO="dopiskur/AgrumyFirmware"
LIMIT=0
TOKEN="${GITHUB_TOKEN:-}"
shift || true
while [ $# -gt 0 ]; do
  case "$1" in
    --repo)  REPO="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --token) TOKEN="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
if [ -z "$TARGET" ]; then
  echo "usage: $0 <target-dir> [--repo owner/name] [--limit N] [--token TOKEN]" >&2
  exit 2
fi
mkdir -p "$TARGET"

AUTH=()
if [ -n "$TOKEN" ]; then AUTH=(-H "Authorization: Bearer $TOKEN"); fi

echo "Reading releases of $REPO ..."
RELEASES_JSON="$(curl -fsSL "${AUTH[@]}" -H "Accept: application/vnd.github+json" -H "User-Agent: agrumy-offline-repo" \
  "https://api.github.com/repos/$REPO/releases?per_page=100")"

# python3 does the JSON work and drives the downloads so the file-name convention and checksum
# logic live in exactly one place - the same regex the API applies on import.
TARGET="$TARGET" REPO="$REPO" LIMIT="$LIMIT" TOKEN="$TOKEN" python3 - "$RELEASES_JSON" <<'PY'
import datetime, hashlib, json, os, re, subprocess, sys

releases = json.loads(sys.argv[1])
target, repo, limit, token = os.environ["TARGET"], os.environ["REPO"], int(os.environ["LIMIT"]), os.environ["TOKEN"]
# Board allows '-'/'_' (kc868-a6, seeed_xiao_esp32c3); the (?<!-full) lookbehind keeps a full-image
# filename (agrumy-<board>-full-v<version>.bin) from being swallowed as board="<board>-full" - this
# script only ever wants the OTA .bin, never the full image.
pattern = re.compile(r"^agrumy-(?P<board>[a-z0-9_-]+)(?<!-full)-v(?P<version>\d+\.\d+\.\d+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?)\.bin$")

manifest = {"schemaVersion": 1, "generatedAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "source": f"github:{repo}", "releases": []}
count = 0
for rel in releases:
    if rel.get("draft"):
        continue
    if limit and count >= limit:
        break
    files = []
    for asset in rel.get("assets", []):
        m = pattern.match(asset["name"])
        if not m:
            continue
        path = os.path.join(target, asset["name"])
        print(f"  {asset['name']}")
        cmd = ["curl", "-fsSL", "-H", "User-Agent: agrumy-offline-repo", "-o", path, asset["browser_download_url"]]
        if token:
            cmd[1:1] = ["-H", f"Authorization: Bearer {token}"]
        subprocess.check_call(cmd)
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        files.append({"board": m.group("board"), "fileName": asset["name"],
                      "sizeBytes": os.path.getsize(path), "sha256": digest, "url": None})
    if not files:
        continue
    version = rel["tag_name"][1:] if rel["tag_name"].startswith("v") else rel["tag_name"]
    manifest["releases"].append({"version": version, "publishedAt": rel.get("published_at"), "files": files})
    count += 1

with open(os.path.join(target, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=2)
print(f"Done: {sum(len(r['files']) for r in manifest['releases'])} file(s) from {count} release(s) -> {target}/manifest.json")
PY

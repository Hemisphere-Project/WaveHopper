#!/usr/bin/env python3
"""Package compiled M5 firmware binaries into publishable OTA releases.

One firmware channel per board (docs/CONTENT-API.md):
  m5cores3  CoreS3 / CoreS3 SE (ESP32-S3)  players/m5cores3/
  m5fire    Fire (classic ESP32 + PSRAM)    players/m5cores3/fire/
Both build from the same sources and share WH_FW_VERSION / WH_FW_BUILD
(players/m5cores3/wh-common.ini).

For each board: copies the built firmware.bin into the web docroot under a
versioned name, computes sha256 + size, and rewrites that board's firmware
manifest LAST (the manifest is the publish switch). Older .bin files of that
board are pruned so only the current release ships (deploys are git pulls).

Refuses a binary whose image header names another chip, or one older than
wh-common.ini (= built before the version bump).

Does NOT compile — build first:
  cd players/m5cores3 && pio run -e m5stack-cores3
  cd players/m5cores3/fire && pio run
Then:
  python3 tools/release-m5.py --board all      # or m5cores3 / m5fire
  git add -A && git commit && git push          # webhook deploys it

Devices update when the published `build` exceeds their compiled WH_FW_BUILD.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
FW_DIR = ROOT / 'players' / 'm5cores3'
VERSION_INI = FW_DIR / 'wh-common.ini'
HOST = 'https://waverz.net'

# board id -> (built binary, build command, ESP image header chip id)
BOARDS = {
    'm5cores3': (FW_DIR / '.pio' / 'build' / 'm5stack-cores3' / 'firmware.bin',
                 'cd players/m5cores3 && pio run -e m5stack-cores3', 9),
    'm5fire': (FW_DIR / 'fire' / '.pio' / 'build' / 'm5stack-fire' / 'firmware.bin',
               'cd players/m5cores3/fire && pio run', 0),
}


def read_version_build() -> tuple[str, int]:
    text = VERSION_INI.read_text(encoding='utf-8')
    ver = re.search(r'-DWH_FW_VERSION=\\"([^\\"]+)\\"', text)
    build = re.search(r'-DWH_FW_BUILD=(\d+)', text)
    if not ver or not build:
        sys.exit(f'release failed: WH_FW_VERSION / WH_FW_BUILD not found in '
                 f'{VERSION_INI.relative_to(ROOT)}')
    return ver.group(1), int(build.group(1))


def release(board: str, version: str, build: int) -> None:
    binary, build_cmd, chip_id = BOARDS[board]
    if not binary.is_file():
        sys.exit(f'release failed: {binary.relative_to(ROOT)} not found — build first ({build_cmd})')
    data = binary.read_bytes()
    if len(data) < 16 or data[0] != 0xE9 or int.from_bytes(data[12:14], 'little') != chip_id:
        sys.exit(f'release failed: {binary.relative_to(ROOT)} is not an image for {board} '
                 f'(header chip id {int.from_bytes(data[12:14], "little")}, want {chip_id})')
    if binary.stat().st_mtime < VERSION_INI.stat().st_mtime:
        sys.exit(f'release failed: {binary.relative_to(ROOT)} is older than '
                 f'{VERSION_INI.relative_to(ROOT)} — rebuild after the version bump ({build_cmd})')

    sha = hashlib.sha256(data).hexdigest()
    name = f'wavehopper-{board}-{version}+{build}.bin'
    out_dir = ROOT / 'players' / 'web' / 'public' / 'content' / 'firmware' / board
    out_dir.mkdir(parents=True, exist_ok=True)
    # Prune older binaries — only the current release is served.
    for old in out_dir.glob('wavehopper-*.bin'):
        if old.name != name:
            old.unlink()
            print(f'{old.relative_to(ROOT)}: removed (old release)')

    (out_dir / name).write_bytes(data)
    print(f'{(out_dir / name).relative_to(ROOT)}: {len(data)} bytes')

    manifest = {
        'schemaVersion': 1,
        'board': board,
        'version': version,
        'build': build,
        'url': f'{HOST}/content/firmware/{board}/{name}',
        'sha256': sha,
        'size': len(data),
    }
    manifest_path = out_dir / 'manifest.json'  # written last — the publish switch
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'{manifest_path.relative_to(ROOT)}: published {version}+{build} (sha {sha[:12]})')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--board', required=True, choices=[*BOARDS, 'all'],
                    help='firmware channel to publish (all = every board)')
    args = ap.parse_args()
    version, build = read_version_build()
    boards = list(BOARDS) if args.board == 'all' else [args.board]
    # Check every binary before writing anything, so "all" never half-publishes
    # because the second board wasn't built.
    for b in boards:
        if not BOARDS[b][0].is_file():
            sys.exit(f'release failed: {BOARDS[b][0].relative_to(ROOT)} not found — '
                     f'build first ({BOARDS[b][1]})')
    for b in boards:
        release(b, version, build)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

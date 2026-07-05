#!/usr/bin/env python3
"""Package a compiled M5 firmware binary into a publishable OTA release.

Reads the version/build the binary was compiled with from platformio.ini,
copies the built firmware.bin into the web docroot under a versioned name,
computes its sha256 + size, and rewrites the firmware manifest LAST (per
docs/CONTENT-API.md — the manifest is the publish switch). Older .bin files
are pruned so only the current release ships (deploys are git pulls).

Does NOT compile — build first:
  cd players/m5cores3 && pio run -e m5stack-cores3
Then:
  python3 tools/release-m5.py
  git add -A && git commit && git push   # webhook/pull deploys it

Devices update when the published `build` exceeds their compiled WH_FW_BUILD.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PIO_INI = ROOT / 'players' / 'm5cores3' / 'platformio.ini'
FIRMWARE_BIN = ROOT / 'players' / 'm5cores3' / '.pio' / 'build' / 'm5stack-cores3' / 'firmware.bin'
OUT_DIR = ROOT / 'players' / 'web' / 'public' / 'content' / 'firmware' / 'm5cores3'
HOST = 'https://waverz.net'
BOARD = 'm5cores3'


def read_version_build() -> tuple[str, int]:
    text = PIO_INI.read_text(encoding='utf-8')
    # The production env's flags; take the first (non-dev) definitions.
    ver = re.search(r'-DWH_FW_VERSION=\\"([^\\"]+)\\"', text)
    build = re.search(r'-DWH_FW_BUILD=(\d+)', text)
    if not ver or not build:
        sys.exit('release failed: WH_FW_VERSION / WH_FW_BUILD not found in platformio.ini')
    return ver.group(1), int(build.group(1))


def main() -> int:
    if not FIRMWARE_BIN.is_file():
        sys.exit(f'release failed: {FIRMWARE_BIN.relative_to(ROOT)} not found — '
                 f'build first (cd players/m5cores3 && pio run -e m5stack-cores3)')

    version, build = read_version_build()
    data = FIRMWARE_BIN.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    name = f'wavehopper-{BOARD}-{version}+{build}.bin'

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    # Prune older binaries — only the current release is served.
    for old in OUT_DIR.glob('wavehopper-*.bin'):
        if old.name != name:
            old.unlink()
            print(f'{old.relative_to(ROOT)}: removed (old release)')

    (OUT_DIR / name).write_bytes(data)
    print(f'{(OUT_DIR / name).relative_to(ROOT)}: {len(data)} bytes')

    manifest = {
        'schemaVersion': 1,
        'board': BOARD,
        'version': version,
        'build': build,
        'url': f'{HOST}/content/firmware/{BOARD}/{name}',
        'sha256': sha,
        'size': len(data),
    }
    manifest_path = OUT_DIR / 'manifest.json'  # written last — the publish switch
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'{manifest_path.relative_to(ROOT)}: published {version}+{build} '
          f'(sha {sha[:12]})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

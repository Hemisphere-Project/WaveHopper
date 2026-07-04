#!/usr/bin/env python3
"""Build WaveHopper artifacts from the content/ sources.

Outputs (all committed, all under the web docroot unless noted):
  players/web/public/stations.json                      web catalog (legacy shape)
  players/web/public/img/stations/*.png                 icon copies for the web app
  players/web/public/content/m5cores3/stations.json     m5 pack: transformed catalog
  players/web/public/content/m5cores3/icons/*.png       m5 pack: 64x64 downscales
  players/web/public/content/m5cores3/manifest.json     m5 pack: content manifest
  players/web/public/content/firmware/m5cores3/manifest.json
                                                        created only if missing,
                                                        NEVER overwritten (it is the
                                                        published firmware pointer)

The pack manifest is content-addressed: contentVersion = sha256 over the
concatenation of "{path}\n{sha256}\n" for `files` sorted by path. When the
recomputed contentVersion equals the on-disk one, nothing is rewritten, so
re-running the build never churns committed artifacts. See docs/CONTENT-API.md
for the schema consumed by devices — changes here must stay in sync with it.

Icon downscaling needs Pillow and imports it lazily: builds that don't change
any icon (the common case) run stdlib-only. Each icon entry in the manifest
carries srcSha256 — the hash of its content/icons/ source at generation time —
which is how staleness is detected without re-encoding. Clients ignore it.

Usage:
  python3 tools/build.py [--seed-m5]

  --seed-m5   also mirror the built pack into players/m5cores3/data/ so
              `pio run -t uploadfs` flashes it as the first-boot content.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from datetime import datetime, timezone
from io import BytesIO
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CONTENT_DIR = ROOT / 'content'
STATIONS_DIR = CONTENT_DIR / 'stations'
ORDER_PATH = CONTENT_DIR / '_order.json'
ICONS_SRC_DIR = CONTENT_DIR / 'icons'

WEB_PUBLIC = ROOT / 'players' / 'web' / 'public'
WEB_STATIONS_PATH = WEB_PUBLIC / 'stations.json'
WEB_ICONS_DIR = WEB_PUBLIC / 'img' / 'stations'

M5_PACK_DIR = WEB_PUBLIC / 'content' / 'm5cores3'
M5_ICON_SIZE = 64
M5_SEED_DIR = ROOT / 'players' / 'm5cores3' / 'data' / 'content' / 'm5cores3'

FIRMWARE_MANIFEST_PATH = WEB_PUBLIC / 'content' / 'firmware' / 'm5cores3' / 'manifest.json'

WEB_ICON_PREFIX = '/img/stations/'


def load_json(path: Path):
    with path.open('r', encoding='utf-8') as handle:
        return json.load(handle)


def dump_json(value) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_if_changed(path: Path, data: bytes) -> bool:
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def build_station_list(stations_dir: Path, order_path: Path) -> list[dict]:
    order = load_json(order_path)
    if not isinstance(order, list) or not all(isinstance(item, str) for item in order):
        raise ValueError('content/_order.json must be a JSON array of station ids')

    ids = sorted(
        path.stem
        for path in stations_dir.glob('*.json')
        if not path.name.startswith('_')
    )

    known = set(order)
    extras = [station_id for station_id in ids if station_id not in known]
    final_ids = [station_id for station_id in order if station_id in ids] + extras

    return [load_json(stations_dir / f'{station_id}.json') for station_id in final_ids]


def build_web(stations: list[dict]) -> None:
    changed = write_if_changed(WEB_STATIONS_PATH, dump_json(stations))
    rel = WEB_STATIONS_PATH.relative_to(ROOT)
    print(f'{rel}: {len(stations)} entries{"" if changed else " (unchanged)"}')

    for src in sorted(ICONS_SRC_DIR.glob('*.png')):
        dest = WEB_ICONS_DIR / src.name
        if write_if_changed(dest, src.read_bytes()):
            print(f'{dest.relative_to(ROOT)}: copied from content/icons/')


def transform_stations_for_m5(stations: list[dict]) -> tuple[list[dict], list[Path]]:
    """Return the m5 view of the catalog plus the icon sources it references.

    Icons are rewritten to pack-relative paths when a local source exists in
    content/icons/, and omitted otherwise — the firmware must never be handed a
    third-party URL (it only talks TLS to its own host).
    """
    transformed: list[dict] = []
    icon_sources: list[Path] = []
    for station in stations:
        entry = dict(station)
        icon = entry.pop('icon', None)
        if isinstance(icon, str) and icon.startswith(WEB_ICON_PREFIX):
            src = ICONS_SRC_DIR / Path(icon).name
            if src.exists():
                entry['icon'] = f'icons/{src.name}'
                icon_sources.append(src)
            else:
                print(f'warning: {entry.get("id")}: no source for {icon} in content/icons/, '
                      f'omitting from m5 pack', file=sys.stderr)
        transformed.append(entry)
    return transformed, icon_sources


def downscale_icon(src: Path) -> bytes:
    from PIL import Image, ImageOps  # lazy: only icon regeneration needs Pillow

    with Image.open(src) as img:
        fitted = ImageOps.contain(img.convert('RGBA'), (M5_ICON_SIZE, M5_ICON_SIZE),
                                  Image.Resampling.LANCZOS)
    if fitted.size != (M5_ICON_SIZE, M5_ICON_SIZE):
        canvas = Image.new('RGBA', (M5_ICON_SIZE, M5_ICON_SIZE), (0, 0, 0, 0))
        canvas.paste(fitted, ((M5_ICON_SIZE - fitted.width) // 2,
                              (M5_ICON_SIZE - fitted.height) // 2))
        fitted = canvas
    out = BytesIO()
    fitted.save(out, format='PNG', optimize=True)
    return out.getvalue()


def build_m5_pack(stations: list[dict]) -> None:
    transformed, icon_sources = transform_stations_for_m5(stations)

    old_manifest = {}
    manifest_path = M5_PACK_DIR / 'manifest.json'
    if manifest_path.exists():
        try:
            old_manifest = load_json(manifest_path)
        except (OSError, ValueError):
            old_manifest = {}
    old_entries = {entry.get('path'): entry for entry in old_manifest.get('files', [])}

    # Decide per icon: reuse the existing pack file (source unchanged) or
    # regenerate. Regeneration is the only step that needs Pillow.
    icon_bytes: dict[str, bytes] = {}
    src_shas: dict[str, str] = {}
    stale: list[tuple[str, Path]] = []
    for src in icon_sources:
        pack_path = f'icons/{src.name}'
        out_path = M5_PACK_DIR / pack_path
        src_shas[pack_path] = sha256_hex(src.read_bytes())
        old = old_entries.get(pack_path)
        if old and old.get('srcSha256') == src_shas[pack_path] and out_path.exists():
            icon_bytes[pack_path] = out_path.read_bytes()
        else:
            stale.append((pack_path, src))

    if stale:
        try:
            import PIL  # noqa: F401
        except ImportError:
            names = ', '.join(path for path, _ in stale)
            sys.exit(f'build failed: {len(stale)} m5 icon(s) need regenerating ({names}) '
                     f'but Pillow is not installed. pip install Pillow, or run with an '
                     f'interpreter that has it (e.g. /usr/bin/python3).')
        for pack_path, src in stale:
            icon_bytes[pack_path] = downscale_icon(src)
            print(f'{(M5_PACK_DIR / pack_path).relative_to(ROOT)}: downscaled to '
                  f'{M5_ICON_SIZE}x{M5_ICON_SIZE}')

    files_bytes: dict[str, bytes] = {'stations.json': dump_json(transformed), **icon_bytes}

    entries = []
    for path in sorted(files_bytes):
        data = files_bytes[path]
        entry = {'path': path, 'sha256': sha256_hex(data), 'size': len(data)}
        if path in src_shas:
            entry['srcSha256'] = src_shas[path]
        entries.append(entry)

    content_version = sha256_hex(
        ''.join(f'{e["path"]}\n{e["sha256"]}\n' for e in entries).encode('utf-8')
    )

    if content_version == old_manifest.get('contentVersion'):
        print(f'{M5_PACK_DIR.relative_to(ROOT)}: unchanged ({content_version[:12]})')
        return

    for path, data in files_bytes.items():
        write_if_changed(M5_PACK_DIR / path, data)
    # Files dropped from the manifest are deleted on devices; mirror that here.
    icons_dir = M5_PACK_DIR / 'icons'
    if icons_dir.is_dir():
        for leftover in icons_dir.glob('*.png'):
            if f'icons/{leftover.name}' not in files_bytes:
                leftover.unlink()
                print(f'{leftover.relative_to(ROOT)}: removed (no longer referenced)')

    manifest = {
        'schemaVersion': 1,
        'contentVersion': content_version,
        'generated': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'totalSize': sum(e['size'] for e in entries),
        'files': entries,
    }
    write_if_changed(manifest_path, dump_json(manifest))
    print(f'{manifest_path.relative_to(ROOT)}: {len(entries)} files, '
          f'contentVersion {content_version[:12]}')


def ensure_firmware_manifest() -> None:
    if FIRMWARE_MANIFEST_PATH.exists():
        return
    placeholder = {
        'schemaVersion': 1,
        'board': 'm5cores3',
        'version': '0.0.0',
        'build': 0,
        'url': None,
        'sha256': None,
        'size': 0,
    }
    write_if_changed(FIRMWARE_MANIFEST_PATH, dump_json(placeholder))
    print(f'{FIRMWARE_MANIFEST_PATH.relative_to(ROOT)}: created placeholder (build 0)')


def seed_m5() -> None:
    if not (M5_PACK_DIR / 'manifest.json').exists():
        sys.exit('build failed: m5 pack missing, cannot seed')
    if M5_SEED_DIR.exists():
        shutil.rmtree(M5_SEED_DIR)
    shutil.copytree(M5_PACK_DIR, M5_SEED_DIR)
    print(f'{M5_SEED_DIR.relative_to(ROOT)}: seeded from pack')


def main() -> int:
    parser = argparse.ArgumentParser(description='Build WaveHopper artifacts from content/')
    parser.add_argument('--seed-m5', action='store_true',
                        help='mirror the built m5 pack into players/m5cores3/data/')
    args = parser.parse_args()

    try:
        stations = build_station_list(STATIONS_DIR, ORDER_PATH)
        build_web(stations)
        build_m5_pack(stations)
        ensure_firmware_manifest()
        if args.seed_m5:
            seed_m5()
    except SystemExit:
        raise
    except Exception as exc:
        print(f'build failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

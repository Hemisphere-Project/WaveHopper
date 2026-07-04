#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
STATIONS_DIR = ROOT / 'stations'
OUTPUT_PATH = ROOT / 'public' / 'stations.json'


def load_json(path: Path):
    with path.open('r', encoding='utf-8') as handle:
        return json.load(handle)


def build_station_list(stations_dir: Path) -> list[object]:
    order = load_json(stations_dir / '_order.json')
    if not isinstance(order, list) or not all(isinstance(item, str) for item in order):
        raise ValueError('stations/_order.json must be a JSON array of station ids')

    ids = sorted(
        path.stem
        for path in stations_dir.glob('*.json')
        if not path.name.startswith('_')
    )

    known = set(order)
    extras = [station_id for station_id in ids if station_id not in known]
    final_ids = [station_id for station_id in order if station_id in ids] + extras

    stations: list[object] = []
    for station_id in final_ids:
        stations.append(load_json(stations_dir / f'{station_id}.json'))
    return stations


def main() -> int:
    try:
        stations = build_station_list(STATIONS_DIR)
        OUTPUT_PATH.write_text(
            json.dumps(stations, indent=2, ensure_ascii=False) + '\n',
            encoding='utf-8',
        )
    except Exception as exc:
        print(f'build failed: {exc}', file=sys.stderr)
        return 1

    print(f'{OUTPUT_PATH.relative_to(ROOT)}: {len(stations)} entries')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

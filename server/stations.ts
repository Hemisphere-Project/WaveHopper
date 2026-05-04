// Builds the merged stations list from per-station JSON files.
// Source of truth: stations/<id>.json + stations/_order.json (deterministic order).
// Files not listed in _order.json are appended alphabetically — guards against forgetting
// to update the order file but keeps known stations in their curated sequence.

import { readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';

export async function buildStationList(stationsDir: string): Promise<unknown[]> {
  const orderRaw = await readFile(join(stationsDir, '_order.json'), 'utf8');
  const order: string[] = JSON.parse(orderRaw);

  const entries = await readdir(stationsDir);
  const ids = entries
    .filter((f) => f.endsWith('.json') && !f.startsWith('_'))
    .map((f) => f.slice(0, -'.json'.length));

  const known = new Set(order);
  const extras = ids.filter((id) => !known.has(id)).sort();
  const final = [...order.filter((id) => ids.includes(id)), ...extras];

  const out: unknown[] = [];
  for (const id of final) {
    const raw = await readFile(join(stationsDir, `${id}.json`), 'utf8');
    out.push(JSON.parse(raw));
  }
  return out;
}

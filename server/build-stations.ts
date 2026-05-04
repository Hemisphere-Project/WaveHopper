// Concatenates stations/<id>.json into public/stations.json — the static artifact
// served by shared hosting / nginx when the dev relay isn't running. public/ is the
// docroot; placing the build there keeps the source tree (server/, stations/, etc.)
// off the public web.

import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { buildStationList } from './stations.ts';

const ROOT = new URL('..', import.meta.url).pathname;
const list = await buildStationList(join(ROOT, 'stations'));
const out = JSON.stringify(list, null, 2) + '\n';
await writeFile(join(ROOT, 'public', 'stations.json'), out);
console.log(`public/stations.json: ${list.length} entries`);

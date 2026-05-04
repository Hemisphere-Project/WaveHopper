// Concatenates stations/<id>.json into a static stations.json at the repo root,
// for static-deploy targets (nginx) that won't run the relay.

import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { buildStationList } from './stations.ts';

const ROOT = new URL('..', import.meta.url).pathname;
const list = await buildStationList(join(ROOT, 'stations'));
const out = JSON.stringify(list, null, 2) + '\n';
await writeFile(join(ROOT, 'stations.json'), out);
console.log(`stations.json: ${list.length} entries`);

// WaveHopper dev server — static files from ../public + stations.json from repo root.
// In production, nginx serves the same paths. No stream proxying: every station's
// upstream is HTTPS with permissive CORS, so the browser plays them directly.

import { file } from 'bun';
import { join, normalize } from 'node:path';

const PORT = Number(Bun.env.PORT ?? 3000);
const ROOT = new URL('..', import.meta.url).pathname;
const PUBLIC = join(ROOT, 'public');

const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.mjs': 'application/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
  '.woff': 'font/woff',
};

function mimeOf(path: string): string {
  const dot = path.lastIndexOf('.');
  return (dot >= 0 && MIME[path.slice(dot)]) || 'application/octet-stream';
}

async function serveFile(absPath: string, headers: Record<string, string> = {}) {
  const f = file(absPath);
  if (!(await f.exists())) return new Response('not found', { status: 404 });
  return new Response(f, {
    headers: { 'Content-Type': mimeOf(absPath), ...headers },
  });
}

Bun.serve({
  port: PORT,
  async fetch(req) {
    const url = new URL(req.url);
    let path = decodeURIComponent(url.pathname);
    if (path === '/') path = '/index.html';

    if (path === '/stations.json') {
      return serveFile(join(ROOT, 'stations.json'), {
        'Cache-Control': 'no-cache',
      });
    }

    const safe = normalize(path).replace(/^(\.\.[/\\])+/, '');
    return serveFile(join(PUBLIC, safe));
  },
});

console.log(`WaveHopper dev server: http://localhost:${PORT}`);

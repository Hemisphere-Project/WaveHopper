// WaveHopper service worker.
// - Pre-caches the app shell so offline launches work and station switching is instant.
// - Stations.json: network-first with cache fallback (so updates flow but offline still loads).
// - Stream URLs are cross-origin and intentionally NOT intercepted — the browser handles
//   them directly. Touching a live audio body in a SW would buffer it in memory and break
//   playback continuity.

const CACHE = 'wh-v2';
const SHELL = [
  '/',
  '/index.html',
  '/style.css',
  '/app.js',
  '/vendor/vt323-latin.woff2',
  '/vendor/hls.light.min.js',
  '/manifest.webmanifest',
  '/icons/favicon/android-chrome-192x192.png',
  '/icons/favicon/android-chrome-512x512.png',
  '/icons/favicon/apple-touch-icon.png',
  '/icons/favicon/favicon-32x32.png',
  '/icons/favicon/favicon.ico',
];
const SHELL_SET = new Set(SHELL);

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    await cache.addAll(SHELL);
  })());
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)));
    await self.clients.claim();
  })());
});

async function staleWhileRevalidate(req) {
  const cache = await caches.open(CACHE);
  const cached = await cache.match(req);
  const fetched = fetch(req).then((res) => {
    if (res && res.ok) cache.put(req, res.clone());
    return res;
  }).catch(() => null);
  return cached || (await fetched) || new Response('offline', { status: 503 });
}

async function networkFirst(req) {
  const cache = await caches.open(CACHE);
  try {
    const res = await fetch(req);
    if (res && res.ok) cache.put(req, res.clone());
    return res;
  } catch {
    const cached = await cache.match(req);
    if (cached) return cached;
    return new Response('offline', { status: 503 });
  }
}

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;  // streams pass through untouched

  if (SHELL_SET.has(url.pathname)) {
    event.respondWith(staleWhileRevalidate(req));
  } else if (url.pathname === '/stations.json') {
    event.respondWith(networkFirst(req));
  }
  // anything else (e.g. dev-only paths, future assets): pass through
});

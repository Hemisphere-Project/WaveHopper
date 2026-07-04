// Waverz·net service worker.
// - Pre-caches the app shell so offline launches work and station switching is instant.
// - Code (HTML/JS/CSS/manifest): network-first so deploys are picked up on the next
//   reload. Cache is the offline fallback only.
// - Static assets (fonts, hls.js, icons): stale-while-revalidate. They're large and
//   change rarely, so serving from cache and refreshing in background is the right
//   trade. They get re-fetched on the next page load if the upstream changed.
// - Stations.json: network-first with cache fallback (so updates flow but offline
//   still loads).
// - Stream URLs are cross-origin and intentionally NOT intercepted — the browser
//   handles them directly. Touching a live audio body in a SW would buffer it in
//   memory and break playback continuity.
//
// Bump APP_VERSION on each deploy that changes the shell so installed PWAs fetch a
// fresh sw.js URL and seed a new cache namespace.

const APP_VERSION = '20260704c';

const CACHE = `wh-${APP_VERSION}`;

// Code paths — always try network first, fall back to cache only if offline.
// Editing any of these and re-deploying = users get the new version on next reload.
const NETWORK_FIRST = new Set([
  '/',
  '/index.html',
  '/style.css',
  '/app.js',
  '/manifest.webmanifest',
  '/stations.json',
]);

// Static assets — serve from cache for snappy launches; refresh in background.
const SWR_ASSETS = new Set([
  '/vendor/vt323-latin.woff2',
  '/vendor/fredoka-latin.woff2',
  '/vendor/hls.light.min.js',
  '/img/favicon/android-chrome-192x192.png',
  '/img/favicon/android-chrome-512x512.png',
  '/img/favicon/apple-touch-icon.png',
  '/img/favicon/favicon-32x32.png',
  '/img/favicon/favicon-16x16.png',
  '/img/favicon/favicon.ico',
  // Locally-bundled station icons. All station icons are bundled since 20260704b
  // so device packs can share them (see docs/CONTENT-API.md).
  '/img/stations/thelot-alt.png',
  '/img/stations/kiosk-alt.png',
  '/img/stations/lyl-alt.png',
  '/img/stations/nts.png',
  '/img/stations/dublab.png',
  '/img/stations/noods.png',
  '/img/stations/wavezero.png',
]);

// Critical install set — install fails if any of these are missing. We prefetch
// all of them so a first-load offline scenario still works.
const PREFETCH_CRITICAL = [
  '/',
  '/index.html',
  `/style.css?v=${APP_VERSION}`,
  `/app.js?v=${APP_VERSION}`,
  '/vendor/vt323-latin.woff2',
  '/vendor/fredoka-latin.woff2',
  '/vendor/hls.light.min.js',
  `/manifest.webmanifest?v=${APP_VERSION}`,
];

// Best-effort prefetch — 404s here don't block install.
const PREFETCH_OPTIONAL = [
  '/img/favicon/android-chrome-192x192.png',
  '/img/favicon/android-chrome-512x512.png',
  '/img/favicon/apple-touch-icon.png',
  '/img/favicon/favicon-32x32.png',
  '/img/favicon/favicon-16x16.png',
  '/img/favicon/favicon.ico',
];

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    await cache.addAll(PREFETCH_CRITICAL);
    await Promise.allSettled(PREFETCH_OPTIONAL.map((url) => cache.add(url)));
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

  if (NETWORK_FIRST.has(url.pathname)) {
    event.respondWith(networkFirst(req));
  } else if (SWR_ASSETS.has(url.pathname)) {
    event.respondWith(staleWhileRevalidate(req));
  }
  // anything else: pass through to the browser network stack
});

// Allow the page to ask for an immediate skipWaiting (used by the auto-update path).
self.addEventListener('message', (event) => {
  if (event.data === 'skipWaiting') self.skipWaiting();
});

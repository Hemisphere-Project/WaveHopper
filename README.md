# WaveHopper

A curated webradio aggregator. Pick a station, hop to the next one, lock your phone and keep listening. Static frontend (vanilla JS PWA) + a small PHP dispatcher for now-playing metadata. No audio relay — every station is HTTPS with permissive CORS, so the browser plays them directly.

## Layout

```
WaveHopper/
├── public/                       Web docroot (served by nginx in prod)
│   ├── index.html
│   ├── app.js                    Frontend entry point (~20KB, no build step)
│   ├── style.css                 Pixel/8-bit aesthetic + 4 skins
│   ├── manifest.webmanifest      PWA manifest
│   ├── sw.js                     Service worker (shell cache, no stream interception)
│   ├── stations.json             Built artifact — committed for static hosting
│   ├── vendor/
│   │   ├── vt323-latin.woff2     VT323 pixel font (Dark / Paper skins)
│   │   ├── fredoka-latin.woff2   Fredoka rounded font (Fantasy skin)
│   │   └── hls.light.min.js      Lazy-loaded only when an HLS station is picked
│   ├── img/favicon/              PWA icons (192/512/maskable, apple-touch, favicons)
│   └── api/
│       ├── now-playing.php       Per-station now-playing dispatcher
│       ├── fetchers/             One PHP fetcher per source type (nts, airtime, …)
│       ├── lib/                  Shared PHP helpers
│       └── cache/                Filesystem cache for upstream metadata
├── build.py                      Root Python build script for public/stations.json
├── stations/                     Per-station source files, source of truth
│   ├── _order.json               Curated display order
│   ├── _template.md              Template for new stations
│   ├── <id>.json                 Channel definition (one per playable channel)
│   └── <id>.md                   Research notes (status, extraction method, etc.)
└── .claude/skills/
    └── import-station/           The /import-station skill
```

## Station data model

Two parallel files per station:

**`stations/<id>.json`** — the playable channel definition. Multi-channel stations (e.g. NTS) become multiple JSON files (`nts-1.json`, `nts-2.json`, `nts-poolside.json`, …):

```json
{
  "id": "nts-1",
  "station": "NTS",
  "channel": "1",
  "city": "London",
  "url": "https://stream-relay-geo.ntslive.net/stream",
  "format": "mp3",
  "color": "#fff205",
  "nowPlaying": { "type": "nts" },
  "defaultDisabled": false
}
```

Optional fields:
- `defaultDisabled: true` — hidden from the main list on first run, opt-in via config (used for the 16 NTS Mixtapes so the default catalog stays small).
- `nowPlaying`: `{ "type": "nts" | "airtime" | "radiocult" | "lyl-graphql" | "hls-id3" | "none" }`. Some types accept extra fields (e.g. airtime takes `endpoint`).

**`stations/<id>.md`** — research notes with frontmatter `status:` field. Lifecycle:

| Status        | Meaning                                                    |
|---------------|------------------------------------------------------------|
| `pending`     | Never touched                                              |
| `researching` | Actively investigating, partial info                       |
| `extracted`   | Candidate stream URL(s) found, not yet verified            |
| `verified`    | `curl` confirms it streams correctly                       |
| `added`       | JSON file exists and the station is wired up               |
| `broken`      | Could not crack from public site, parked                   |

**`stations/_order.json`** — the curated display order (array of ids). Stations not listed are appended alphabetically; the build script ([build.py](build.py)) handles the merge.

## Current catalog

23 channels across 6 stations, 2 parked:

| Station        | Channels | Source         | Status |
|----------------|----------|----------------|--------|
| Dublab         | 1        | Airtime.pro    | added  |
| Kiosk Radio    | 1        | Airtime.pro    | added  |
| LYL Radio      | 1        | self-hosted    | added  |
| Noods Radio    | 1        | Radiocult      | added  |
| NTS            | 18       | Radiomast      | added  |
| The Lot Radio  | 1        | Livepeer (HLS) | added  |
| Threads Radio  | —        | dead Airtime   | broken |
| VIZI Radio     | —        | site offline   | broken |

## How playback works

- **MP3 (Icecast)**: native `<audio>` everywhere.
- **HLS**: native on Safari/iOS; on Chrome/Firefox/Android Chrome, `hls.js` is lazily fetched the first time the user picks an HLS station (one-time ~110KB gz).
- **Auto-skip**: if the active stream errors fatally (HLS manifest down, MP3 server unreachable), the player auto-advances to the next enabled station with one HLS soft-recovery attempt first. After all enabled stations fail, it stops with "no stations on air".
- **Background hardening**: stuck-time watcher (8s threshold), reattach on `visibilitychange` / `online`, never pauses on backgrounding (the OS handles that).
- **Last station memory**: on reload, the previously-played station is pre-selected; tapping PLAY resumes it without a station picker round-trip.

## Now-playing metadata

[public/api/now-playing.php](public/api/now-playing.php) is a tiny dispatcher: takes `?id=<station-id>`, looks up the station's `nowPlaying.type`, runs the matching fetcher in [public/api/fetchers/](public/api/fetchers/), caches the result on disk, and returns a normalized `{ title, subtitle?, ends?, artwork? }` payload. The frontend polls every 30 seconds while playing and visible — paused or backgrounded means no polling.

`hls-id3` and `none` skip the dispatcher (the former reads ID3 from HLS frags client-side, the latter has no metadata).

## PWA

- Installable on Android Chrome and iOS Safari ("Add to Home Screen"). Runs in standalone mode for better lock-screen audio reliability than a tab.
- Service worker pre-caches the app shell so the player launches offline (streams obviously still need the network).
- Media Session API exposes the current station + show on the OS lock screen and Bluetooth controls (play/pause/next/previous).

## Skins

Four themes, persisted to `localStorage`:

- **Dark** — VT323 pixel font, near-black bg, station accent color drives the active row + play button.
- **Paper** — VT323 on cream, station accent.
- **Fantasy** — Fredoka rounded font, pastel pink/lavender bg, gradient title.
- **Clippy** — Comic Sans MS, retro Word-doc chrome with Win98 bevel buttons.

Fantasy and Clippy ignore the per-station accent so the skin's identity stays coherent. Switch in **CONFIG** mode (gear icon).

## Persistence (localStorage)

| Key                  | Purpose                                       |
|----------------------|-----------------------------------------------|
| `wh:disabled`        | Array of station ids the user has hidden      |
| `wh:skin`            | Current skin id                               |
| `wh:lastStationId`   | Most recently played station, for auto-attach |

## The `/import-station` skill

Project skill at `.claude/skills/import-station/SKILL.md`. Iterative by design — process one station at a time so progress survives interrupted sessions, and the skill's "Patterns we've seen" section sharpens after each station.

```
/import-station <id>
```

Same command for: brand new station (scaffolds from `_template.md`), resuming a parked one, or refreshing a `verified`/`added` whose URL has rotted.

### Quick status dashboard

```sh
grep -H '^status:' stations/*.md
```

Or to see what's left:

```sh
grep -l 'status: pending\|status: researching\|status: broken' stations/*.md
```

## Development

```sh
python3 build.py
php -S 127.0.0.1:3000 -t public
```

`build.py` writes [public/stations.json](public/stations.json) from the per-station files in [stations/](stations/). For local development, rerun it after station changes, then serve [public/](public/) with PHP's built-in server so `/api/*.php` works the same way it does on the deploy target.

### Building the static stations artifact

```sh
python3 build.py          # writes public/stations.json from stations/<id>.json + _order.json
```

Run this before deploying to any PHP-ready host.

## Deploy

Production assumes nginx + PHP-FPM (or any static + PHP host). No Bun or Node runtime is required.

1. `python3 build.py` to refresh `public/stations.json`.
2. Sync [public/](public/) to the docroot.
3. Configure PHP-FPM for `*.php` under `/api/`. Make sure `public/api/cache/` is writable by the PHP user.
4. nginx serves everything else as static files. PWA install requires HTTPS — terminate TLS at nginx.

Service worker scope is `/`, so nothing special is needed in nginx beyond serving `sw.js` with the right MIME type.

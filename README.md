# WaveHopper

A curated webradio aggregator. Pick a station, hop to the next one, lock your
phone and keep listening.

One station catalog, several players: the live web app / PWA at
[waverz.net](https://waverz.net) is the authoritative deploy; device and store
players sync content (and, where applicable, firmware) from it. The design
goal is **maximum shared content, minimum per-player code surface** — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the model and
[docs/CONTENT-API.md](docs/CONTENT-API.md) for the normative cross-player
contract.

## Players

| Player | Status | Code | Content updates |
|---|---|---|---|
| Web / PWA | live | `players/web/` | on every load from `/stations.json` |
| M5Stack CoreS3 | working prototype | `players/m5cores3/` | self-syncs `/content/m5cores3/` pack + self-OTA |
| iOS / Android (Capacitor) | planned | `players/mobile/` | same endpoints as the PWA; binary via stores |

Each player directory has its own `README.md` (humans) and `CLAUDE.md`
(AI-agent working rules with a scope fence), so work on different players can
proceed in parallel without stepping on the shared contract.

## Layout

```
WaveHopper/
├── content/                      SOURCE OF TRUTH
│   ├── stations/                 <id>.json (channel def) + <id>.md (research notes)
│   ├── icons/                    full-size station icon sources
│   └── _order.json               curated display order
├── players/
│   ├── web/public/               deployable docroot (PWA + PHP now-playing API)
│   │   ├── index.html, app.js, style.css, sw.js, manifest.webmanifest
│   │   ├── stations.json         BUILT artifact (committed)
│   │   ├── img/stations/         BUILT icon copies
│   │   ├── content/              BUILT device packs + firmware manifests
│   │   └── api/                  now-playing dispatcher + fetchers (PHP)
│   ├── m5cores3/                 M5Stack CoreS3 firmware (PlatformIO)
│   └── mobile/                   Capacitor shell (placeholder)
├── tools/build.py                content/ → all committed artifacts
├── docs/                         ARCHITECTURE.md + CONTENT-API.md
└── .claude/skills/               /import-station, /import-now-playing
```

## Station data model

Two parallel files per station in `content/stations/`:

**`<id>.json`** — the playable channel definition. Multi-channel stations
(e.g. NTS) become multiple JSON files (`nts-1.json`, `nts-poolside.json`, …):

```json
{
  "id": "nts-1",
  "station": "NTS",
  "channel": "1",
  "city": "London",
  "url": "https://stream-relay-geo.ntslive.net/stream",
  "format": "mp3",
  "color": "#fff205",
  "nowPlaying": { "type": "nts" }
}
```

Optional fields: `homepage`, `icon`, `defaultDisabled` (hidden from the main
list on first run — used for the 16 NTS Mixtapes), `nowPlaying.type` of
`nts | airtime | radiocult | lyl-graphql | thelot-html | azuracast | hls-id3 | none`.
The full field table with per-player notes is in
[docs/CONTENT-API.md](docs/CONTENT-API.md).

**`<id>.md`** — research notes with a frontmatter `status:` field. Lifecycle:

| Status        | Meaning                                             |
|---------------|-----------------------------------------------------|
| `pending`     | Never touched                                       |
| `researching` | Actively investigating, partial info                |
| `extracted`   | Candidate stream URL(s) found, not yet verified     |
| `verified`    | `curl` confirms it streams correctly                |
| `added`       | JSON file exists and the station is wired up        |
| `broken`      | Could not crack from public site, parked            |

**`content/_order.json`** — curated display order (array of ids; unlisted
stations are appended alphabetically by the build).

### Current catalog

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

### Importing stations

Project skills, iterative by design — one station per run so progress survives
interrupted sessions:

```
/import-station <id>        # new station, resume a parked one, or refresh a rotted URL
/import-now-playing <id>    # wire up / re-check the metadata source
```

Quick dashboard: `grep -H '^status:' content/stations/*.md`

## The web player

- **Playback**: MP3/AAC via native `<audio>`; HLS native on Safari, `hls.js`
  lazy-loaded elsewhere. Auto-skip to the next station on fatal stream errors;
  stuck-time watcher and reattach-on-wake hardening for lock-screen listening.
- **PWA**: installable (Android Chrome / iOS Safari), offline shell via
  service worker, Media Session lock-screen controls.
- **Now-playing**: `api/now-playing.php?id=<id>` dispatches to a per-source
  PHP fetcher with disk caching; the frontend polls every 30 s while playing
  and visible.
- **Skins**: Dark, Paper, Fantasy, Clippy — persisted to `localStorage`.

## Development

```sh
python3 tools/build.py                        # rebuild artifacts after content/ edits
php -S 127.0.0.1:3000 -t players/web/public   # serve locally with working PHP API
```

The build is idempotent — with no content changes it rewrites nothing. Icon
downscaling for the m5 pack needs Pillow (imported lazily; only when an icon
actually changed).

For the M5 firmware: see [players/m5cores3/README.md](players/m5cores3/README.md)
(PlatformIO; `pio run`, `pio run -t upload`, `python3 tools/build.py --seed-m5`
+ `pio run -t uploadfs`).

## Deploy (web, authoritative)

Production runs on Infomaniak shared hosting (Apache + PHP).

**Automatic**: pushing to `main` deploys via GitHub Actions
([.github/workflows/deploy.yml](.github/workflows/deploy.yml)) whenever
`players/web/public/**` changed — it verifies the committed artifacts match
`content/` (re-runs the build, fails on diff) then rsyncs the docroot
(`--delete`, sparing the server-writable `api/cache/`). One-time setup: enable
SSH on the hosting and set the `DEPLOY_HOST/USER/KEY/PATH` repo secrets — see
the workflow header.

**Manual** (fallback):

1. `python3 tools/build.py` and commit any artifact changes.
2. rsync `players/web/public/` to the docroot (keep `api/cache/` writable).

Cache rules live in `players/web/public/.htaccess` (Apache/LiteSpeed); mirror
them in nginx where applicable — `/content/**/manifest.json` must never be
served stale (see CONTENT-API.md §Cache policy).

Firmware releases for the M5 player follow the runbook in
[docs/CONTENT-API.md](docs/CONTENT-API.md#firmware-release-runbook-m5cores3).

## License

GPL-3.0 — see [LICENSE](LICENSE). The M5 firmware links GPL-3.0
[ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S).

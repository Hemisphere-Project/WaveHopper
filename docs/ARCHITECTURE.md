# WaveHopper Architecture

One catalog of curated webradio stations, many players. This repo holds the
content source of truth, the build pipeline, and every player implementation.

## The model

```
content/  ──build──▶  players/web/public/  ──deploy──▶  https://waverz.net
(source)              (committed artifacts)                    │
                                                               │ serves
                       ┌───────────────────────────────────────┤
                       ▼                    ▼                  ▼
                   web PWA            mobile apps         M5 firmware
                   (browsers)      (Capacitor shells)    (CoreS3 devices)
```

- **`content/` is the only source of truth** for stations and icons. Nobody
  edits built artifacts by hand.
- **The web deploy is authoritative**: whatever is live at `waverz.net` *is*
  the current catalog, content packs, and firmware pointers. Content flows one
  way: repo → `tools/build.py` → docroot → deploy → clients.
- **Firmware surface is kept minimal** on every player so app/firmware updates
  are rare and content updates are instant and shared.

## Update matrix

| Player | Code/binary updates | Content updates |
|---|---|---|
| Web PWA | on deploy (service worker, network-first shell) | `/stations.json` on every load (network-first) |
| Mobile (Capacitor) | app stores | same web endpoints as the PWA |
| M5 CoreS3 | self-OTA from `/content/firmware/m5cores3/` | syncs `/content/m5cores3/` pack (manifest + sha256 diff) |

The full contract — endpoints, schemas, versioning and sync semantics — lives
in [CONTENT-API.md](CONTENT-API.md). **That file is normative**; read it before
touching anything a shipped client consumes.

## Repo map

```
content/                 source of truth
├── stations/            <id>.json (channel definition) + <id>.md (research notes)
├── icons/               full-size station icon sources
└── _order.json          curated display order

players/                 one directory per player, each with its own CLAUDE.md
├── web/public/          deployable docroot: PWA + PHP now-playing API
│   └── content/         build-emitted packs + firmware manifests (committed)
├── m5cores3/            PlatformIO firmware for M5Stack CoreS3 / SE
└── mobile/              Capacitor iOS/Android shell (placeholder for now)

tools/build.py           content/ → all committed artifacts (see its docstring)
docs/                    this file + CONTENT-API.md
.claude/skills/          /import-station and /import-now-playing workflows
```

## Build & deploy sequence

1. Edit `content/` (usually via the `/import-station` or `/import-now-playing`
   skills — one station at a time).
2. `python3 tools/build.py` — regenerates web `stations.json`, docroot icon
   copies, and the m5 content pack. Idempotent: unchanged content rewrites
   nothing.
3. Commit sources and artifacts together.
4. Deploy: rsync `players/web/public/` to the docroot at waverz.net (nginx +
   PHP-FPM; `api/cache/` must stay writable by the PHP user).

Web code changes additionally require an `APP_VERSION` bump in
`players/web/public/sw.js` + `app.js` — see `players/web/CLAUDE.md`.

Firmware releases follow the runbook at the end of CONTENT-API.md.

## Why per-player content packs

Different players have different constraints (the M5 can't render remote
icons, needs small images, can't play HLS). Instead of one lowest-common-
denominator catalog, `tools/build.py` emits a *transformed view* per player
under `/content/<player>/`, each with a content-addressed manifest. The web
app predates this scheme and keeps its legacy endpoints (`/stations.json`,
`/img/stations/*`) — installed PWAs can't be forced to migrate, and mobile
wraps the same web code, so those paths are frozen.

## Adding a new player

1. Create `players/<name>/` with a `README.md` and a `CLAUDE.md` (scope fence +
   inline contract summary — copy the shape of `players/m5cores3/CLAUDE.md`).
2. If it needs its own pack: add an emit step in `tools/build.py` and document
   the new paths in CONTENT-API.md (append-only).
3. If it self-updates: add `/content/firmware/<name>/` with the same manifest
   schema and `build`-number semantics.

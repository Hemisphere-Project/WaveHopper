# WaveHopper Content API — the cross-player contract

**This document is normative.** Every player (web PWA, mobile, M5 firmware,
future devices) consumes the endpoints and schemas below, and shipped clients
cannot be forced to update. Therefore:

- URL paths and schemas here are **append-only**. Adding optional fields is
  fine; renaming, removing, or changing the meaning of anything is a breaking
  change and requires a `schemaVersion` bump **plus** an audit of every shipped
  client.
- All clients MUST ignore unknown JSON keys, everywhere.
- The server side of this contract is produced by `tools/build.py` and the PHP
  API in `players/web/public/api/` — changes there must stay in sync with this
  file, in the same commit.

Authoritative origin: **`https://waverz.net`** (the deployed
`players/web/public/` docroot).

## Endpoint families

| Path | Consumers | Purpose |
|---|---|---|
| `/stations.json` | web PWA, mobile | Station catalog, legacy shape (bare array) |
| `/img/stations/<file>.png` | web PWA, mobile | Full-size station icons |
| `/content/m5cores3/manifest.json` | M5 firmware | Content pack manifest |
| `/content/m5cores3/stations.json` | M5 firmware | Transformed catalog (see below) |
| `/content/m5cores3/icons/<file>.png` | M5 firmware | 64×64 downscaled icons |
| `/content/firmware/m5cores3/manifest.json` | M5 firmware | Published-firmware pointer |
| `/content/firmware/m5cores3/<name>-<version>+<build>.bin` | M5 firmware | OTA images (versioned names, never `latest.bin`) |
| `/api/now-playing.php?id=<station-id>` | all players | Normalized now-playing metadata |

A new player type gets its own pack under `/content/<player-dirname>/` (rule:
pack id == its directory name under `players/`) and, if it self-updates, a
firmware dir under `/content/firmware/<player-dirname>/`.

## Station object

Produced from `content/stations/<id>.json` sources. Fields:

| Field | Type | Consumers / notes |
|---|---|---|
| `id` | string | all — unique key; used in `/api/now-playing.php?id=` |
| `station` | string | all — display name |
| `channel` | string | all — channel label, may be `""` |
| `city` | string | all — display |
| `url` | string | all — stream URL (arbitrary hosts) |
| `format` | `"mp3" \| "aac" \| "hls"` | all — playback capability filter; M5 v1 plays `mp3` only and must skip others |
| `color` | string (hex) | all — accent; M5 uses it as icon placeholder when `icon` absent |
| `homepage` | string, optional | web — link out |
| `icon` | string, optional | web: docroot-absolute (`/img/stations/…`) or remote URL. **M5 pack: pack-relative (`icons/…`) or absent — never a remote URL** (the firmware only talks TLS to waverz.net) |
| `nowPlaying` | object, optional | `{ "type": "nts" \| "airtime" \| "radiocult" \| "lyl-graphql" \| "thelot-html" \| "azuracast" \| "hls-id3" \| "none", … }`; `type` decides whether polling `/api/now-playing.php` is useful (`hls-id3` and `none` are client-side/no-op) |
| `defaultDisabled` | bool, optional | all — hidden from the default list, opt-in via config |

`/stations.json` (web) is a bare JSON array of these objects.
`/content/m5cores3/stations.json` is the same array with the `icon` rewrite
applied. Unknown fields: ignore.

## Content pack manifest

`/content/m5cores3/manifest.json`:

```json
{
  "schemaVersion": 1,
  "contentVersion": "d12f93a7133b…(64 hex)",
  "generated": "2026-07-04T10:00:00Z",
  "totalSize": 20419,
  "files": [
    { "path": "icons/kiosk-alt.png", "sha256": "…", "size": 4384, "srcSha256": "…" },
    { "path": "stations.json", "sha256": "…", "size": 9421 }
  ]
}
```

- **`contentVersion`** = sha256 hex over the concatenation of
  `"{path}\n{sha256}\n"` for `files` sorted lexicographically by `path`.
  `generated` and `totalSize` are excluded, so the version is purely
  content-addressed and builds are idempotent.
- **Comparison is equality, not ordering.** A client syncs iff the remote
  `contentVersion` differs from the one it recorded locally.
- `path` is relative to the manifest's directory. Clients MUST reject paths
  containing `..` or starting with `/`.
- **Deletions are implicit**: any file in a client's local mirror that is not
  listed in `files` must be removed when the sync commits.
- `srcSha256` is build provenance (hash of the icon's full-size source);
  clients ignore it.
- If `schemaVersion` is greater than what the client supports: skip content
  sync, but **still check the firmware manifest** — a firmware update is how
  the client learns the new schema.

## Firmware manifest

`/content/firmware/m5cores3/manifest.json`:

```json
{
  "schemaVersion": 1,
  "board": "m5cores3",
  "version": "0.2.1",
  "build": 12,
  "url": "https://waverz.net/content/firmware/m5cores3/wavehopper-m5cores3-0.2.1+12.bin",
  "sha256": "…(64 hex)",
  "size": 1834240
}
```

- **`build` is the update trigger**: a device updates iff remote `build`
  (integer) is **strictly greater** than its compiled-in build number.
  `version` (semver) is for humans and logs only. Strictly-greater means a
  stale cached manifest can never downgrade a device; a deliberate rollback is
  published as old code under a **new** build number.
- `build: 0` with `url: null` means "nothing published yet" — devices no-op.
  (`tools/build.py` creates this placeholder if the file is missing and never
  overwrites an existing one.)
- `board` must match the device's compiled-in board id (`m5cores3`) — cheap
  insurance against flashing the wrong player's image.
- Binaries use versioned filenames so a cached manifest can never pair with a
  mismatched binary; the device verifies `sha256` while streaming regardless.

## Device sync algorithm (normative for self-updating players)

State on device: a local mirror of the pack (e.g. LittleFS
`/content/m5cores3/`) whose local `manifest.json` copy is the **commit
marker**, plus a staging dir (`/content/.staging/`).

1. On boot: if the staging dir is non-empty, wipe it. If the local manifest is
   missing or unparseable, treat everything as needing sync.
2. GET the remote content manifest. If `contentVersion` equals the local
   record → done.
3. If remote `schemaVersion` > supported → skip to firmware check.
4. Free-space check: `totalSize` + 64 KB slack must fit (LittleFS degrades
   badly when nearly full).
5. For each entry whose `sha256` differs from the local file (or which is
   missing locally): GET `<pack-base>/<path>?v=<contentVersion>` (the query
   string busts intermediary caches), stream to `staging/<path>` while
   computing sha256; verify hash and size. Retry a failed file up to 3 times —
   whole-file retry, no Range resume (files are small).
6. Commit, ordered: rename icons into place **first**, `stations.json`
   **second** (each rename is atomic; ordering keeps a crashed commit from
   leaving a catalog that references missing icons for longer than one file).
7. Write the local `manifest.json` copy **last** — this is the commit. Then
   delete local files not listed in `files`, and wipe staging.
8. A power cut anywhere before step 7 self-heals: the marker was never
   written, so the next pass re-diffs and re-downloads only what differs.

Firmware check (independent of content sync, run after it): GET the firmware
manifest; if `board` matches and `build` > own, stream the binary into the
inactive OTA slot, verifying `sha256` while streaming; activate only on match;
reboot. There is no bootloader rollback on stock arduino-esp32 — the sha256
gate plus on-device testing of every published build is the safety story.

Recommended cadence: content check on boot + every 6 h while idle; firmware
check on boot + daily. Never run sync or OTA downloads (TLS ≈ 50 KB heap)
while audio is streaming.

## Now-playing

`GET /api/now-playing.php?id=<station-id>` → normalized JSON:

```json
{
  "title": "primary line (track or show host)",
  "subtitle": "secondary line or null",
  "starts": "2026-07-04T08:00:00+00:00",
  "ends": "2026-07-04T10:00:00+00:00",
  "next": { "title": "…", "starts": "…" },
  "source": "airtime",
  "fetchedAt": 1783153719
}
```

`starts`/`ends` are ISO 8601 (nullable); `next` is optional; `fetchedAt` is
epoch seconds. HTTP 204 = nothing playing / `type: none`. Upstream fetch +
disk cache are handled server-side (see `players/web/public/api/`). Poll only
while playing and visible/awake, ≥ 30 s interval. Stations with
`nowPlaying.type` of `hls-id3` or `none` (or no `nowPlaying` at all) are not
served by the dispatcher.

## Cache policy (server)

- `/content/**/manifest.json`: `Cache-Control: max-age=0, must-revalidate`.
- Pack files and icons: long-lived caching is fine — clients bust with
  `?v=<contentVersion>` and verify sha256.
- Firmware `.bin`: served as `application/octet-stream`; versioned filenames
  make caching harmless.

Configured in `players/web/public/.htaccess` (Apache) — mirror any change in
the nginx config on the production host.

## Firmware release runbook (m5cores3)

1. Bump `WH_FW_BUILD` (always) and `WH_FW_VERSION` (human-meaningful) in
   `players/m5cores3/platformio.ini`.
2. `pio run` → `players/m5cores3/.pio/build/m5stack-cores3/firmware.bin`.
3. Rename to `wavehopper-m5cores3-<version>+<build>.bin`, compute sha256 + size.
4. Upload the `.bin` to `/content/firmware/m5cores3/` on the host **first**.
5. Rewrite `manifest.json` (schema above) **last** — it is the publish switch.
6. Test the OTA on the in-hand device before walking away. Keep the previous
   `.bin` on the host until the fleet has moved past it.

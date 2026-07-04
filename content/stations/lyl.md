---
id: lyl
name: LYL Radio
site: https://lyl.live/
status: added
last_checked: 2026-05-03
---

## Channels
- [x] main — stream URL: https://radio.lyl.live/hls/live.m3u8

LYL is a single live channel served as an HLS master playlist with three AAC variants (`aac_lofi.m3u8` 140kbps, `aac_midfi.m3u8` 105kbps — actually mid, `aac_hifi.m3u8` 211kbps). Store the master, not a variant.

## Extraction notes

- Homepage is a CRA SPA shell (~3.7KB HTML, root div + webpack chunk loader). No URLs in the HTML.
- Main JS chunk references `https://api.lyl.live/graphql` and a player div with classNames `mixcloud`/`airtime` — misleading, the actual host is neither Mixcloud nor Airtime.pro.
- The player config is fetched at runtime via GraphQL. Found the query embedded in the bundle: `query { onair { title hls } }`.
- Hitting the GraphQL endpoint directly returned the canonical relay:
  ```
  curl -sL -X POST https://api.lyl.live/graphql \
    -H 'Content-Type: application/json' \
    -d '{"query":"{ onair { title hls } }"}'
  → {"data":{"onair":{"title":"...","hls":"https://radio.lyl.live/hls/live.m3u8"}}}
  ```
- `radio.lyl.live` looks like a self-hosted Nginx HLS origin (etag, no Server header leak, permissive CORS).

## Verification

- Format: HLS (AAC, master playlist with 3 variants)
- Scheme: https
- CORS: `access-control-allow-origin: *` (works with Web Audio API and `<audio>`)
- Mixed-content risk: none

```
curl -I -L https://radio.lyl.live/hls/live.m3u8
HTTP/2 200
content-type: application/vnd.apple.mpegurl
access-control-allow-origin: *
```

## Now-playing
- **Source type:** `lyl-graphql`
- **Endpoint:** `POST https://api.lyl.live/graphql` — query `{ calendar { startAt duration title artists } }`
- **Mapping:** `artists` → `title` (show host), `title` → `subtitle` (show name), `startAt` (UTC ISO 8601) + parsed `duration` → `starts`/`ends`. Next entry in the list → `next.title`/`next.starts`.
- **Cache key:** `lyl-graphql` (shared, 30 s TTL — show-level, full-day schedule per call)
- **Caveats:** `calendar` returns ~23 entries covering today's broadcast day from ~01:00 UTC. The `onair` query also exists (`{ onair { title } }`) but only returns the combined `"Artist - Show"` string with no separate fields and no timestamps — `calendar` is strictly better. No timezone conversion needed: `startAt` is already UTC.

## Open questions

- LYL also has a Paris studio per their meta description; unclear whether it broadcasts to a separate channel or shares the main feed. The GraphQL `onair` query exposes only one HLS field, so it's a single feed.

## M5 device playback (2026-07-05)

The lib (ESP32-audioI2S 3.4.6) plays LYL's HLS natively: master → AAC-LC
variants, standard nginx MPEG-TS segments. Verified 50+ s stable on the
CoreS3 (buffer flat, arrival = consumption). `m5Url` points at `aac_hifi`
(211 kbps) — the lib otherwise picks the first-listed variant (lofi).

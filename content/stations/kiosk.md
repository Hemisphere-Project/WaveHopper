---
id: kiosk
name: Kiosk Radio
site: https://www.kioskradio.com
status: added
last_checked: 2026-05-04
---

## Channels
- [x] Main — stream URL: https://kioskradiobxl.out.airtime.pro/kioskradiobxl_b

## Extraction notes
Next.js site backed by Contentful CMS. Homepage HTML had no inline stream URLs. Stream URL found in
`/_next/static/chunks/pages/_app-b799ac94a18eb2ca.js` — the `_app` bundle initializes the player
with `liveInfoUrl:"/api/now-playing"` and `streamingUrl:"https://kioskradiobxl.out.airtime.pro/kioskradiobxl_b"`.
Two Streamnerd HLS URLs were also present in the bundle
(`https://play.streamnerd.nl/kioskradio/kioskradio/playlist.m3u8` and `.../kioskradio2/playlist.m3u8`)
but both returned HTTP 404. Only the Airtime Icecast stream is live.

## Verification
- Format: AAC (`audio/aac`), 192 kbps — Icecast 2.4.0-kh15, `icy-name: Kiosk Radio`
- Scheme: https ✓
- CORS: `Access-Control-Allow-Origin: *` ✓
- Mixed-content risk: none

## Now-playing
- **Source type:** `airtime`
- **Endpoint:** `https://kioskradiobxl.airtime.pro/api/live-info-v2`
  - Also proxied via the site at `https://www.kioskradio.com/api/now-playing` (identical response)
- **Mapping:** `shows.current.name` → `title` (HTML-decoded, trimmed); no subtitle. `shows.current.starts/ends` → `starts/ends` (converted from `station.timezone = Europe/Brussels` to UTC ISO 8601). `shows.next[0]` → `next.title/starts`.
- **Cache key:** `airtime-kiosk` (30 s TTL — show-level data)
- **Caveats:** Show names arrive HTML-entity-encoded (e.g. `&amp;`) with occasional leading whitespace — the fetcher normalises both. When a Live DJ is on air, `tracks.current.type = "livestream"` and `tracks.current.name` is empty; the show name from `shows.current` is the only usable metadata.

## Open questions

## Plain-HTTP variant (m5)
`http://kioskradiobxl.out.airtime.pro:8000/kioskradiobxl_b` — Airtime's native
Icecast port serves plain HTTP directly. Verified 2026-07-06 (200, audio/aac,
realtime flow). Set as `m5Url` (device heap relief — see CONTENT-API.md);
web keeps the https url.

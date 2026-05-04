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
(filled in by /import-now-playing — source endpoint, mapping to title/subtitle/starts/ends, cache key, any caveats)

## Open questions

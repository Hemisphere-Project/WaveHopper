---
id: wavezero
name: Wave Zero
site: https://wavezero.world
status: added
last_checked: 2026-05-04
---

## Channels
- [x] live — stream URL: `https://stream.ondezero.net/listen/oz/radio.mp3`

## Extraction notes
Wave Zero is a pirate webradio "for the Mediterranean waves" tied to a spring flotilla to Gaza. Site is server-rendered HTML with a custom JS player loaded from `/config.js`, `/station.js`, `/player.js` (no bundler chunks).

The whole player config — including the canonical stream URL — sits in `/config.js` as a single inline JSON blob assigned to `window.__STATION_CONFIG__`. Relevant fields:
- `azuracastHost`: `stream.ondezero.net`
- `stationShortcode`: `oz`
- `streamUrl`: `https://stream.ondezero.net/listen/oz/radio.mp3`
- `sseUrl`: `https://stream.ondezero.net/api/live/nowplaying/sse?cf_connect=...station:oz...`

Backend is **AzuraCast** (open-source radio platform), hosted under `stream.ondezero.net` with shortcode `oz` ("Onde Zero" — the Italian sister/origin of Wave Zero). Standard AzuraCast mountpoint: `/listen/<shortcode>/radio.mp3`.

## Verification
- Format: MP3 (192 kbps, 44.1 kHz stereo) — `Content-Type: audio/mpeg`, `icy-br: 192`, `icy-name: WaveZero`
- Scheme: https
- CORS: reflects Origin (`Access-Control-Allow-Origin: https://wavezero.world` when that Origin is sent; `Vary: Origin`). Fine for `<audio>` tags from any origin; if WaveHopper uses Web Audio API from a different origin, a relay is needed.
- Mixed-content risk: none

## Now-playing
(filled in by /import-now-playing — likely the AzuraCast SSE endpoint at `https://stream.ondezero.net/api/live/nowplaying/sse?cf_connect={"subs":{"station:oz":{"recover":true}}}`, or the simpler REST `https://stream.ondezero.net/api/nowplaying/oz`)

## Open questions
- Is the project still on-air outside flotilla periods? Schedule blob in `config.js` shows live programming through May 2026.

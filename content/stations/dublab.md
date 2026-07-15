---
id: dublab
name: Dublab
site: https://www.dublab.com/
status: added
last_checked: 2026-05-03
---

## Channels
- [x] main (Los Angeles) — https://dublab.out.airtime.pro/dublab_a

(Sister stations dublab.de, dublab.es, dublab.jp, dublab.com.br are linked from
the homepage but operate as separate sites with their own players. Not imported
here — would need their own `/import-station` runs.)

## Extraction notes
The homepage is a Vue SPA: raw HTML is just `<div id="app"></div>` with no inline
state. URLs are baked into the JS bundle. Steps:

1. `curl -sL https://www.dublab.com/` → 2KB shell, no streams.
2. From the HTML, grab the script paths (`/js/app.*.js`, `/js/chunk-vendors.*.js`)
   and `curl` them to `/tmp/`.
3. `grep -ioE '(\.mp3|\.m3u8|\.aac|stream|out\.airtime|livepeer|icecast)' app.js`
   surfaces two candidates:
   - `https://livepeercdn.studio/hls/8d33n2at0gq7aud6/index.m3u8` — used by the
     `<video>`-ref HLS player. This is Dublab's **video** broadcast (Dublab
     Visions). When no stream is live the manifest returns
     `#EXT-X-ERROR: Stream open failed` followed by `#EXT-X-ENDLIST`.
   - `https://dublab.out.airtime.pro/dublab_a` — Airtime.pro Icecast 2.4.0
     stream, 192kbps MP3. **This is the actual radio audio.** Picked this one.

The fact that the audio stream is on a different host (Airtime.pro, a
Sourcefabric Airtime install) than the video stream (Livepeer) is the key
insight — don't assume one player URL covers both.

Also visible in the bundle but not used:
- `http://74.62.195.106:8000/live/dubhome.flv` — legacy FLV, IP-only, HTTP-only.
- `https://lvpr.tv?v=8d33n2at0gq7aud6` — Livepeer's hosted player wrapper.
- `https://player.dacast.com/...` — appears to be a fallback video embed.

## Verification
- Format: MP3 (audio/mpeg), 192kbps, 44.1kHz stereo (Icecast headers)
- Scheme: https
- CORS: `Access-Control-Allow-Origin: *`
- Mixed-content risk: none
- Server: Icecast 2.4.0-kh15
- icy-name: dublab, icy-description: "coming at you from LA"

## Now-playing
- **Source type:** `airtime`
- **Endpoint:** `https://dublab.airtime.pro/api/live-info-v2`
- **Mapping (auto-DJ / track mode):** `tracks.current.type = "track"` → `metadata.track_title` → `title`, `metadata.artist_name` → `subtitle`, `tracks.current.starts/ends` → `starts/ends` (20 s TTL).
- **Mapping (live DJ / stream mode):** `tracks.current.type = "livestream"` → `shows.current.name` → `title`, no subtitle, `shows.current.starts/ends` → `starts/ends`, `shows.next[0]` → `next`.
- **Cache key:** `airtime-dublab` (20 s TTL)
- **Timezone:** `America/Los_Angeles` → converted to UTC ISO 8601 by the fetcher.
- **Caveats:** Show names are HTML-entity-encoded with occasional leading whitespace — the fetcher normalises both. When auto-DJ is running, per-track timestamps are exact to the second; show-level `starts/ends` are only available as the show window, not the individual track.

## Open questions
- Add dublab.de / dublab.es / dublab.jp / dublab.com.br as separate stations?
  They're independent operations broadcasting from Berlin, Barcelona, Tokyo,
  São Paulo respectively. Each would need its own import.

## Plain-HTTP variant (m5)
`http://dublab.out.airtime.pro:8000/dublab_b` — Airtime's native Icecast port
serves plain HTTP directly (port 80 is unreachable, not a redirect). Set as
`m5Url` for two reasons: (1) plain HTTP frees ~50 KB of internal heap the
device would otherwise pin on a stream TLS session (the verified
now-playing/telemetry handshakes need it); (2) the `_b` mount is **128 kbps**
vs `_a`'s 192 kbps — same live program, re-encoded lower — which the CoreS3
never out-resolves and which buys real headroom on flaky wifi (lower arrival
rate to stay realtime, faster prebuffer convergence). Mounts verified
2026-07-15: `_a` 200 audio/mpeg icy-br:192, `_b` 200 audio/mpeg icy-br:128
(frame header `fffb 9264` = MPEG-1 L3 128 kbps 44.1 kHz, sustained realtime).
Web keeps the https `_a` url (mixed content + no bandwidth constraint).

Policy: the m5 pack carries the lowest listenable mount (~96–128 kbps floor)
a platform offers, plain-HTTP where possible — see CONTENT-API.md `m5Url`.

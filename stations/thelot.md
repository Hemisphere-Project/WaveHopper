---
id: thelot
name: The Lot Radio
site: https://www.thelotradio.com/
status: added
last_checked: 2026-05-06
---

## Channels
- [x] main (Brooklyn) — https://livepeercdn.studio/hls/85c28sa2o8wppm58/index.m3u8

## Extraction notes
Next.js site (`/_next/static/chunks/...`), but the player config is inlined in
the homepage HTML as a JSON blob:

```
"title":"The Lot Live","src":[
  {"type":"hls","src":"https://livepeercdn.studio/hls/85c28sa2o8wppm58/index.m3u8"},
  {"type":"webrtc","src":"https://livepeercdn.studio/webrtc/85c28sa2o8wppm58"}
]
```

Caught by `grep -oE '.{80}livepeercdn[^" ]+' /tmp/thelot_home.html` over the raw
HTML — no need to crack the JS chunks.

The Lot is a live video broadcast from a shipping container in Brooklyn (Tue–Sat
afternoons/evenings ET). The HLS feed carries the video+audio mix; there is
**no separate audio-only Icecast fallback** visible on the site (unlike Dublab).

The page also exposes 8 `link.storjshare.io/raw/.../thelot-archive/episodes/<id>/hls/index.m3u8`
URLs — these are archived episodes (Storj decentralized storage, served over HLS),
not part of the live feed. Skipped per skill rules.

When checked at 2026-05-03 16:57 UTC (Sunday afternoon NYC), the manifest
returned `#EXT-X-ERROR: Stream open failed` + `#EXT-X-ENDLIST` — broadcaster
offline. The URL itself is correct (HTTP 200, audio/HLS content-type, CORS `*`,
307s to playback.livepeer.studio). Pattern previously documented from Dublab.

## Verification
- Format: HLS (application/vnd.apple.mpegurl), video+audio
- Scheme: https
- CORS: `Access-Control-Allow-Origin: *`
- Mixed-content risk: none
- Live state at check: offline (between broadcasts) — URL still valid
- WebRTC alternative also exposed: https://livepeercdn.studio/webrtc/85c28sa2o8wppm58

## Open questions
- WaveHopper's player needs to handle HLS (hls.js) and tolerate offline live
  streams gracefully — the manifest can return an empty/error playlist when no
  show is on air.
- Are the archived episodes worth pulling into a separate "shows" view? Out of
  scope for this skill.

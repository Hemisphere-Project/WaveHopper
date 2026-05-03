---
id: nts
name: NTS Live
site: https://www.nts.live/
status: added
last_checked: 2026-05-03
---

## Channels

### Live channels
- [x] NTS 1 — https://stream-relay-geo.ntslive.net/stream
- [x] NTS 2 — https://stream-relay-geo.ntslive.net/stream2

### Infinite Mixtapes
- [x] Poolside — https://stream-mixtape-geo.ntslive.net/mixtape4
- [x] Slow Focus — https://stream-mixtape-geo.ntslive.net/mixtape
- [x] Low Key — https://stream-mixtape-geo.ntslive.net/mixtape2
- [x] Memory Lane — https://stream-mixtape-geo.ntslive.net/mixtape6
- [x] 4 To The Floor — https://stream-mixtape-geo.ntslive.net/mixtape5
- [x] Island Time — https://stream-mixtape-geo.ntslive.net/mixtape21
- [x] The Tube — https://stream-mixtape-geo.ntslive.net/mixtape26
- [x] Sheet Music — https://stream-mixtape-geo.ntslive.net/mixtape35
- [x] Feelings — https://stream-mixtape-geo.ntslive.net/mixtape27
- [x] Expansions — https://stream-mixtape-geo.ntslive.net/mixtape3
- [x] Rap House — https://stream-mixtape-geo.ntslive.net/mixtape22
- [x] Labyrinth — https://stream-mixtape-geo.ntslive.net/mixtape31
- [x] Sweat — https://stream-mixtape-geo.ntslive.net/mixtape24
- [x] Otaku — https://stream-mixtape-geo.ntslive.net/mixtape36
- [x] The Pit — https://stream-mixtape-geo.ntslive.net/mixtape34
- [x] Field Recordings — https://stream-mixtape-geo.ntslive.net/mixtape23

## Extraction notes
URLs were inline in the homepage HTML, embedded in a JSON blob of the form
`"streams":{"nts1":{...,"href":"https://stream-relay-geo.ntslive.net/stream"},"nts2":{...}}`
and, for mixtapes, as repeated objects with `"audio_stream_endpoint":"https://stream-mixtape-geo.ntslive.net/mixtapeN"`.
Plain `grep` over the raw HTML caught everything — no JS execution needed.
WebFetch's summarised view missed all of them; raw `curl | grep` is the fallback.

Both relay hosts (`stream-relay-geo.ntslive.net`, `stream-mixtape-geo.ntslive.net`)
302 to `streams.radiomast.io/<id>`, which 302s again to a regional
`audio-edge-*.fra.h.radiomast.io` Radiomast edge. Always reference the relay URL
(stable id), not the edge URL (rotates per request).

The page also surfaces an `audio_stream_endpoint_hls_aac` field on each mixtape
pointing at `streams.radiomast.io/<uuid>/hls.m3u8`. Those are the same streams in
HLS form — preferred the MP3 relays for consistency with NTS 1/2.

Mixtape number → name mapping (worth keeping; not obvious from the URL):
mixtape=Slow Focus, 2=Low Key, 3=Expansions, 4=Poolside, 5=4 To The Floor,
6=Memory Lane, 21=Island Time, 22=Rap House, 23=Field Recordings, 24=Sweat,
26=The Tube, 27=Feelings, 31=Labyrinth, 34=The Pit, 35=Sheet Music, 36=Otaku.
The numbers are roughly chronological (launch order), not curated.

## Verification
- Format: MP3 (audio/mpeg) on all 18 channels
- Scheme: https (no relay needed)
- CORS: `Access-Control-Allow-Origin: *` (Web Audio API safe)
- Mixed-content risk: none
- Spot-checked: NTS 1, NTS 2, Poolside (mixtape4), Slow Focus (mixtape), Otaku (mixtape36)

## Open questions
- None.

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

- **Type:** `azuracast` (new fetcher)
- **Endpoint:** `https://stream.ondezero.net/api/nowplaying/oz`
- **Cache key:** `azuracast-wavezero` (per-station, 20 s TTL)

The REST endpoint returns rich data — the SSE alternative was rejected because it needs a persistent connection that doesn't fit the file-cached PHP dispatcher model.

### Mapping

| Source field                                | Normalized field |
|---------------------------------------------|------------------|
| `now_playing.song.title`                    | `title`          |
| `now_playing.song.artist`                   | `subtitle`       |
| `now_playing.played_at` (Unix UTC seconds)  | `starts`         |
| `now_playing.played_at + duration`          | `ends`           |
| `playing_next.song.title`                   | `next.title`     |
| `playing_next.cued_at`                      | `next.starts`    |

When `live.is_live` is true a human DJ is on. The streamer name (`live.streamer_name`) becomes the title and the song title (if present) becomes the subtitle. When the auto-DJ has no `song.artist` (rare), the playlist tag is used as a soft fallback after stripping the `[CATEGORY] - ` prefix.

`is_online: false` → fetcher returns null → dispatcher 204 → frontend hides the card.

Sample observed payload (auto-DJ track mode):

```
song.title  = "Another genocide behind walls"
song.artist = "Pierre Loiselle, Nora Barrows-Friedman, Tamara Nassar"
song.album  = "Electronic Intifada Radio"
playlist    = "[SHOW] - Electronic Intifada - Radio"
played_at   = 1777896006   (Unix UTC, no tz conversion needed)
duration    = 3480
elapsed     = 2545, remaining = 935
```

### Verification

```sh
curl -sS https://stream.ondezero.net/api/nowplaying/oz | jq '{
  online: .is_online,
  song: .now_playing.song.title,
  artist: .now_playing.song.artist,
  starts: (.now_playing.played_at | todate)
}'
```

## Open questions
- Is the project still on-air outside flotilla periods? Schedule blob in `config.js` shows live programming through May 2026.

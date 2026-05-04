---
id: noods
name: Noods Radio
site: https://noodsradio.com/
status: added
last_checked: 2026-05-03
---

## Channels
- [x] main — stream URL: https://noods-radio.radiocult.fm/stream

Single live channel. Despite the prior note, the public site does not expose additional simultaneous "rooms" — they have multiple shows but one live broadcast at a time.

## Extraction notes

- Homepage is server-rendered (~580KB of HTML). No SPA grind needed.
- Stream URL is right there in an `<audio>` tag:
  ```html
  <audio id="audioplayer" class="hidden" controls="true"
         src="https://noods-radio.radiocult.fm/stream" type="audio/mp3">
  ```
- Hosted on **Radiocult.fm** — Icecast-based stream provider (server header confirms `Icecast`). 192 kbps MP3, permissive CORS.

## Verification

- Format: MP3 (`audio/mpeg`)
- Scheme: https
- CORS: `access-control-allow-origin: *`
- Mixed-content risk: none

```
curl -I -L https://noods-radio.radiocult.fm/stream
HTTP/2 200
server: Icecast
content-type: audio/mpeg
icy-br: 192
icy-name: noods-radio
access-control-allow-origin: *
```

## Now-playing
- **Source type:** `radiocult`
- **Endpoint:** `GET https://api.radiocult.fm/api/station/noods-radio/schedule?startDate={now}&endDate={now+3h}`
- **Response shape:** `{ "schedules": [{ startDateUtc, endDateUtc, title, artistIds, ... }] }` sorted by start time. All timestamps UTC ISO 8601.
- **Mapping:** `schedules[current].title` → `title` (show name; host is usually embedded, e.g. "Strung Out w/ Izzy Twist"). `subtitle` → null (no separate host/artist field exposed publicly — `artistIds` are UUIDs, the `/artists/{id}` endpoint returns `{"success":false}`).
- **Cache key:** `radiocult-noods` (30 s TTL — show-level, typically 1-hr slots)
- **Caveats:** `slug` from `nowPlaying.slug` in station JSON (`noods-radio`) used to build the URL. The `status: "schedule"` field in the live endpoint shows the data is schedule-driven (pre-uploaded mixes). A `metadata` block with `artist: "Live Now"` is present but not useful.

## Open questions

- None. Single channel, clean Icecast.

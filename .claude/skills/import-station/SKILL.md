---
name: import-station
description: Extract and verify the live audio stream URL for a single webradio station, then update its station MD and per-station JSON. Use when the user asks to import, add, refresh, or re-check a WaveHopper station. Argument is the station id (e.g. /import-station nts).
---

# Import Station

You are extending WaveHopper's station catalog by cracking the stream URL of **one** webradio station. Process exactly one station per invocation.

## Inputs

The user invokes this skill with a station id (e.g. `nts`, `noods`, `dublab`).

- If `stations/<id>.md` exists, read it for prior state.
- If it does not exist, copy `stations/_template.md` to `stations/<id>.md` and ask the user for the homepage URL before proceeding.

## State machine

Update the `status:` frontmatter as you progress:

- `pending` — never touched
- `researching` — actively investigating, partial info
- `extracted` — candidate stream URL(s) found, not yet verified
- `verified` — curl confirms it streams correctly
- `added` — entry exists in `stations.json` and is wired up
- `broken` — could not crack from public site, parked

Always set `last_checked:` to today's ISO date when you touch a station.

## Playbook

1. **Read state.** Open `stations/<id>.md`. Note channels listed and any prior extraction attempts. Set status to `researching`.
2. **Fetch the homepage** with WebFetch. Save the response or the relevant excerpts.
3. **Grep the obvious** in the HTML/inline JS:
   - File extensions: `.mp3`, `.m3u8`, `.aac`, `.ogg`, `.pls`
   - Words: `stream`, `audio.src`, `new Audio(`, `<audio`, `icecast`, `shoutcast`, `hls`
   - Embedded JSON: `__NUXT__`, `__NEXT_DATA__`, `__INITIAL_STATE__`, `<script type="application/json">`
4. **Common stream hosts** to recognise: `radiocult.fm`, `radio.co`, `s2.radio.co`, `stream.zeno.fm`, `streamingv2.shoutcast.com`, AWS CloudFront/MediaPackage, `live.streamhoster.com`, `streema.com`.
5. **Fallback paths** if the homepage is opaque: `/embed`, `/player`, `/api/`, `/feed`, `/wp-json/`, RSS, `<link rel>` tags. Some stations expose their player via a separate subdomain (e.g. `player.<site>`).
6. **Verify with curl** before claiming a URL works:
   ```sh
   curl -I -L <url>
   curl -r 0-2048 -o /dev/null -L -w 'http=%{http_code} ct=%{content_type} cors=%{header_json}\n' <url>
   ```
   Confirm:
   - HTTP 200 (or follows redirects to one)
   - Content-Type is `audio/mpeg`, `audio/aac`, `audio/ogg`, or `application/vnd.apple.mpegurl` (HLS)
   - URL scheme is `https` (otherwise the relay is mandatory, not optional — note this in the MD)
   - Note presence/absence of `Access-Control-Allow-Origin` (matters for Web Audio API, not for `<audio>`)
7. **Update `stations/<id>.md`** with everything found:
   - Tick channels, fill stream URLs
   - Fill the Verification block (format, scheme, CORS, mixed-content risk)
   - In Extraction notes, write *where* the URL was hidden — this is the durable lesson
   - Bump status (`extracted` after step 6, `verified` once curl is clean)
8. **Write `stations/<id>.json`** (only if verified), one file per channel. Schema:
   ```json
   {
     "id": "nts-1",
     "station": "NTS",
     "channel": "1",
     "city": "London",
     "url": "https://stream.example.com/...",
     "format": "aac",
     "color": "#ff0000"
   }
   ```
   Then add the new id to `stations/_order.json` at the position where it should appear in the UI list (preserve existing order — usually append, or group with sibling channels). After writing, set the MD status to `added` and run `bun run build:stations` to regenerate `public/stations.json` (the static artifact deployed alongside the rest of `public/`).
9. **Update the "Patterns we've seen" section below** with anything new and reusable. This is how the skill gets sharper.

## Patterns we've seen

(Append findings here after each station — what worked, what didn't. Keep entries short.)

- **WebFetch's summary often hides stream URLs.** It thinks marketing pages have "no streaming infrastructure" while raw HTML has them inline. Always follow up WebFetch with `curl -sL <url> -o /tmp/x.html && grep -ioE '.mp3|.m3u8|.aac|stream' /tmp/x.html`. NTS hid both main streams in an inline JSON `"streams":{"nts1":{...}}` blob that the summary dropped.
- **Stream-relay vs edge URLs.** Stations often expose a stable relay host (e.g. `stream-relay-geo.ntslive.net/stream`) that 302s to a rotating edge (`audio-edge-*.radiomast.io`). Always store the relay URL, never the edge — the edge changes per request/region.
- **Radiomast.io (`streams.radiomast.io`, `audio-edge-*.h.radiomast.io`)** is a hosted streaming provider (used by NTS). Returns `audio/mpeg` (MP3) with permissive CORS. `curl --max-time` exits 28 on live streams even when working — read the `-w` headers, not the exit code.
- **One station can map to many channels.** NTS exposes 2 live channels + 16 always-on "Infinite Mixtapes". Each mixtape is its own relay (`stream-mixtape-geo.ntslive.net/mixtapeN`) with a title in the same JSON blob. When a station has bundles like this, ask the user before bulk-adding, then map title→relay with one grep over `"title":"..."[^{}]{0,800}<relay-pattern>`.
- **SPA shells with empty HTML.** If `curl` returns ~2KB containing only `<div id="app"></div>` and Vue/React/Next bundle preloads, the streams are baked into the JS. Pull the bundles (look for `<script src=/js/app.*.js>` and `chunk-vendors.*.js`) and grep them. Dublab's audio URL was hardcoded as `var Rr="https://dublab.out.airtime.pro/dublab_a"` in app.js.
- **Audio stream and video stream may live on different hosts.** Dublab's video player loads HLS from Livepeer (`livepeercdn.studio/hls/<id>/index.m3u8`); the actual radio audio is on Airtime.pro Icecast. Don't grab the first stream URL you find — confirm which one is *audio* by checking the player JS context (`<video>` ref vs `<audio>`) or content-type.
- **Livepeer "Stream open failed" ≠ broken URL.** A Livepeer manifest returning `#EXT-X-ERROR: Stream open failed` + `#EXT-X-ENDLIST` just means the broadcaster is offline at that moment. The URL is still correct.
- **Airtime.pro (`*.out.airtime.pro/<id>_a`)** is a hosted Icecast 2.4 install (Sourcefabric Airtime). Returns MP3 (audio/mpeg), permissive CORS, exposes `icy-name`/`icy-description`/`icy-br` headers — useful for sanity-checking you have the right station.
- **Next.js sites: try the HTML before the chunks.** Even when you see `/_next/static/chunks/` script tags, Next.js often serializes the player config into the page itself (look for `"src":[{"type":"hls","src":"..."}]` patterns). The Lot Radio's HLS URL was inline — the `_next` chunks weren't needed.
- **Livepeer-as-radio is a recurring pattern.** Live-from-a-physical-space stations (Dublab, The Lot Radio) increasingly use Livepeer HLS for combined video+audio. Expect "stream offline" responses outside broadcast hours — store the URL anyway, that's the canonical id.
- **GraphQL-backed SPAs: query the API, don't grind the bundle.** When the JS references a GraphQL endpoint (e.g. `apollo`, `uri:"https://api.<site>/graphql"`), grep the bundle for `query` literals and look for a `nowPlaying`/`onair`/`live`/`stream` query that returns a URL field. POST it directly with `curl` — far cleaner than hunting URLs in minified chunks. LYL's `query { onair { title hls } }` returned the canonical HLS master in one request.
- **Player classNames lie.** LYL's player div had `className: "airtime"` with a `"mixcloud"` fallback, but the actual stream host was neither — it was a self-hosted Nginx HLS origin at `radio.lyl.live`. ClassNames identify *which player component to render*, not where the bytes come from. Confirm via the resolved URL, not internal labels.
- **HLS master vs variant.** Self-hosted Nginx HLS origins commonly expose a master playlist (`live.m3u8`) listing AAC variants (`aac_lofi.m3u8` / `aac_midfi.m3u8` / `aac_hifi.m3u8`). Always store the master — the player picks the bitrate.
- **Radiocult.fm (`<station>.radiocult.fm/stream`)** is a hosted Icecast service (server header `Icecast`). Returns MP3 (`audio/mpeg`), permissive CORS, exposes `icy-name`/`icy-br`. Used by Noods Radio at 192 kbps. Subdomain pattern is the slugged station name.
- **Server-rendered sites: just grep the HTML.** Don't reflexively reach for the JS bundles. Noods Radio's HTML was ~580KB with the stream URL inline in a plain `<audio src="...">` tag. Always start with `grep -ioE '<audio[^>]*src="[^"]*"' /tmp/x.html` before chasing chunks.
- **`<audio src>` can be stale — verify DNS first.** Before treating an inline `<audio src>` as canonical, run `dig +short <host>`. Threads Radio's homepage still listed `https://threads.out.airtime.pro/threads_a` but DNS returns NXDOMAIN (other Airtime tenants resolve fine, so it's the tenant that's gone, not the provider). When you find a stream URL whose host doesn't resolve, the station is `broken`, not `verified` — don't add it to `stations.json` no matter how confident the HTML looks.
- **`/embed` ≠ audio stream.** Some stations' `/embed` endpoint returns a Twitch (or YouTube) iframe for the video feed of their physical studio, not an audio-only relay. Twitch HLS URLs are token-gated and redistribution violates ToS — if the only live surface is a Twitch channel, mark the station `broken` rather than scraping Twitch.
- **`_app` bundle is the first place to look on Next.js radio sites.** The outer `_app` chunk initialises the player context with `streamingUrl` props — much easier to grep than page-specific chunks. Kiosk Radio's live Icecast URL (`kioskradiobxl.out.airtime.pro/kioskradiobxl_b`) was right there, alongside two streamnerd HLS URLs that turned out to be 404 (unused/deprecated). Always verify all candidates rather than stopping at the first hit.
- **Airtime `_b` suffix = AAC, not MP3.** Airtime/Sourcefabric Icecast mounts follow `<tenant>_a` (MP3) / `<tenant>_b` (AAC). Kiosk Radio serves only AAC (`audio/aac`, 192 kbps). Check the content-type header to confirm.

## What to do if stuck

- Try one or two fallbacks beyond step 5; don't grind for hours.
- Mark the station `broken`, write what was tried in the Extraction notes, and stop.
- **Do not guess URLs.** A wrong URL is worse than no URL.

## Output to the user

A short summary: station id, channels found, URLs (truncated), new status, and one sentence on what the durable lesson was for the skill.

## Out of scope

- Do not bulk-research multiple stations in one invocation.
- Do not write frontend or relay code from this skill — only the station MD, the per-station `stations/<id>.json`, and `stations/_order.json` (plus the regenerated `stations.json`).
- Do not invent channels the user didn't ask for; if you discover extra channels on a station, list them as candidates and ask before adding.

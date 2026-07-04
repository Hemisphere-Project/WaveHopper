---
id: vizi
name: VIZI Radio
site: https://viziradio.com/live-new.php
status: broken
last_checked: 2026-05-03
---

## Channels
- [ ] main — stream URL: ? (site is offline / "Coming Soon")

## Extraction notes

Site is currently a "Coming Soon" placeholder. All requested paths return the same parked page:

```
GET /live-new.php  -> 200, redirects to /
GET /live          -> 200, redirects to /
GET /listen        -> 200, redirects to /
GET /player        -> 200, redirects to /
GET /embed         -> 200, redirects to /
GET /stream        -> 200, redirects to /
```

The page (`<title>Coming Soon</title>`, contains "under construction") is ~570KB but it's mostly a base64-inlined favicon and a static splash. No `<audio>`, `<iframe>`, or stream URLs in the markup. Grepped for `mp3|m3u8|aac|ogg|stream|icecast|radiocult|radio.co|airtime|hls` — zero hits.

Subdomains checked (all NXDOMAIN via 1.1.1.1):

- `stream.viziradio.com`
- `listen.viziradio.com`
- `radio.viziradio.com`
- `player.viziradio.com`
- `live.viziradio.com`
- `panel.viziradio.com`
- `api.viziradio.com`

DNS for `viziradio.com` itself is just two Cloudflare anycast IPs with no MX or TXT — minimally configured, consistent with a domain parked behind a static landing.

## Verification

- Format: n/a
- Scheme: n/a
- CORS: n/a
- Mixed-content risk: n/a

## Open questions

- Is VIZI actually launched yet? The URL we were given (`/live-new.php`) suggests there may have been a previous live page that's been pulled. Worth re-checking once the site goes out of "Coming Soon" — at which point a homepage refetch + grep should expose the stream URL.
- If they relaunch on a different domain, this entry will need a new `site:` value.

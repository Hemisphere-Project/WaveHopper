---
id: threads
name: Threads Radio
site: https://threadsradio.com/
status: broken
last_checked: 2026-05-03
---

## Channels
- [ ] main — stream URL: ? (canonical URL on the page is dead, see below)

## Extraction notes

Site is Next.js, server-rendered. Stream URL is inline in the HTML:

```html
<audio class="react-audio-player" preload="none"
       src="https://threads.out.airtime.pro/threads_a"
       title="https://threads.out.airtime.pro/threads_a">
```

But this URL no longer resolves:

```
$ dig +short threads.out.airtime.pro @1.1.1.1   # empty
$ dig +short threads.out.airtime.pro @8.8.8.8   # NXDOMAIN
```

Other Airtime tenants on the same provider still resolve fine (`dublab.out.airtime.pro` → `p10.a.airtime.pro` → `176.9.17.170`), so the parent service is up — it's specifically the `threads` tenant that's gone. Likely an account cancellation or rebranded slug; the homepage HTML hasn't been updated to reflect this.

### Fallbacks tried (all dead ends)

- `/player` → 404
- `/listen` → renders the same homepage (audio src still the dead Airtime URL)
- `/embed` → returns a Twitch iframe pointing at `https://player.twitch.tv/?channel=purizu_` (video, not an audio-only stream we can redistribute)
- `/api/stream`, `/feed` → 404
- `api.threadsradio.com`, `stream.threadsradio.com` → no DNS
- `threadsradio.out.airtime.pro`, `threads-radio.out.airtime.pro` (slug variants) → NXDOMAIN
- `__NEXT_DATA__` blob has no stream URL field
- All Next.js JS chunks (main, _app, pages/index, pages/embed, 519, 700) grepped for `airtime|stream|m3u8|hls|radio.co|icecast|cult` — only matches were generic streaming utilities and that one Twitch embed string

### Twitch as the new home?

`/embed` pointing at Twitch suggests they may have moved live programming there. Twitch can produce HLS audio-only streams via their internal API (`/api/channel/hls/<chan>.m3u8` with a token), but those URLs are token-gated, redistribution is against ToS, and they aren't the kind of stable radio relay we want in `stations.json`.

## Verification

- Format: n/a (no working URL)
- Scheme: n/a
- CORS: n/a
- Mixed-content risk: n/a

## Open questions

- What is Threads' current canonical audio relay? Worth re-checking in a few weeks: maybe the homepage `<audio src>` will get updated, or someone publishes the new endpoint on social.
- If they've fully migrated to Twitch and dropped the standalone audio stream, we should drop Threads from the catalog rather than try to scrape Twitch.

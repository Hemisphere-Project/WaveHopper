---
name: import-now-playing
description: Extract and verify the now-playing metadata source for a single WaveHopper station, then update its station MD and per-station JSON nowPlaying block. Use when the user asks to import, refresh, or re-check a station's now-playing info. Argument is the station id (e.g. /import-now-playing nts-1).
---

# Import Now-Playing

You are extending WaveHopper's metadata layer by cracking the **now-playing source** of one station. Process exactly one station per invocation.

This skill is the metadata-layer sibling of `/import-station`. The audio stream URL is already known by the time you run this skill — you are finding the *separate* endpoint that exposes "what is currently airing."

## Inputs

The user invokes this skill with a station id (e.g. `nts-1`, `dublab`, `noods`).

- The station must already exist in `stations/<id>.json` with status `added` in its MD. If not, point the user to `/import-station` first.
- If the MD lacks a `## Now-playing` section, add one (template is in `stations/_template.md`).

## Output

This skill writes:
- A `nowPlaying` block in `stations/<id>.json` with at minimum `{ "type": "<type>" }`. Optional per-type fields (e.g. an `endpoint` URL for Airtime) can sit alongside.
- A filled-out `## Now-playing` section in `stations/<id>.md` describing endpoint, mapping, cache key, and any caveats.
- A new fetcher at `public/api/fetchers/<type>.php` if the type doesn't exist yet (function name: `wh_fetch_nowplaying_<type>`, dashes in the type become underscores in the function name).
- An updated `public/stations.json` via `bun run build:stations`.

If the station has no usable metadata source, write `"nowPlaying": { "type": "none" }` — that's the explicit "we checked, there's nothing" marker. The dispatcher returns 204 and the frontend hides the card.

## Normalized response shape

Every fetcher must return either `null` (nothing right now) or an array shaped like:

```php
[
    'title'    => 'Joy Orbison',         // primary display line — track title OR show host
    'subtitle' => 'Daytime',             // secondary line — artist OR show slot, may be null
    'starts'   => '2026-05-04T14:00:00Z',// ISO 8601 or null
    'ends'     => '2026-05-04T16:00:00Z',// ISO 8601 or null
    'next'     => ['title' => '...', 'starts' => '...'], // optional
]
```

The dispatcher injects `source` and `fetchedAt` afterward. **Don't add other top-level fields** — the frontend only reads these. If you need station-specific data, put it under a `details` sub-object and the frontend will ignore it until someone wires it.

## Known source types

| type         | Used by         | Shape                                                              |
|--------------|-----------------|--------------------------------------------------------------------|
| `nts`        | nts-1, nts-2    | One JSON endpoint covers both channels; cache key `nts`            |
| `airtime`    | dublab (likely) | `/api/live-info-v2` on the airtime tenant; per-station cache key   |
| `radiocult`  | noods (likely)  | Radiocult schedule API; per-station cache key                      |
| `lyl-graphql`| lyl             | POST `{ onair { title } }` to the LYL GraphQL endpoint             |
| `hls-id3`    | thelot (likely) | Client-side ID3 parsing via `Hls.Events.FRAG_PARSING_METADATA` — no fetcher needed; the type signals to the frontend to switch on ID3 mode |
| `none`       | NTS Mixtapes    | Explicit "no source" marker. No fetcher invoked.                   |

## Playbook

1. **Read state.** Open `stations/<id>.md`. Check the `## Now-playing` section if it exists.
2. **Identify a candidate source.** In rough order of preference:
   - The station's homepage often surfaces the now-playing endpoint via XHR/fetch in DevTools-equivalent inspection — `curl -sL <site>` and grep for `now`, `playing`, `live`, `metadata`, `current`, `track`, `schedule`.
   - Common provider patterns:
     - **Airtime.pro**: `https://<tenant>.airtime.pro/api/live-info-v2` (CORS may or may not be open, but PHP doesn't care).
     - **Radiocult.fm**: `https://api.radiocult.fm/api/station/<slug>/...` — check their docs.
     - **Custom GraphQL**: grep the bundle for `query` + `nowPlaying|onair|live`.
     - **HLS ID3**: if the audio is HLS and grep finds nothing else, assume `hls-id3` and let the frontend parse.
3. **Verify with curl, server-side.** Run the candidate endpoint from this machine; record the response JSON in a scratch file. Confirm it includes a current-show field and (ideally) start/end times.
4. **Map to the normalized shape.** Decide what becomes `title` vs `subtitle`. The rule:
   - If the source has per-*track* info (artist + song): track → `title`, artist → `subtitle`.
   - If the source has only per-*show* info: show host → `title`, slot/program name → `subtitle` (or null if redundant).
5. **Write the fetcher** at `public/api/fetchers/<type>.php`. Cache TTL: 30s for show-level data, 20s for per-track data. If one upstream call serves multiple stations (NTS pattern), use a shared cache key and pick by `station.channel` or `station.id`.
6. **Update `stations/<id>.json`** — add the `nowPlaying` block.
7. **Update `stations/<id>.md`** — fill the `## Now-playing` section (endpoint, mapping, cache key, caveats).
8. **Run `bun run build:stations`** to regenerate `public/stations.json`.
9. **Update "Patterns we've seen" below** with anything new and reusable.

## Verification on the deployed host

The PHP backend only runs on the deployed shared host — there's no dev-server parity by design. After deploying:

```sh
curl -sS "https://<host>/api/now-playing.php?id=<station-id>" | jq .
```

Expected: a JSON object with `title`, `source`, `fetchedAt`, and at minimum one timestamp. 204 means "nothing playing" or "type=none". 4xx/5xx is a bug in the fetcher or the station JSON.

## Patterns we've seen

(Append findings here after each station — what worked, what didn't. Keep entries short.)

- **NTS `/api/v2/live` returns both channels in one shot.** Cache key `nts` is shared across nts-1 / nts-2 so a single upstream fetch serves both. The dispatcher picks per channel by `station.channel` (`"1"` / `"2"`). Mixtapes have no per-track metadata exposed — they get `"type": "none"` (and currently are absent altogether, which is also fine).

## Out of scope

- Do not touch the audio stream URL — that's `/import-station` territory.
- Do not bulk-research multiple stations.
- Do not invent "track" data when the source only has show data; leave `subtitle` null and let the frontend render show times instead.
- Do not skip the cache. Shared hosting can't take per-listener upstream traffic.

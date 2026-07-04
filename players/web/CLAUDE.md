# CLAUDE.md — web player (PWA + PHP API)

Working rules for AI agents in this directory. Repo-wide picture: root
`CLAUDE.md`. Normative cross-player contract: `docs/CONTENT-API.md`.

## Scope fence

- App code lives in `public/` (vanilla JS, no build step) and the now-playing
  API in `public/api/` (PHP). Station data and icons are **not** edited here —
  they come from `content/` via `python3 tools/build.py`.
- `public/stations.json`, `public/img/stations/*.png`, and everything under
  `public/content/` are **build artifacts** (committed, but generated). Edit
  `tools/build.py` or `content/`, never the artifacts.
- This docroot is the authoritative server for every other player. URL paths
  and response schemas that shipped clients consume (see CONTENT-API.md) are
  append-only: renaming/removing any of them is a breaking change requiring a
  schemaVersion bump and a shipped-client audit.

## PWA invariants

- **`APP_VERSION`** (in both `sw.js` and `app.js`, keep them equal): bump on
  every deploy that changes the shell (HTML/JS/CSS/manifest). Do NOT bump for
  content-only changes (station edits) — those flow through network-first
  fetches without a new cache namespace.
- `sw.js` hardcodes the cached asset sets. A new locally-hosted station icon
  must be added to `SWR_ASSETS`; removing one must remove it there too. The
  `/import-station` skill handles this.
- Never intercept cross-origin requests in the service worker — audio streams
  must pass through untouched.
- The app must keep working with an *older* cached shell against a *newer*
  `stations.json` (and vice versa): station-object changes are additive only;
  the frontend ignores unknown fields.

## Dev loop

```sh
python3 tools/build.py                       # after any content/ change
php -S 127.0.0.1:3000 -t players/web/public  # from the repo root
```

The PHP built-in server runs `api/*.php` the same way nginx+FPM does in prod.
`api/cache/` must be writable. Deploy = rsync `players/web/public/` to the
docroot (details in README.md §Deploy).

## Server config

`public/.htaccess` carries the Apache rules (https redirect, MIME types,
cache policy — including `must-revalidate` for `/content/**/manifest.json`
and `application/octet-stream` for `.bin`). The production nginx config must
mirror any change made there, and vice versa.

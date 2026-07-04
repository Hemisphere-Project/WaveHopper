# CLAUDE.md — WaveHopper (repo root)

Curated webradio catalog + multiple players. One content source, one
authoritative server (`https://waverz.net`), several clients that sync from it.

## Map

| Path | What | Rules live in |
|---|---|---|
| `content/` | **Source of truth**: stations (`<id>.json` + `<id>.md`), icons, `_order.json` | this file |
| `players/web/` | PWA + PHP API — the deployable, authoritative docroot | `players/web/CLAUDE.md` |
| `players/m5cores3/` | M5Stack CoreS3 firmware (PlatformIO) | `players/m5cores3/CLAUDE.md` |
| `players/mobile/` | Capacitor iOS/Android shell (placeholder) | `players/mobile/CLAUDE.md` |
| `tools/build.py` | `content/` → all committed artifacts | its docstring |
| `docs/CONTENT-API.md` | **Normative cross-player contract** | — |
| `docs/ARCHITECTURE.md` | The why + update matrix | — |

When working on a specific player, follow that player's CLAUDE.md — each one
starts with a scope fence. Cross-player work (schemas, URLs, build pipeline)
starts by editing `docs/CONTENT-API.md`.

## The one rule that outranks everything

Shipped clients (installed PWAs, store apps, flashed devices) consume the URL
paths and JSON schemas in `docs/CONTENT-API.md` and cannot be forced to
update. **That contract is append-only.** Adding optional fields is fine;
renaming, removing, or repurposing anything requires a `schemaVersion` bump
and an audit of every shipped client. All clients ignore unknown JSON keys.

## Everyday commands

```sh
python3 tools/build.py                        # rebuild artifacts after content/ edits
python3 tools/build.py --seed-m5              # + refresh the m5 LittleFS seed
php -S 127.0.0.1:3000 -t players/web/public   # local web dev (PHP API works)
grep -H '^status:' content/stations/*.md      # station research dashboard
```

Committed build artifacts (`players/web/public/stations.json`,
`…/img/stations/*.png`, `…/content/**`) are generated — never hand-edit them.
The build is idempotent; running it with no content changes must produce no
diff (if it doesn't, that's a bug in `tools/build.py`).

Pushing to `main` auto-deploys the docroot to production (Infomaniak) via
`.github/workflows/deploy.yml` when `players/web/public/**` changed — CI
re-runs the build and refuses to deploy artifacts that don't match `content/`.
Anything you commit under the docroot goes live on push.

## Station workflow

One station at a time, via the skills: `/import-station <id>` (stream URL,
research notes, icon) and `/import-now-playing <id>` (metadata source). They
maintain `content/stations/` and know the downstream steps (build, sw.js
asset list).

# WaveHopper

Web Radio aggregate, select and play.

A simple browser front-end that aggregates a curated list of webradio streams behind a unified player. Vanilla JS frontend, thin Bun relay (HTTPS-upgrade + CORS + optional metadata), nginx reverse proxy with SSL in front.

## Layout

```
WaveHopper/
├── public/                    Frontend (HTML/CSS/JS, served as static)
├── server/relay.ts            Bun relay (stream proxying, CORS)
├── stations.json              Source of truth — one entry per channel
├── stations/                  Per-station research notes (Markdown)
│   ├── _template.md           Template for new stations
│   └── <id>.md                One file per station
└── .claude/skills/
    └── import-station/        The /import-station skill
```

## Station data model

`stations.json` is a flat array of channels — multi-channel stations (e.g. NTS) appear as multiple entries:

```json
[
  {
    "id": "nts-1",
    "station": "NTS",
    "channel": "1",
    "city": "London",
    "url": "https://stream.example.com/...",
    "format": "aac",
    "color": "#ff0000"
  }
]
```

Each station also has a sibling Markdown file at `stations/<id>.md` tracking research progress with frontmatter:

```yaml
---
id: nts
name: NTS Live
site: https://www.nts.live/
status: pending
last_checked:
---
```

### Status state machine

| Status        | Meaning                                                    |
|---------------|------------------------------------------------------------|
| `pending`     | Never touched                                              |
| `researching` | Actively investigating, partial info                       |
| `extracted`   | Candidate stream URL(s) found, not yet verified            |
| `verified`    | `curl` confirms it streams correctly                       |
| `added`       | Entry exists in `stations.json` and is wired up            |
| `broken`      | Could not crack from public site, parked                   |

## The `/import-station` skill

A project skill at `.claude/skills/import-station/SKILL.md` automates extracting a single station's stream URL. It is iterative by design: process one station at a time so progress survives interrupted sessions, and the skill's "Patterns we've seen" section sharpens after each station.

### Usage in Claude Code

Adding a brand new station:

```
/import-station <id>
```

If `stations/<id>.md` does not exist, the skill copies the template and asks for the homepage URL before proceeding.

Resuming a previously parked station (e.g. one in `researching` or `broken` status):

```
/import-station <id>
```

Same command — the skill reads the existing MD, sees prior attempts in the Extraction notes, and continues from there.

Refreshing a `verified`/`added` station whose stream URL has rotted:

```
/import-station <id>
```

Same again — re-runs verification, updates `last_checked`, and rewrites the URL if it changed.

### Checking overall progress

Quick dashboard from the shell:

```sh
grep -H '^status:' stations/*.md
```

Or to see what's left:

```sh
grep -l 'status: pending\|status: researching\|status: broken' stations/*.md
```

### Adding a station that isn't in the list yet

1. Run `/import-station <new-id>` — the skill will scaffold `stations/<new-id>.md` from the template.
2. Provide the homepage URL when asked.
3. The skill takes it from there.

## Initial station list

Curated kickoff set:

| id        | Station         | Site                                  |
|-----------|-----------------|---------------------------------------|
| `nts`     | NTS Live        | https://www.nts.live/                 |
| `dublab`  | Dublab          | https://www.dublab.com/               |
| `thelot`  | The Lot Radio   | https://www.thelotradio.com/          |
| `threads` | Threads Radio   | https://threadsradio.com/             |
| `noods`   | Noods Radio     | https://noodsradio.com/               |
| `lyl`     | LYL Radio       | https://lyl.live/                     |
| `vizi`    | VIZI Radio      | https://viziradio.com/live-new.php    |

## Development

```sh
bun install
bun run dev        # relay with hot reload on :3000
```

The frontend (`public/`) is static and can be served by nginx in production; the relay handles `/stream/:id` and (later) `/now-playing/:id`.

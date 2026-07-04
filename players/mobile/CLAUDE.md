# CLAUDE.md — mobile player (Capacitor)

Working rules for AI agents in this directory. Repo-wide picture: root
`CLAUDE.md`. Normative cross-player contract: `docs/CONTENT-API.md`.

## Scope fence

- Work **only inside `players/mobile/`**. The app's actual UI/player code
  lives in `players/web/` — if a change is needed there for mobile (e.g. a
  WebView quirk), it must keep the PWA working and follow
  `players/web/CLAUDE.md` (including `APP_VERSION` rules); coordinate, don't
  fork.
- Anything touching server URLs or schemas goes through `docs/CONTENT-API.md`
  first (append-only for shipped clients).

## Current state

Placeholder only — Capacitor is chosen (see README.md decision record) but
`npx cap init` has not been run. When starting real work here: initialize the
Capacitor project in this directory, keep the shell thin (plugin glue +
config, no app logic), and update README.md + this file with the real build
commands.

## Principles

- The shell must stay dumb: content and player logic come from
  `https://waverz.net` so web deploys update the app without store releases.
- Store binaries are the only thing app releases deliver — background-audio
  glue, media-session bridging, icons, splash.
- Test background playback (screen locked, app backgrounded, Bluetooth
  controls) on both platforms before shipping anything.

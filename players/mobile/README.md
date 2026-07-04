# WaveHopper — mobile apps (iOS / Android)

**Status: not initialized.** This directory is a placeholder plus the decision
record; app work starts after the M5 player stabilizes.

## Decision record (2026-07-04)

**Chosen: [Capacitor](https://capacitorjs.com/).** The app is the existing web
player (`players/web/public/`) wrapped in a WebView shell, with native plugins
only where the web platform falls short inside a store app.

Considered and rejected:

- **Cordova** — same wrapper model, but the ecosystem is in maintenance
  decline (unmaintained plugins, slow platform updates). Capacitor is its
  actively-maintained successor from the Ionic team.
- **Native players (Kotlin/Swift)** — best battery/lock-screen behavior, but
  two extra codebases with near-zero UI sharing. Contradicts the
  minimum-firmware-surface goal.

## Update model

- **Binary**: App Store / Play Store releases. Keep the shell thin so these
  are rare (plugin glue + config only, no app logic).
- **Content & code**: the WebView loads the same authoritative endpoints as
  the PWA (`https://waverz.net` — `/stations.json`, `/img/stations/*`,
  `/api/now-playing.php`). Web deploys update the mobile experience instantly
  with no store release. See [docs/CONTENT-API.md](../../docs/CONTENT-API.md).

## Known work items for initialization

- `npx cap init` + `npx cap add android ios`; point the WebView at the remote
  URL (or bundle the shell and load content remotely — decide with store
  review rules in mind).
- Background audio: iOS needs the `audio` background mode + a correctly
  configured `AVAudioSession`; Android needs a foreground service with a
  MediaSession. Evaluate `@capacitor-community` audio/media-session plugins vs
  thin custom glue before adding dependencies.
- Media Session lock-screen controls already work in the web code
  (`navigator.mediaSession`) — verify they surface through the WebView on both
  platforms before writing anything native.
- Store assets: icons/splash from `players/web/public/img/favicon/` sources.

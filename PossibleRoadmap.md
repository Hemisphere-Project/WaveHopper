A. Catalog growth & rot detection
Problem: 21 channels is a strong start, but the catalog will rot — Threads is already dead, Vizi is parked, NTS rotates mixtape numbers periodically.

More stations: Rinse FM, KEXP, Soho Radio, Worldwide FM, KCRW, BBC 6 Music (if EU-allowed), local picks. Just /import-station runs.
/check-station <id> skill — separate from import. Re-runs the verification step (HEAD + small range GET, parse manifest if HLS) and flips last_checked. Cheaper than re-importing.
Scheduled health check — a routine (cron-style scheduled agent) that runs /check-station over every added station nightly, flags any that fail, opens an issue or just edits the MD status: broken. Catches rot before users do.
Question for you: is the scheduled routine worth the setup, or does manual /check-station weekly feel fine for now?

B. Now-playing metadata (the deferred v2 thing)
Heterogeneous, station-by-station:

NTS: https://www.nts.live/api/v2/live — clean JSON, both channels' current shows.
Dublab (Airtime.pro): /api/live-info-v2 — standard Airtime endpoint.
Noods (Radiocult): Radiocult has a public schedule API.
LYL: the { onair { title } } GraphQL query we already reverse-engineered.
The Lot (HLS): probably ID3-in-segments → Hls.Events.FRAG_PARSING_METADATA.
NTS Mixtapes: probably no per-track metadata (they're auto-curated). Show the mixtape name only.
Pattern: each station entry gets an optional nowPlaying: { type: 'nts' | 'airtime' | 'radiocult' | 'lyl-graphql' | 'hls-id3' } discriminator. Frontend has one fetcher per type, polls every 30s while playing, and updates the now-playing card + Media Session metadata.

CORS may bite for some endpoints (Airtime is iffy). If so, a thin Bun proxy comes back into scope — but only for the metadata endpoints, never the audio stream. The static-only deploy stays for audio.

Tradeoff: moderate code, real user value. This is the single feature that pushes the player from "useful" to "delightful."

C. UX polish
In rough effort order:

Default-curated subset — you already started this with defaultDisabled in app.js. Mark all 16 NTS mixtapes defaultDisabled: true in stations.json so the first-run list is ~7 stations, not 21. User opts into the mixtapes via config.
Long-press on a row in player mode → quick toggle disable (no need to enter config for a one-off).
Sleep timer — auto-pause after 30/60/90 min. Standard for radio apps.
Swipe-left on now-playing card → NEXT — natural mobile gesture.
Per-station logo — optional logo: field in stations.json pointing to a stations/<id>.png. Shown on the now-playing card and in Media Session artwork (so the lock screen shows NTS yellow logo, not our equalizer). Nice but starts to make the catalog feel heavy.
Search box / tag filter — only worth it once we cross ~40 stations.
Drag-to-reorder in config mode — a lot of code for a nice-to-have.
D. Sharing & deep links
?s=<station-id> URL param → opens directly playing that station. Nearly free.
Web Share API "share what I'm listening to". Free.
Embed mode (?embed=1&s=nts-1) — a stripped-down view for blogs etc. Useful only if you actually want this.
E. Background reliability hardening (incremental)
The current code handles visibility + online events. Could add:

Currentime-stuck watcher: if audio.currentTime doesn't advance for 8s while we think we're playing, reattach.
HLS soft-recovery: hls.startLoad() once on NETWORK_ERROR before declaring fatal.
Auto-resume after long sleep: when phone wakes, the OS may have killed the audio element. Detect and silently reattach if user hadn't paused.
Cheap and useful, but invisible until something breaks.

F. Aesthetic stretch
Optional CRT scanline overlay — toggle in config. Easy via a fixed-position SVG/canvas overlay.
Animated equalizer on the now-playing card when foregrounded (Web Audio AnalyserNode → small canvas). Works only foregrounded since AudioContext suspends on backgrounding — so not a regression to background playback, but a foreground-only flourish.
Press Start 2P for the WAVEHOPPER header only, VT323 for everything else — adds one webfont for the logo.
G. Repo / dev workflow
Per-station JSON files instead of monolithic stations.json — better git diffs when adding stations, easier merge. Tiny build step concatenates them. Probably not worth it until 50+ stations.
/refresh-broken skill — wraps /import-station over all status: broken MDs. Useful for periodic recovery sweeps.
CI / GitHub Action that runs the health check and fails if any station has rotted — only relevant if this becomes a multi-contributor project.
My recommendation for the next batch
Three things, in order:

B. Now-playing metadata — biggest UX leap, and the data is already structured (one of 4-5 known endpoint shapes).
A. /check-station skill + a small defaultDisabled curation pass — keeps the catalog honest, and uses your existing defaultDisabled field properly.
C. Sleep timer + ?s=<id> deep link — small surface, real value, no architecture decisions.
I'd defer scheduled cron routines, drag-reorder, CRT overlay, search/tags, and per-station logos until you've actually used the player for a few weeks and know which gap stings most.

Questions to lock the plan:

Is now-playing metadata the right next big swing, or do you want catalog/curation first?
For metadata: ok with a small Bun proxy only for metadata endpoints if some lack CORS, or do you want to keep "no server code" as a hard rule (and skip those stations' now-playing)?
Default-disabled: which stations should be on by default? My instinct = NTS 1, NTS 2, Dublab, Noods, LYL, The Lot — leave the 16 mixtapes opt-in.
Anything you're already itching to add that I missed?
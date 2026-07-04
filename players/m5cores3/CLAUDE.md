# CLAUDE.md — M5Stack CoreS3 player

Working rules for AI agents developing this firmware. The repo-wide picture is
in the root `CLAUDE.md`; the normative cross-player contract is
`docs/CONTENT-API.md`.

## Scope fence

- Work **only inside `players/m5cores3/`**. Never edit `content/`,
  `players/web/`, or `tools/build.py` from a firmware session.
- Anything that would change a URL, schema, or field consumed from the server
  is a contract change: it goes through `docs/CONTENT-API.md` first (append-only
  for shipped clients; breaking changes need a `schemaVersion` bump and a
  shipped-client audit).

## Contract summary (inline for convenience — CONTENT-API.md is normative)

- Base host: `https://waverz.net`, verified TLS (ISRG roots in
  `include/certs.h`, SNTP required first). Three endpoint families only:
  `/content/m5cores3/*`, `/content/firmware/m5cores3/*`,
  `/api/now-playing.php?id=<id>`. Audio streams go to arbitrary hosts
  (unverified — accepted trade-off).
- Content sync: **equality** on `contentVersion`, per-file sha256 diff,
  staged atomic commit — implemented in `content_sync.cpp`; don't reinvent.
- Firmware: update iff remote `build` (integer) **>** compiled `WH_FW_BUILD`.
- Ignore unknown JSON keys; reject `..`/absolute manifest paths; if remote
  content `schemaVersion` > supported, skip sync but still check firmware.
- The API responds **chunked** over HTTP/1.1: parse via `getString()`, never
  `getStream()`, for JSON endpoints. Pack files/binaries are static
  (Content-Length) — stream those.

## Toolchain — do not "fix" these

- `platform` is a **pinned pioarduino release URL** (official `espressif32` is
  stuck on Arduino core 2.x, which ESP32-audioI2S ≥ 3.x cannot compile
  against). pioarduino requires **Python ≤ 3.13**: the working CLI is
  `~/.platformio/penv-py313/bin/platformio`.
- ESP32-audioI2S and M5Module-Audio are pinned by git tag/commit (registry
  copies are stale). M5Unified and M5GFX move **together**.
- Compile gate: `pio run` (both envs). No device CI — every OTA-published
  binary gets hand-tested on the in-hand device first.
- PlatformIO reorders `-U`/`-D` build flags: never mix `${...}` inheritance
  with undef-then-redefine overrides (leaves macros undefined — bit us once).

## Hardware truths (paid for in debugging hours — do not rediscover)

- **AW88298 (internal amp, I2C 0x36)**: 16-bit registers written **MSB first**
  (M5Unified bswap16s before its pointer write — easy to misread). The AW9523
  (0x58) reg 0x02 bit2 rail **power-cycles the whole amp chip**.
- **ESP32-audioI2S outputs 32-bit I2S slots** → the amp's I2SCTRL (reg 0x06)
  needs I2SBCK=64*fs — base `0x1CE0`, *not* M5Unified's 16-bit-slot `0x14C0`.
  Wrong ratio = PLL never locks (SYSST=0x0000) = silence with perfect-looking
  registers.
- **The amp cannot lock the ESP32's fractional 44.1 kHz BCLK** and faults
  (SYSST.SWS bit8 drops) on any I2S clock reconfig. Both solved by pinning the
  output clock: `audio.setOutput48KHz(true)` (lib resamples). Keep the 5 s
  read-only SWS health check (power-cycle recovery) — and never rewrite a live
  amp register that hasn't changed (audible glitch).
- **I2S pinouts** (all share GPIO13 data; exactly one profile active):
  internal {BCLK 34, LRCK 33, DOUT 13}, RCA M125 {7, 0, 13}, Module Audio
  M144 {BCLK 0, LRCK 6, DOUT 13, **MCLK 7 mandatory**, pin switch on B}.
  `Audio::setPinout` MCLK default is -1/unused — passing 0 routes MCLK onto
  GPIO0.
- **Probe 0x33 only** for Module Audio (its STM32 helper). Never probe 0x10 —
  the internal BMM150 magnetometer answers there on every CoreS3.
- **Realtime-paced streams** (Icecast/Airtime/AzuraCast) leave ~3 KB of
  buffer = every wifi hiccup is an audible gap. The lib has no prebuffer API:
  `player.cpp` suspends the lib's decode task ("PeriodicTask") across
  `connecttohost` while `audio.loop()` fills a 48 KB cushion (3 s cap). NTS
  (CDN) doesn't need it but is harmless.
- ESP32-audioI2S 3.4.6 model: lib spawns the decode task at `setPinout`; the
  sketch must still pump `audio.loop()` every ~1–5 ms (all networking lives
  there — `player.cpp`'s task does this); events arrive via the single
  `Audio::audio_info_callback` **in the pumping task's context**;
  `connecttohost` blocks up to ~3 s (never call from the UI task); volume
  range 0..21; no legacy `audio_info()` weak callbacks anymore.
- A TLS handshake blocks ~0.5–1 s and needs ~50 KB heap: network calls run on
  worker tasks (`now_playing.cpp`) or at boot, never on the input loop; the
  one shared verified client is mutex-guarded (`net.cpp`); no keep-alive (the
  server drops idle connections and a parked session pins ~50 KB).
- USB-CDC: `Serial.begin()` is required for `Serial.print*` (log_* bypasses
  it). `pio device monitor` needs a TTY — use `scripts/serial_capture.py`
  from scripts/agents (it also does the proper DTR-low reset dance).
- LittleFS partition is labeled `littlefs`: mount with
  `LittleFS.begin(true, "/littlefs", 10, "littlefs")` (esp_littlefs defaults
  to the label "spiffs" and fails).
- UI matches the webapp's default Dark skin: palette constants in `ui.cpp`
  (bg #0a0a0a, fg #e8e8e8, station accent with dark accent-fg) and the real
  VT323 font embedded via `include/font_vt323.h` — GENERATED, don't edit;
  regenerate with `scripts/gen_gfxfont.py <VT323.ttf> include/font_vt323.h
  VT323 16 24 32` (grab the TTF from google/fonts ofl/vt323).

## Flash & filesystem

- `partitions.csv`: dual 4 MB OTA slots + ~7.9 MB LittleFS. **Offsets/sizes
  frozen once any device has OTA'd.** USB reflash is the only recovery from a
  table change. Erase just the FS:
  `esptool erase-region 0x810000 0x7E0000` (run it with
  `~/.platformio/penv/bin/python3 ~/.platformio/packages/tool-esptoolpy/esptool.py`).
- On-device layout: `/content/m5cores3/` pack mirror; `/content/.staging/`
  in-flight downloads; local `manifest.json` is the commit marker (absence ⇒
  full sync). Empty FS is valid — the device syncs itself.
- NVS namespace `wh`: `ssid`, `pass`, `last_st`, `vol` (0–21), `aout`
  (0 auto/1 internal/2 rca/3 module), `bright` — documented in config.h.

## Bench workflow (no hands needed)

- Dev env `m5stack-cores3-dev` points the content host at
  `http://192.168.8.76:3111` (edit for your LAN): copy `players/web/public`
  somewhere, `php -S 0.0.0.0:3111 -t <copy>`, flash dev env, drive
  sync/OTA tests by editing the copy. OTA bench: build with a bumped
  `WH_FW_BUILD` (explicit full flag list in the dev env), drop the `.bin` +
  manifest under `<copy>/content/firmware/m5cores3/`.
- Everything observable ships to serial; assert against
  `scripts/serial_capture.py` output. Sound/touch/visuals need a human.

## Release rule

Every OTA-published binary bumps `WH_FW_BUILD` (monotonic integer) and
`WH_FW_VERSION` (human semver) in `platformio.ini` **production env**, and
follows the runbook in CONTENT-API.md: upload the **versioned** `.bin` first,
rewrite the firmware manifest last. Never publish a `latest.bin`, never
publish from the dev env.

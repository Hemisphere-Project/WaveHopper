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

- Base host: `https://waverz.net`. The firmware makes TLS requests to this host
  **only**, for exactly three endpoint families:
  - `/content/m5cores3/{manifest.json, stations.json, icons/*.png}` — content pack
  - `/content/firmware/m5cores3/manifest.json` (+ the versioned `.bin` it points at)
  - `/api/now-playing.php?id=<id>` — shared now-playing metadata
  Audio stream URLs from `stations.json` go to arbitrary hosts (unverified TLS —
  accepted trade-off; never send anything but the stream GET there).
- Content sync semantics: **equality**, not ordering — sync iff remote
  `contentVersion` != locally recorded one; then per-file sha256 diff.
- Firmware update semantics: update iff remote `build` (integer) **>**
  compiled-in `WH_FW_BUILD`. Never trust `version` (semver) for comparison.
- If remote content `schemaVersion` > `WH_CONTENT_SCHEMA_SUPPORTED`: skip
  content sync but **still check the firmware manifest** — that's how a stale
  device gets unstuck.
- Ignore unknown JSON keys everywhere. Reject manifest paths containing `..`
  or a leading `/`.
- Sync algorithm (staging → verify → ordered commit → manifest last as commit
  marker → prune → wipe staging; free-space precheck with `totalSize` + 64 KB
  slack; boot wipes `.staging/`): implement exactly as written in
  CONTENT-API.md §Device sync.

## Toolchain — do not "fix" these

- `platform` is a **pinned pioarduino release URL** because the official
  `espressif32` platform is stuck on Arduino core 2.x, which ESP32-audioI2S
  ≥ 3.x cannot compile against. Never simplify to `platform = espressif32`.
  Upgrades = deliberately bump the pinned URL, then full `pio run` + on-device test.
- ESP32-audioI2S is pinned by git tag (registry copy is a stale 2.0.7).
- M5Unified and M5GFX versions move **together**.
- Compile gate: `pio run` must pass. There is no device CI — every OTA-published
  binary gets hand-tested on the in-hand device first.

## Hardware facts (CoreS3 SE)

- ESP32-S3, 16 MB QIO flash, 8 MB octal PSRAM, 320×240 capacitive touch,
  microSD slot, USB-C.
- Audio out: AW88298 amp. It must be configured over the internal I2C bus
  before I2S output — that is M5Unified's job (`M5.begin()`), but `M5.Speaker`
  then owns the I2S peripheral. **Handoff pattern**: let `M5.begin()` program
  the amp, keep `M5.Speaker` stopped, drive I2S from ESP32-audioI2S with
  `audio.setPinout(34 /*BCLK*/, 33 /*LRCK*/, 13 /*DOUT*/, 0 /*MCLK*/)`.
  This is the project's #1 technical risk — spike it before building UI on top.
  **Fallback if it fights you**: earlephilhower's ESP8266Audio decoding into
  `M5.Speaker.playRaw()` — M5Unified ships exactly this as its WebRadio example
  (lower risk, weaker AAC/ICY support).
- SE lacks camera, IMU, proximity sensor — never depend on them.
- Playback scope v1: `format == "mp3"` only (22/24 channels). HLS (`thelot`,
  `lyl`) is out — ESP32-audioI2S's m3u8 handling doesn't cover them; skip, don't
  attempt.

## Flash & filesystem

- `partitions.csv`: dual 4 MB OTA slots + ~7.9 MB LittleFS. **Offsets/sizes are
  frozen once any device has OTA'd** — the running firmware writes into the old
  table's slot. USB reflash is the only recovery from a table change.
- On-device layout: `/content/m5cores3/` mirror of the pack;
  `/content/.staging/` for in-flight downloads; local `manifest.json` is the
  commit marker (written last, absence ⇒ full sync).
- NVS holds device state (Wi-Fi credentials, last station, volume) — keep keys
  documented in `include/config.h` as they appear.
- No bootloader rollback (stock arduino-esp32): sha256-verify the OTA image
  *while streaming* into the inactive slot and only `Update.end()` on match.

## Memory discipline

- Big buffers go to PSRAM (`ps_malloc` / PSRAM-backed containers).
- Stream-parse JSON with ArduinoJson from LittleFS/HTTP — never slurp into
  `String`; avoid `String` churn in loops entirely.
- A TLS handshake needs ~50 KB heap: never run two TLS connections while audio
  is playing. Sequence: sync/OTA checks happen before playback starts or while
  stopped; now-playing polls reuse one connection where possible.

## Commands

```sh
pio run                                  # compile (the CI gate)
pio run -t upload                        # flash over USB
python3 ../../tools/build.py --seed-m5   # refresh data/ from content/
pio run -t uploadfs                      # flash LittleFS seed
pio device monitor                       # 115200 baud
```

## Release rule

Every binary published for OTA bumps `WH_FW_BUILD` (monotonic integer) and
`WH_FW_VERSION` (human semver) in `platformio.ini`, and follows the release
runbook in CONTENT-API.md: upload the **versioned** `.bin` first, rewrite the
firmware manifest last. Never publish a `latest.bin`.

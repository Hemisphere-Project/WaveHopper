# CLAUDE.md — M5Stack player (CoreS3 + Fire)

Working rules for AI agents developing this firmware. One codebase, one
binary per chip: **CoreS3 / CoreS3 SE** (ESP32-S3, touch, board id
`m5cores3`) and **Fire** (classic ESP32 + 4 MB PSRAM, 3 buttons, board id
`m5fire`). Board facts live in `include/board.h` (selected by compile target);
`WH_HAS_TOUCH` splits the UI (touch overlay in `ui.cpp`, button menu in
`ui_menu.cpp`). Basic/Gray are unsupported (no PSRAM). The repo-wide picture is
in the root `CLAUDE.md`; the normative cross-player contract is
`docs/CONTENT-API.md`.

## Scope fence

- Work **only inside `players/m5cores3/`** (both boards, incl. `fire/`). Never edit `content/`,
  `players/web/`, or `tools/build.py` from a firmware session.
- Anything that would change a URL, schema, or field consumed from the server
  is a contract change: it goes through `docs/CONTENT-API.md` first (append-only
  for shipped clients; breaking changes need a `schemaVersion` bump and a
  shipped-client audit).

## Contract summary (inline for convenience — CONTENT-API.md is normative)

- Base host: `https://waverz.net`, verified TLS (ISRG roots in
  `include/certs.h`, SNTP required first). Four endpoint families only:
  `/content/m5cores3/*`, `/content/firmware/m5cores3/*`,
  `/api/now-playing.php?id=<id>`, `/api/telemetry.php` (anonymous listener
  stats, fire-and-forget on its own worker). Audio streams go to arbitrary
  hosts (unverified — accepted trade-off). Firmware channel is per board:
  `/content/firmware/<WH_BOARD_ID>/*`; every board reads the `m5cores3` pack. **Exception:** now-playing (only)
  falls back to plain `http://` when a verified handshake can't be afforded
  (`net::whBegin(path, allowPlain)`) — never extend that to sync/OTA/telemetry.
- Content sync: **equality** on `contentVersion`, per-file sha256 diff,
  staged atomic commit — implemented in `content_sync.cpp`; don't reinvent.
- Firmware: update iff remote `build` (integer) **>** compiled `WH_FW_BUILD`,
  manifest `board` == `WH_BOARD_ID`, image header chip id == ours.
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
- **The Fire is a separate PlatformIO project (`fire/`) with its own
  `core_dir` (`~/.platformio-fire`).** pioarduino's hybrid compile keeps ONE
  custom-libs slot per core, keyed on sdkconfig + MCU: any other-chip or
  no-custom-sdkconfig build in the CoreS3's core reinstalls the stock
  framework (wiping the ~15-min 16 KB-window rebuild) — and vice versa. Never
  add a Fire env to `./platformio.ini`. Shared platform/flags/version/libs:
  `wh-common.ini` (both projects include it).
- First hybrid compile may die with `No module named
  'SCons.Tool.FortranCommon'`: its second stage swapped `tool-scons` under the
  running build. The libs were built — just re-run.
- Compile gate: `pio run -e m5stack-cores3` here AND `pio run` in `fire/`. No device CI — every OTA-published
  binary gets hand-tested on the in-hand device first.
- `custom_sdkconfig` (lwIP TCP window, see below) puts pioarduino in
  **hybrid compile**: the framework libs are rebuilt from ESP-IDF once
  (~15 min, then cached in the shared `framework-arduinoespressif32-libs`).
  Its second stage re-invokes `pio` from **`~/.platformio/penv`** (not
  penv-py313) — that venv's Core must also be ≥ 6.1.19 or it *uninstalls the
  pinned platform* as incompatible (`uv pip install --python
  ~/.platformio/penv/bin/python platformio==6.1.19`). `esp-modbus` is removed
  via `custom_component_remove` (fails its own static assert there).
- PlatformIO reorders `-U`/`-D` build flags: never mix `${...}` inheritance
  with undef-then-redefine overrides (leaves macros undefined — bit us once).

## Hardware truths (paid for in debugging hours — do not rediscover)

- **Audio outputs (both boards): Module Audio or RCA — nothing built in.**
  The CoreS3 internal speaker was dropped (2026-10-02); its AW88298 amp
  still sits on the I2S data line (GPIO13), so `audio_out::init` turns it off
  every boot (reg 0x04 = 0x4000, 16-bit registers written **MSB first**, then
  the AW9523 0x58 reg 0x02 bit2 power rail). Auto = Module Audio if 0x33
  answers, else RCA — the RCA module (PCM5102A + DC-DC) has **no I2C, no
  detectable signature**: it is the blind default, and "no module at all"
  can't be told apart from "RCA". NVS `aout` legacy 1 (speaker) reads as auto.
  The history of the AW88298 (64*fs I2SCTRL 0x1CE0, no lock on 44.1 kHz,
  SWS fault power-cycling) is in git before this change, if it ever returns.
- `audio.setOutput48KHz(true)` (constant 48 kHz I2S clock, lib resamples)
  stays: it avoids I2S clock reconfigs (pops) on station changes.
- **I2S pinouts** (exactly one profile active), same M-Bus positions on both
  boards — RCA {BCLK pin22, LRCK pin24, DOUT pin23}, Module Audio adds MCLK:
  CoreS3: RCA {7, 0, 13}, Module Audio {BCLK 0, LRCK 6, DOUT 13, **MCLK 7
  mandatory**, switch B}. Fire: RCA {13, 0, 15} (verified 2026-10-02),
  Module Audio {13, 12, 15, MCLK 0, switch A}. `Audio::setPinout` MCLK
  default is -1/unused — passing 0 routes MCLK onto GPIO0.
- **Probe 0x33 only** for Module Audio (its STM32 helper). Never probe 0x10 —
  the internal BMM150 magnetometer answers there on every CoreS3.
- **Module Audio's codec init steals the touch bus.** `M5Module_Audio.begin`
  runs `Wire.begin()` on I2C0 over the SAME physical pins (12/11) as
  `M5.In_I2C` (I2C1, the touch controller + amp). Bringing up the second
  peripheral re-muxes the GPIOs and silently detaches I2C1 → touch reads
  garbage forever after. `audio_out.cpp` uses Wire only for the one-shot
  ES8388 register setup, then `Wire.end()` + `M5.In_I2C.begin(I2C_NUM_1,12,11)`
  to hand the pins back. Any new Wire user must do the same.
- **ES8388 default init = mic monitor into the headphones.** The M5Module_Audio
  codec init leaves the output mixers at 0xd0 (DAC **+** mic/line amp mixed in;
  0x90 is DAC-only) with micbias on and PGA at 24 dB. On a TRRS headset the
  live, amplified mic feeds the phones = constant background hiss. `audio_out.cpp`
  forces DACCONTROL17/20 to 0x90 and powers ADCPOWER down (0xFF) after init —
  this radio never records.
- **lwIP TCP receive window**: the precompiled Arduino libs ship 5760 B
  (4 MSS) → throughput ≤ window/RTT ≈ 20 KB/s to the US Airtime hosts, below
  a 192 kbps stream. `platformio.ini` raises it to **16 KB** (+ recvmbox 32).
  Don't go bigger: HLS segments arrive as line-rate bursts that park up to a
  window in *internal* RAM — 32 KB took internal heap to 0 KB max block on
  every segment. (mbedtls options can't be changed this way — the hybrid
  compile leaves `libmbedcrypto.a` prebuilt; TLS stays in internal RAM.)
- **Diagnose wifi before firmware.** `[buf] arriv=` ≈ `cons=` with a sinking
  cushion, multi-second TCP connects, or ping to the device in the seconds
  = the 2.4 GHz channel, not the code. 2026-09-26: the bench AP (RE700X,
  AP mode) sat on ch 4 next to another AP on ch 5 → ~3 s pings, stations
  never locking; moved to ch 1 / 20 MHz → kiosk holds a full 164 KB cushion.
- **Wifi runtime watchdog** (`whwifi::maintain`): the stack's auto-reconnect
  once failed to recover from an AP channel change (offline until reset);
  after 20 s down it re-begins the association.
- **Framework/lib landmines found by fuzzing** (fixed — keep the fixes):
  - `NetworkManager::hostByName()` calls `dns_clear_cache()` without the
    TCPIP core lock when the IP state flips (wifi drop/reconnect) → abort in
    `udp_remove` if another lookup is in flight. Wrapped via
    `-Wl,--wrap=dns_clear_cache` (`src/lwip_fixes.cpp`).
  - M5GFX's alpha-PNG path takes an **unchecked** `heap_alloc_dma` line
    buffer → null store when internal heap is low (HTTPS streams). Never
    decode PNGs at render time: icons are pre-decoded into PSRAM sprites at
    boot (`ui::preloadIcons`).
  - ESP32-audioI2S TS reader busy-looped on an idle/dead socket → task-wdt
    reset on a wifi drop mid-HLS; stream TLS handshake defaulted to 120 s.
    Vendored-lib patches 3 + 4 (WAVEHOPPER-PATCHES.md).
- **Internal heap is the scarce resource**, not PSRAM: a verified TLS
  handshake (~50 KB peak) on top of a stream bring-up measured 2988 B free /
  788 B max block. The metadata worker stays quiet while tuning + 5 s after
  playback starts (`now_playing.cpp` quiet window). Watch `heap=`/`maxblk=`
  in `status` and the fuzz summary when adding anything network-y.
- **DNS right after audio bring-up** intermittently times out (lost first
  queries, 3–6 s) — setup() prewarms the start station's host before audio
  init. Root cause not pinned down (router DNS is fast; not IPv6/RDNSS).
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
- **The lib's decode task lives in ONE static TCB/stack** (`xAudioTaskBuffer`).
  `setAudioTaskCore()` on a live task = `vTaskDelete` + immediate re-create in
  the same TCB; a task deleted from the other core is parked on the idle
  task's termination list, so the re-init corrupts that list → intermittent
  IDLE0 `LoadProhibited` in `uxListRemove` right after `audio:` at boot. Set
  the core **before** `setPinout` (task created once, on the right core);
  never `setAudioTaskCore`/`stopAudioTask` a running task.
- A TLS handshake blocks ~0.5–1 s and needs ~50 KB heap: network calls run on
  worker tasks (`now_playing.cpp`) or at boot, never on the input loop; the
  one shared verified client is mutex-guarded (`net.cpp`); no keep-alive (the
  server drops idle connections and a parked session pins ~50 KB).
- **An HTTPS *stream* pins its own ~50 KB TLS session for its whole runtime**
  (CoreS3 — the Fire keeps it in PSRAM, see below),
  leaving too little internal heap for a second verified handshake — measured:
  now-playing/telemetry fail with `-32512` at 61 KB free. `net::whBegin` skips
  those polls below 64 KB free / 20 KB max-block (a clean skip, not a churn).
  The real fix is upstream: stations that publish a verified plain-HTTP stream
  carry an `m5Url` so the pack `url` is `http://` (no stream TLS session) —
  see CONTENT-API.md `m5Url`. Stations that must stay HTTPS (AzuraCast, HLS)
  get now-playing over the plain-HTTP fallback; telemetry is skipped for them.
- **Fire (classic ESP32, D0WDQ6 rev3, 16 MB flash, 4 MB PSRAM):**
  - PSRAM is mandatory — ESP32-audioI2S `begin` fails without it.
  - I2S via the same M-Bus positions as the CoreS3 (M5Unified board_M5Stack
    table): Module Audio {BCLK 13, LRCK 12, DOUT 15, **MCLK 0**}, pin switch
    on **A**; RCA {13, 0, 15}. The 8-bit DAC speaker (GPIO25) is held low.
  - GPIO15 is also the Fire base's LED-bar data line (flickers with I2S).
  - GPIO0 = MCLK = the UART bridge's DTR line (auto-reset circuit): a held
    DTR pulls it low. Serial scripts release DTR on `ttyUSB*`, and drop RTS
    **before** DTR on open — the other order passes through RTS-only = EN low
    = a reset (it killed a wifi-join mid-flight once).
  - `M5.In_I2C` is **I2C_NUM_0 on 21/22 — the same port as `Wire`**:
    `audio_out` re-begins In_I2C on its own port/pins after the codec setup.
  - Internal heap: ~164 KB at boot, **~50 KB / 26 KB max block while
    playing**. A TLS session can't fit → mbedtls allocations ≥512 B go to
    PSRAM (`net::tlsMemInit`, public `mbedtls_platform_set_calloc_free`,
    `WH_TLS_IN_PSRAM`): HTTPS/HLS streams handshake in ~600 ms and verified
    API calls work mid-stream. (CoreS3 candidate, untested there.)
  - Stock lwIP window (5760) — measured fine at 128–256 kbps from EU hosts.
  - **AAC is CPU-bound on core 1** (decode task "PeriodicTask"). Glitch
    bursts with a FULL cushion = I2S underruns, not the network — read
    `under=` / `dec=` in the `[buf]` line (lib patch 5) and `tasks` on the
    console. Two fixes were both needed: lib patch 6 (no VU/spectrum/flat-EQ
    per-sample work, ~10 points) and **flash QIO @ 80 MHz** in
    `fire/platformio.ini` (PlatformIO's board json says DIO @ 40: faad2
    executes from flash through the cache — The Lot's codec 87% → ~73%).
    Measured dead ends: the `-mfix-esp32-psram-cache-issue` MEMW workaround
    (no change), pooling faad2's ~50 KB/frame of PSRAM scratch allocations
    (no change). The 44.1→48 kHz resampler is kept (works; ES8388 untested
    at 44.1). Headroom is thin: The Lot ≈ 73% codec + post-processing.
- **HLS/TS (both boards):** a cushion that drains slowly while `out=` is
  exactly 48000 Hz means bytes are lost on the way in — compare per-segment
  demuxed payload with ffmpeg (`-c copy -f adts`). LYL's "chunk jumps" were
  lib patch 8 (TS PIDs forgotten per segment; LYL puts audio before PAT/PMT).
  `WH-HLS continuity lost` in the log = segments skipped at the playlist level.
- Wi-Fi join race (both boards): while an unreachable stored network is
  being retried, `disconnect()+begin()` is rejected ("sta is connecting,
  cannot set config") and the OLD network keeps going. Always switch
  networks through `restartAssociation()` (`wh_wifi.cpp`).
- Wi-Fi setup portal (`wifi_portal.cpp`): AP + captive DNS + form, boot-only
  (never next to a stream — settings sets the NVS `portal` flag + reboots).
- USB-CDC: `Serial.begin()` is required for `Serial.print*` (log_* bypasses
  it). `pio device monitor` needs a TTY — use `scripts/serial_capture.py`
  from scripts/agents (it also does the proper DTR-low reset dance).
- LittleFS partition is labeled `littlefs`: mount with
  `LittleFS.begin(true, "/littlefs", 10, "littlefs")` (esp_littlefs defaults
  to the label "spiffs" and fails).
- UI matches the webapp's default Dark skin: palette constants in `ui_internal.h`
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
  (0 auto/2 rca/3 module; legacy 1 = auto), `bright`, `portal` (one-shot) —
  documented in config.h. `ssid` present but empty = forgotten (no
  secrets.h fallback).

## Bench workflow (no hands needed)

- Dev env `m5stack-cores3-dev` points the content host at
  `http://192.168.8.76:3111` (edit for your LAN): copy `players/web/public`
  somewhere, `php -S 0.0.0.0:3111 -t <copy>`, flash dev env, drive
  sync/OTA tests by editing the copy. OTA bench: build with a bumped
  `WH_FW_BUILD` (explicit full flag list in the dev env), drop the `.bin` +
  manifest under `<copy>/content/firmware/m5cores3/`.
- Everything observable ships to serial; assert against
  `scripts/serial_capture.py` output. Sound/touch/visuals need a human.
- **Serial console** (`src/serial_cmd.cpp`, replies prefixed `@`): `status`,
  `list`, `tune <idx|id>`, `next`/`prev`, `retune`, `vol <0-21>`,
  `dns <host>`, `net`, `wifi-drop`, `wifi-ssid <s>` / `wifi-pass <p>` /
  `wifi-join` (bench provisioning — also live in the boot wifi wait),
  `portal`, `tasks` (per-task CPU % + core since the previous call),
  `reboot`. Ports: CoreS3 `/dev/ttyACM0` (default), Fire
  `/dev/ttyUSB0` (`--port`). Drive it with
  `scripts/wh_console.py` (`cmd …`, `boot --runs N` = boot→lock timing, `log`)
  — one process owns the port; it handshakes first (after a long idle the
  first bytes sent can be dropped). Run with `~/.platformio/penv/bin/python3`.
- **Fuzzing**: `scripts/fuzz.py --minutes 30 --seed N --log <file>` — random
  tunes / surf bursts / retunes / volume spam / wifi drops / reboots, checks
  crashes, unexpected resets, UI + player hangs, lock-after-settle, heap
  trend. Seeds replay the action sequence. Run a 30-min pass before any OTA
  release that touches player, network or UI code.

## Release rule

Every OTA-published binary bumps `WH_FW_BUILD` (monotonic integer) and
`WH_FW_VERSION` (human semver) in **`wh-common.ini`** (shared by both
boards), and follows the runbook in CONTENT-API.md:
`python3 tools/release-m5.py --board m5cores3|m5fire|all` writes the
**versioned** `.bin` first and the manifest last. Never publish a
`latest.bin`, never publish from a dev env.

# WaveHopper — M5Stack player (CoreS3 + Fire)

Webradio firmware for the [M5Stack CoreS3 / CoreS3 SE](https://docs.m5stack.com/en/core/CoreS3)
(ESP32-S3, 16 MB flash, 8 MB PSRAM, 320×240 capacitive touch)
and the [M5Stack Fire](https://docs.m5stack.com/en/core/fire) (classic ESP32,
16 MB flash, 4 MB PSRAM, 320×240, three buttons). Same sources, one binary per
chip — board facts live in `include/board.h`. Basic/Gray are **not** supported
(no PSRAM, which the audio library requires).

The device is a self-updating client of the authoritative web deploy at
`https://waverz.net` (and shows **Waverz·net** on its boot screen — "WaveHopper"
is the repo/project name):

- **Content** (station catalog + icons) syncs from `/content/m5cores3/` — see
  [docs/CONTENT-API.md](../../docs/CONTENT-API.md) for the manifest schema and
  the atomic sync algorithm.
- **Firmware** self-OTAs from `/content/firmware/<board>/manifest.json`
  (`m5cores3` or `m5fire`; updates when the remote `build` number is greater
  than the compiled-in one). Both boards read the same content pack.
- **Now-playing** metadata reuses the shared `/api/now-playing.php?id=<id>`.

## State: working prototype (v1)

Auto-plays on boot (last station remembered), streams MP3/AAC stations with a
prebuffer cushion against wifi jitter, auto-retries/skips dead streams, syncs
content and checks OTA at boot, polls now-playing while playing.

**Controls** (no play/pause — it just plays):

| Input | Action |
|---|---|
| Tap left / right half of screen | previous / next station |
| Horizontal flick | next (left) / previous (right) |
| Vertical drag | browse the station list; tunes when you settle |
| Bezel touch-buttons A / C (below screen) | volume down / up |
| Touch-hold the card ~0.5 s, or hold bezel button B | open settings overlay |

The settings overlay holds **brightness**, per-station **enable/disable**, and
**wifi** (scan + on-screen keyboard join), with firmware/content versions in
the footer. It is also reachable during the boot wifi wait — the boot screen
retries the saved network forever and shows a gear (top-right); tap it (or use
the same hold gesture) to join a new network before the device is online. There
is no play/pause and no manual audio-output picker (output is auto-detected).

**Controls on the Fire** (three buttons; a soft-key bar labels them):

| Input | Action |
|---|---|
| A / C tap | previous / next station |
| A / C hold | browse the station list; tunes ~0.6 s after release |
| B tap | volume mode: A = −, C = + (hold repeats); B or 3 s idle leaves |
| B hold | settings menu: A up, C down, B tap select, B hold back |
| B held at power-on | straight into the Wi-Fi setup portal |

**Wi-Fi setup from a phone** (both boards): the device opens an access point
`Waverz-XXXX` and shows two QR codes — join it, then open `192.168.4.1` (most
phones pop the page up by themselves); pick the network, type its password,
and the device verifies, saves and reboots. It opens by itself when no network
is stored, after 2 minutes of a stored network being unreachable (closing
again after 3 idle minutes), from settings → wifi → phone setup, or with B held
at power-on. Bench alternative: the serial console's `wifi-ssid` /
`wifi-pass` / `wifi-join`.

**Audio outputs** — the same two modules on both boards (neither has a usable
built-in output: the CoreS3's speaker amp and the Fire's 8-bit DAC speaker are
kept off):

| Output | Hardware | Selection |
|---|---|---|
| `module` | Module Audio M144 (ES8388, TRRS headphone) — pin switch **B** on the CoreS3, **A** (default) on the Fire | auto when its helper answers at I2C 0x33 |
| `rca` | Module13.2 RCA (PCM5102A line-out) | auto otherwise — it can't be detected (no I2C), so it is the default |

Settings → **audio** cycles `auto` → `module` → `rca` (shown as e.g.
`auto: module`); a change is saved at once and applies on the reboot that
"save + reboot" triggers.

## Build & flash

Requires [PlatformIO](https://platformio.org/) with **Python ≤ 3.13** (the
pinned pioarduino platform rejects 3.14; on this machine the CLI lives at
`~/.platformio/penv-py313/bin/platformio`). The platform is a pinned
[pioarduino](https://github.com/pioarduino/platform-espressif32) release
(Arduino core 3.x) — read the comment in `platformio.ini` before touching it.

```sh
cp include/secrets.h.example include/secrets.h   # fill in wifi credentials
pio run                    # compile (production env)
pio run -t upload          # flash over USB
python3 ../../tools/build.py --seed-m5           # refresh data/ from content/
pio run -t uploadfs        # flash the LittleFS seed (optional — device syncs)
pio device monitor         # serial console (115200)
```

Non-interactive serial capture (works without a TTY, for scripts/agents):

```sh
python3 scripts/serial_capture.py --reset --seconds 30
```

### Fire

The Fire is a separate PlatformIO project in `fire/` with its own core
(`~/.platformio-fire`) — see the comment at the top of `fire/platformio.ini`
for why. Its USB port is a CP2104 UART bridge (`/dev/ttyUSB0`):

```sh
cd fire
pio run                                      # compile
pio run -t upload --upload-port /dev/ttyUSB0
python3 ../scripts/wh_console.py --port /dev/ttyUSB0 cmd status
```

### Dev bench env

`pio run -e m5stack-cores3-dev -t upload` redirects the content host to a LAN
machine over plain HTTP (see `platformio.ini`) — used to bench-test sync/OTA
against a local `php -S 0.0.0.0:3111 -t <docroot-copy>` without touching
production. Never publish binaries from this env.

## Layout

```
platformio.ini      CoreS3 envs (read the warning comments)
wh-common.ini       shared: pinned platform, flags, version/build, libraries
fire/platformio.ini Fire envs — separate project + PlatformIO core
include/board.h     per-board facts (board id, touch, amp, I2S pin sets)
partitions.csv      16 MB: dual 4 MB OTA slots + ~7.9 MB LittleFS (frozen once shipped)
include/config.h    host, endpoint paths, timings, NVS schema
include/certs.h     ISRG Root X1+X2 (verified TLS to waverz.net)
include/secrets.h   wifi credentials (gitignored; see secrets.h.example)
src/main.cpp        boot sequencer + input/UI loop
src/player.*        audio engine: lib task mgmt, prebuffer, auto-skip supervisor
src/audio_out.*     output profiles: Module Audio (ES8388) / RCA (PCM5102A) + probe; CoreS3 amp kept off
src/content_sync.*  CONTENT-API §Device sync implementation
src/fw_update.*     OTA with streaming sha256 verify
src/now_playing.*   30 s metadata poll on a worker task (ICY fallback)
src/catalog.*       stations.json loader/filter from the local pack
src/net.*           one mutex-guarded verified-TLS client to waverz.net
src/ui.*            canvas card, marquee, toast list, volume bar, touch settings
src/ui_menu.cpp     button-board settings menu (Fire)
src/wifi_portal.*   phone Wi-Fi setup: SoftAP + captive DNS + web form + QR codes
src/wh_wifi.* wh_nvs.*  connectivity + persisted settings
data/               LittleFS seed (generated, git-ignored)
CLAUDE.md           working rules + hardware truths for AI agents — read it
```

## Licensing

GPL-3.0 (whole repo). The audio pipeline links
[ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) (GPL-3.0) and
[M5Module-Audio](https://github.com/m5stack/M5Module-Audio) (MIT).

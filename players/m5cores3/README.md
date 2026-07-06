# WaveHopper — M5Stack CoreS3 player

Webradio firmware for the [M5Stack CoreS3 / CoreS3 SE](https://docs.m5stack.com/en/core/CoreS3)
(ESP32-S3, 16 MB flash, 8 MB PSRAM, 320×240 capacitive touch, AW88298 speaker amp).

The device is a self-updating client of the authoritative web deploy at
`https://waverz.net` (and shows **Waverz·net** on its boot screen — "WaveHopper"
is the repo/project name):

- **Content** (station catalog + icons) syncs from `/content/m5cores3/` — see
  [docs/CONTENT-API.md](../../docs/CONTENT-API.md) for the manifest schema and
  the atomic sync algorithm.
- **Firmware** self-OTAs from `/content/firmware/m5cores3/manifest.json`
  (updates when the remote `build` number is greater than the compiled-in one).
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

**Audio outputs** (auto-detected at boot — no manual selection):

| Output | Hardware | Detection |
|---|---|---|
| `module audio` | Module Audio M144 (ES8388, TRRS headphone) — pin switch on **B** | preferred when probed at I2C 0x33 |
| `internal` | built-in AW88298 amp + speaker | fallback when no module is found |
| `rca module` | Module13.2 RCA M125 (PCM5102A line-out) | unprobeable — not auto-selected |

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

### Dev bench env

`pio run -e m5stack-cores3-dev -t upload` redirects the content host to a LAN
machine over plain HTTP (see `platformio.ini`) — used to bench-test sync/OTA
against a local `php -S 0.0.0.0:3111 -t <docroot-copy>` without touching
production. Never publish binaries from this env.

## Layout

```
platformio.ini      pinned platform + libraries (read the warning comments)
partitions.csv      16 MB: dual 4 MB OTA slots + ~7.9 MB LittleFS (frozen once shipped)
include/config.h    host, endpoint paths, audio profile pin table, timings, NVS schema
include/certs.h     ISRG Root X1+X2 (verified TLS to waverz.net)
include/secrets.h   wifi credentials (gitignored; see secrets.h.example)
src/main.cpp        boot sequencer + input/UI loop
src/player.*        audio engine: lib task mgmt, prebuffer, auto-skip supervisor
src/audio_out.*     output profiles: AW88298 / RCA / ES8388 + probe + fault recovery
src/content_sync.*  CONTENT-API §Device sync implementation
src/fw_update.*     OTA with streaming sha256 verify
src/now_playing.*   30 s metadata poll on a worker task (ICY fallback)
src/catalog.*       stations.json loader/filter from the local pack
src/net.*           one mutex-guarded verified-TLS client to waverz.net
src/ui.*            canvas card, marquee, toast list, volume bar, settings
src/wh_wifi.* wh_nvs.*  connectivity + persisted settings
data/               LittleFS seed (generated, git-ignored)
CLAUDE.md           working rules + hardware truths for AI agents — read it
```

## Licensing

GPL-3.0 (whole repo). The audio pipeline links
[ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) (GPL-3.0) and
[M5Module-Audio](https://github.com/m5stack/M5Module-Audio) (MIT).

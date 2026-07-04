# WaveHopper — M5Stack CoreS3 player

Firmware for the [M5Stack CoreS3 / CoreS3 SE](https://docs.m5stack.com/en/core/CoreS3)
(ESP32-S3, 16 MB flash, 8 MB PSRAM, 320×240 capacitive touch, AW88298 speaker amp).

The device is a self-updating client of the authoritative web deploy at
`https://waverz.net`:

- **Content** (station catalog + icons) syncs from `/content/m5cores3/` — see
  [docs/CONTENT-API.md](../../docs/CONTENT-API.md) for the manifest schema and
  the atomic sync algorithm.
- **Firmware** self-OTAs from `/content/firmware/m5cores3/manifest.json`
  (updates when the remote `build` number is greater than the compiled-in one).
- **Now-playing** metadata reuses the shared `/api/now-playing.php?id=<id>`.

Current state: **skeleton** — boots the display, mounts LittleFS, shows the
seeded content version. Sync, OTA, UI, and playback are not implemented yet.

## Build & flash

Requires [PlatformIO](https://platformio.org/). The platform is a pinned
[pioarduino](https://github.com/pioarduino/platform-espressif32) release
(Arduino core 3.x) — see the comment in `platformio.ini` before touching it.

```sh
pio run                   # compile
pio run -t upload         # flash over USB
pio run -t uploadfs       # flash the LittleFS image built from data/
pio device monitor        # serial console (115200)
```

`data/` is a build product (git-ignored). Populate it with the current content
pack before `uploadfs`:

```sh
python3 ../../tools/build.py --seed-m5
```

Seeding is a convenience for first-boot-offline demos; a blank filesystem is
also valid — the firmware treats "no local manifest" as "full sync needed".

## Layout

```
platformio.ini    pinned platform + libraries (read the warning comment)
partitions.csv    16 MB: dual 4 MB OTA slots + ~7.9 MB LittleFS (frozen once shipped)
include/config.h  host, endpoint paths, I2S pinout, schema level
src/main.cpp      skeleton entry point
data/             LittleFS seed (generated, git-ignored)
CLAUDE.md         working rules + hardware/contract facts for AI agents
```

## Licensing

This project is GPL-3.0. The audio pipeline uses
[ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) (GPL-3.0) —
compatible, but keep it in mind before relicensing anything.

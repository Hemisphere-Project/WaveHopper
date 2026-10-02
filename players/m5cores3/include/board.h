// WaveHopper — per-board facts, selected by the compile target.
//
// One codebase, one binary per CHIP: the CoreS3 (ESP32-S3) and the Fire
// (classic ESP32) can never share an image (different ISA, bootloader, image
// header chip id). Each board has its own firmware channel
// (/content/firmware/<WH_BOARD_ID>/) — both read the same content pack
// (/content/m5cores3/: same 320x240 screen, same icons, same m5Url logic).
#pragma once

#include <sdkconfig.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3)

// M5Stack CoreS3 / CoreS3 SE — touch screen, AW88298 internal amp.
#define WH_BOARD_ID          "m5cores3"
#define WH_BOARD_NAME        "CoreS3"
#define WH_HAS_TOUCH         1
#define WH_HAS_INTERNAL_AMP  1
#define WH_TLS_IN_PSRAM      0  // candidate (same starvation) — not yet bench-tested here
#define WH_DIM_DIVISOR       4   // auto-dim = brightness / this (AXP2101-driven backlight)
#define WH_DIM_MIN           12

// I2S pin sets on the CoreS3 M-Bus (chip-level facts in CLAUDE.md). MCLK -1 =
// unused; NEVER default MCLK to 0: passing 0 to Audio::setPinout routes MCLK
// onto GPIO0 (only Module Audio deliberately uses GPIO7 for MCLK).
//
//                          BCLK  LRCK  DOUT  MCLK
#define WH_PINS_INTERNAL    { 34,   33,   13,  -1 }   // AW88298 amp (BCK-clocked)
#define WH_PINS_RCA         {  7,    0,   13,  -1 }   // Module13.2 RCA (PCM5102A)
#define WH_PINS_MODULE      {  0,    6,   13,   7 }   // Module Audio (switch on B)

#elif defined(CONFIG_IDF_TARGET_ESP32)

// M5Stack Fire — 3 physical buttons, no touch, 4 MB PSRAM (required:
// ESP32-audioI2S refuses to start without it, so Basic/Gray are unsupported).
// The internal speaker (8-bit DAC on GPIO25) is deliberately unused — audio
// goes out through Module Audio or the RCA module only.
#define WH_BOARD_ID          "m5fire"
#define WH_BOARD_NAME        "Fire"
#define WH_HAS_TOUCH         0
#define WH_HAS_INTERNAL_AMP  0
// mbedtls buffers (≥512 B) go to PSRAM — see net::tlsMemInit(). Without it
// the classic ESP32 has ~50 KB internal heap left while playing: no HTTPS
// stream (The Lot, LYL: -32512 at the handshake) and no verified API calls.
#define WH_TLS_IN_PSRAM      1
// The Fire's PWM backlight stays bright at /4 — dim much deeper to match the
// CoreS3's dimmed look.
#define WH_DIM_DIVISOR       12
#define WH_DIM_MIN           6
#define WH_PIN_DAC_SPEAKER   25  // held low: a floating DAC input hums in the speaker

// Same M-Bus positions as the CoreS3 profiles, mapped through M5Unified's
// board_M5Stack M-Bus table (pin21=G12, pin22=G13, pin23=G15, pin24=G0).
// Module Audio on a classic-ESP32 host swaps BCK/MCK positions (switch on A,
// the factory default); its MCLK lands on GPIO0 — one of the only three pins
// (0/1/3) the ESP32 I2S can route MCLK to. GPIO15 is also the Fire base's
// LED-bar data line: I2S data there makes the LEDs flicker (hardware, can't fix).
//
//                          BCLK  LRCK  DOUT  MCLK
#define WH_PINS_RCA         { 13,    0,   15,  -1 }   // Module13.2 RCA (PCM5102A)
#define WH_PINS_MODULE      { 13,   12,   15,   0 }   // Module Audio (switch on A)

#else
#error "WaveHopper: unsupported target (ESP32-S3 CoreS3 or ESP32 Fire only)"
#endif

// The ESP image header's chip id for this target (esp_image_header_t.chip_id,
// little-endian u16 at byte 12 of an app image) — the OTA path refuses a
// binary built for another chip even if a manifest got mixed up.
#define WH_IMAGE_CHIP_ID CONFIG_IDF_FIRMWARE_CHIP_ID

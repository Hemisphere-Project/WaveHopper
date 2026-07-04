// WaveHopper CoreS3 — build-time configuration.
// The cross-player contract (endpoints, manifest schemas, sync algorithm) is
// normative in docs/CONTENT-API.md; the values here must match it.

#pragma once

// ---------------------------------------------------------------------------
// Authoritative host. The firmware makes VERIFIED TLS requests to this host
// ONLY (content sync, firmware OTA, now-playing). Audio stream URLs come from
// stations.json and go to arbitrary hosts, unverified (ESP32-audioI2S calls
// setInsecure() internally) — a documented, accepted trade-off.
#define WH_CONTENT_HOST "waverz.net"

// Content pack for this player (equality sync on manifest contentVersion).
#define WH_CONTENT_MANIFEST_PATH "/content/m5cores3/manifest.json"
#define WH_CONTENT_BASE_PATH     "/content/m5cores3/"

// Firmware OTA pointer (update iff remote `build` > WH_FW_BUILD).
#define WH_FIRMWARE_MANIFEST_PATH "/content/firmware/m5cores3/manifest.json"

// Now-playing metadata, shared with every other player.
#define WH_NOW_PLAYING_PATH "/api/now-playing.php?id="

// Highest content manifest schemaVersion this firmware understands. If the
// remote manifest advertises a newer one, skip content sync (but still check
// for firmware updates — that is how we get unstuck).
#define WH_CONTENT_SCHEMA_SUPPORTED 1

// Local LittleFS layout: mirror of the pack + staging area for atomic sync.
// The local copy of manifest.json is the sync commit marker — written last.
#define WH_FS_CONTENT_DIR "/content/m5cores3"
#define WH_FS_STAGING_DIR "/content/.staging"

// ---------------------------------------------------------------------------
// Audio output profiles — I2S pin sets on the CoreS3 M-Bus (see CLAUDE.md for
// the chip-level facts). MCLK -1 = unused; NEVER default MCLK to 0: passing 0
// to Audio::setPinout routes MCLK onto GPIO0 (only the Module Audio profile
// deliberately uses GPIO7 for MCLK).
//
//                          BCLK  LRCK  DOUT  MCLK
#define WH_PINS_INTERNAL    { 34,   33,   13,  -1 }   // AW88298 amp (BCK-clocked)
#define WH_PINS_RCA         {  7,    0,   13,  -1 }   // Module13.2 RCA (PCM5102A)
#define WH_PINS_MODULE      {  0,    6,   13,   7 }   // Module Audio (ES8388, MCLK mandatory)

// Internal-bus I2C addresses used by the audio path.
#define WH_I2C_AW88298   0x36  // internal speaker amp
#define WH_I2C_AW9523    0x58  // IO expander: reg 0x02 bit2 = speaker power rail
#define WH_I2C_MODAUDIO  0x33  // Module Audio's STM32 helper — the ONLY probeable
                               // module id. Never probe 0x10 (ES8388) — the internal
                               // BMM150 magnetometer also answers at 0x10.

// ---------------------------------------------------------------------------
// Timing.
#define WH_WIFI_TIMEOUT_MS    30000  // boot wifi connect budget
#define WH_SNTP_TIMEOUT_MS    10000  // clock sync budget (TLS needs wall time)
#define WH_TUNE_TIMEOUT_MS    15000  // connect→codec deadline per attempt
// Startup cushion: most stations (Icecast/Airtime/AzuraCast) pace at exactly
// realtime, so without a head start every wifi hiccup is an audible gap. We
// hold the decoder suspended after connect until this many bytes buffered (or
// the wait cap), trading tune latency for a persistent jitter cushion.
#define WH_PREBUFFER_BYTES    98304  // ~4 s at 192 kbps
#define WH_PREBUFFER_WAIT_MS  4500   // cap on the buffering hold
// Depleted-cushion recovery: when the buffer stays under LOW for LOW_MS while
// playing, the connection is sick (or jitter ate the cushion for good) — one
// deliberate reconnect rebuilds it via the server's burst-on-connect, instead
// of many micro-gaps.
#define WH_REBUFFER_LOW       8192
#define WH_REBUFFER_LOW_MS    5000
#define WH_STALL_MS           20000  // PLAYING with empty buffer this long = dead
#define WH_ALLFAIL_SWEEP_MS   60000  // retry period after every station failed
#define WH_NP_POLL_MS         30000  // now-playing poll while playing

// ---------------------------------------------------------------------------
// NVS (Preferences namespace "wh"):
//   ssid    str   wifi network            pass   str   wifi password
//   last_st str   station id to auto-play vol    uchar 0..21 (default 12)
//   aout    uchar 0 auto | 1 internal | 2 rca | 3 module   (default 0)
//   bright  uchar display brightness 0..255 (default 200)

#ifndef WH_FW_VERSION
#define WH_FW_VERSION "0.0.0-dev"
#endif
#ifndef WH_FW_BUILD
#define WH_FW_BUILD 0
#endif

// WaveHopper CoreS3 — build-time configuration.
// The cross-player contract (endpoints, manifest schemas, sync algorithm) is
// normative in docs/CONTENT-API.md; the values here must match it.

#pragma once

// Authoritative host. The firmware talks TLS to this host ONLY (content sync,
// firmware OTA, now-playing). Audio stream URLs come from stations.json and go
// to arbitrary hosts, unverified — that is a documented, accepted trade-off.
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

// AW88298 amp is configured over I2C by M5Unified; ESP32-audioI2S then drives
// I2S directly with this pinout (keep M5.Speaker stopped while audio runs).
#define WH_I2S_BCLK 34
#define WH_I2S_LRCK 33
#define WH_I2S_DOUT 13
#define WH_I2S_MCLK 0

#ifndef WH_FW_VERSION
#define WH_FW_VERSION "0.0.0-dev"
#endif
#ifndef WH_FW_BUILD
#define WH_FW_BUILD 0
#endif

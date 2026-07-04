// Firmware OTA: checks /content/firmware/m5cores3/manifest.json and updates
// when the remote integer `build` is strictly greater than WH_FW_BUILD.
// sha256 verified while streaming into the inactive slot; Update.end() only
// on a full match (no bootloader rollback on stock arduino-esp32 — the hash
// gate plus on-device testing of every published build is the safety story).
#pragma once

namespace fw_update {
// Blocking; run at boot after content sync, before playback. On a successful
// update this reboots the device and never returns.
void checkAndUpdate();
}  // namespace fw_update

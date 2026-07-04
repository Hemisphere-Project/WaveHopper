// Wi-Fi bring-up + wall-clock sync (TLS cert validation needs real time).
#pragma once

#include "wh_nvs.h"

namespace whwifi {
// Resolve credentials (NVS wins, else compiled-in secrets.h → stored to NVS)
// and connect. Blocks up to timeoutMs. Returns true when connected.
bool connect(WhSettings& s, uint32_t timeoutMs);

// SNTP sync; blocks until the clock is sane or timeoutMs. Returns success.
bool syncClock(uint32_t timeoutMs);

// Try new credentials (settings-page join). Drops the current link, attempts
// the new one within timeoutMs, returns whether it connected. Does NOT persist
// — the caller saves + reboots on success, or restores the old link on failure.
bool joinNew(const String& ssid, const String& pass, uint32_t timeoutMs);

bool isConnected();
}  // namespace whwifi

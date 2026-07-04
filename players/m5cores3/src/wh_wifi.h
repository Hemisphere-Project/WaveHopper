// Wi-Fi bring-up + wall-clock sync (TLS cert validation needs real time).
#pragma once

#include "wh_nvs.h"

namespace whwifi {
// Resolve credentials (NVS wins, else compiled-in secrets.h → stored to NVS)
// and connect. Blocks up to timeoutMs. Returns true when connected.
bool connect(WhSettings& s, uint32_t timeoutMs);

// SNTP sync; blocks until the clock is sane or timeoutMs. Returns success.
bool syncClock(uint32_t timeoutMs);

bool isConnected();
}  // namespace whwifi

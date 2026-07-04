// Listener telemetry: anonymous install id (NVS) + start/heartbeat/stop
// events POSTed to the shared ingest endpoint. Strictly fire-and-forget on a
// low-priority worker — telemetry must never affect playback.
#pragma once

#include <Arduino.h>

namespace telemetry {
// Call every loop with the player state; emits start/stop on transitions and
// heartbeats every WH_TELEMETRY_HB_MS while playing. Station id "" = none.
void tick(bool playing, const String& stationId);
}  // namespace telemetry

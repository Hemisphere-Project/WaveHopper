// Listener telemetry: anonymous install id (NVS) + start/heartbeat/stop
// events POSTed to the shared ingest endpoint. Strictly fire-and-forget on a
// low-priority worker — telemetry must never affect playback.
#pragma once

#include <Arduino.h>

namespace telemetry {
// Call every loop with the player state; emits start/stop on transitions and
// heartbeats every WH_TELEMETRY_HB_MS while playing. Station id "" = none.
// Events are queued only — the now_playing worker drains them (one shared
// network task: a second 12 KB TLS-capable stack starved the heap enough to
// break every verified handshake during playback).
void tick(bool playing, const String& stationId);

// Non-blocking: posts at most one queued event. Called by the shared worker.
void drainOne();
}  // namespace telemetry

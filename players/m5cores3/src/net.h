// Verified-TLS HTTP to the authoritative host (waverz.net) — one persistent
// client + connection reuse shared by now-playing, content sync, and OTA.
// Heap discipline: never open a second verified connection while one request
// is in flight; keep requests OFF the audio path where possible (boot, or the
// small 30 s now-playing poll which coexists with the stream's own TLS).
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>

namespace net {
// Boards with WH_TLS_IN_PSRAM: route mbedtls allocations ≥512 B to PSRAM
// (public mbedtls_platform_set_calloc_free hook — no framework rebuild).
// Call first thing in setup(), before any TLS. No-op elsewhere.
void tlsMemInit();

// True once wh_wifi::syncClock succeeded — verified TLS needs wall time.
// When false, all whBegin() calls fail fast (streams still play, unverified).
void setClockValid(bool valid);
bool clockValid();

// Begin a GET on https://WH_CONTENT_HOST<path>. On true, use http() to read
// status/stream, then end(). whBegin/end bracket a critical section (one
// shared TLS client) — callers on different tasks serialize automatically;
// never call whBegin twice without end(). allowPlain: when a verified
// handshake can't be afforded (heap, or no clock), use plain http:// instead
// of skipping — ONLY for public display data (now-playing).
bool whBegin(const String& path, bool allowPlain = false);
HTTPClient& http();
void end();
}  // namespace net

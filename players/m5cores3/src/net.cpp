#include "net.h"

#include <NetworkClientSecure.h>

#include "certs.h"
#include "config.h"

// WH_DEV_INSECURE_HOST ("ip:port"): bench-testing override — plain HTTP to a
// LAN machine running `php -S` instead of verified TLS to waverz.net. Build
// via the *-dev PlatformIO env only; production builds must never define it.
#ifdef WH_DEV_INSECURE_HOST
#warning "DEV build: plain-HTTP content host override active - do not ship"
#endif

namespace {
#ifdef WH_DEV_INSECURE_HOST
NetworkClient g_client;
#else
NetworkClientSecure g_client;
NetworkClient g_plain;  // opt-in plain-HTTP fallback (see whBegin)
#endif
HTTPClient g_http;
bool g_clockValid = false;
bool g_inited = false;
SemaphoreHandle_t g_mutex = xSemaphoreCreateMutex();

void initOnce() {
  if (g_inited) return;
  g_inited = true;
#ifndef WH_DEV_INSECURE_HOST
  g_client.setCACert(WH_ROOT_CAS);
  g_client.setHandshakeTimeout(10);  // seconds
#else
  log_e("=== DEV BUILD: content host is http://%s ===", WH_DEV_INSECURE_HOST);
#endif
  // No keep-alive: the server drops idle connections between 30 s polls
  // anyway, and a parked TLS session pins ~50 KB of internal heap. One
  // handshake per poll/sync-batch is the cheaper trade.
  g_http.setReuse(false);
  g_http.setConnectTimeout(5000);
  g_http.setTimeout(8000);
}
}  // namespace

#if WH_TLS_IN_PSRAM
// mbedtls allocator for boards whose internal heap can't hold a TLS session
// next to a running stream (the Fire: ~50 KB left, a session wants ~50 KB
// with ~17 KB contiguous record buffers). Record buffers, certs and big MPIs
// (≥512 B) live in PSRAM; small hot allocations stay internal (handshake
// math speed). Either region falls back to the other. Frees from both
// allocators (anything handed out before the switch) are heap_caps_free-safe.
extern "C" {
#include <mbedtls/platform.h>
}
namespace {
void* tlsCalloc(size_t n, size_t size) {
  const size_t bytes = n * size;
  if (n && bytes / n != size) return nullptr;  // overflow
  void* p = nullptr;
  if (bytes >= 512) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p && bytes < 512) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p;
}
void tlsFree(void* p) { heap_caps_free(p); }
}  // namespace
#endif

namespace net {

void tlsMemInit() {
#if WH_TLS_IN_PSRAM
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  log_i("net: mbedtls allocations >=512 B -> PSRAM");
#endif
}

void setClockValid(bool valid) { g_clockValid = valid; }
bool clockValid() { return g_clockValid; }

bool whBegin(const String& path, bool allowPlain) {
  bool plain = false;
#ifndef WH_DEV_INSECURE_HOST
  // A verified TLS handshake peaks ~50 KB of internal heap with ~17 KB
  // contiguous record buffers. Measured on-device: 61 KB free still fails
  // (-32512 alloc) while an HTTPS stream pins its own ~50 KB session. Below
  // these floors the handshake cannot succeed — skip cleanly instead of
  // churning mbedtls (fragmentation + crash risk).
#if WH_TLS_IN_PSRAM
  // TLS buffers live in PSRAM here (tlsMemInit): the internal floor only has
  // to cover the socket, lwIP pbufs and the small handshake allocations.
  bool tlsOk = g_clockValid && ESP.getFreePsram() >= 96000 && ESP.getFreeHeap() >= 24000 &&
               ESP.getMaxAllocHeap() >= 8192;
#else
  bool tlsOk = g_clockValid && ESP.getFreeHeap() >= 64000 && ESP.getMaxAllocHeap() >= 20000;
#endif
  if (!tlsOk) {
    // Callers fetching public, display-only data (now-playing) may fall back
    // to plain HTTP: a socket needs a few KB, not ~50. Without it an HTTPS
    // stream (The Lot, LYL) never showed metadata — every poll was skipped.
    // Integrity trade-off = the stream audio's (fetched unverified). Content
    // sync, OTA and telemetry never opt in. Decided 2026-09-27 (Thomas).
    if (allowPlain && ESP.getFreeHeap() >= 16000 && ESP.getMaxAllocHeap() >= 4096) {
      plain = true;
    } else {
      log_w("net: skipping %s, low heap (%lu free, %lu max block)", path.c_str(),
            (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
      return false;
    }
  }
#endif
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
  initOnce();
#ifdef WH_DEV_INSECURE_HOST
  String url = String("http://") + WH_DEV_INSECURE_HOST + path;
  NetworkClient& client = g_client;
#else
  String url = String(plain ? "http://" : "https://") + WH_CONTENT_HOST + path;
  NetworkClient& client = plain ? g_plain : static_cast<NetworkClient&>(g_client);
  if (plain) log_i("net: %s over plain HTTP (TLS unaffordable now)", path.c_str());
#endif
  if (!g_http.begin(client, url)) {
    log_e("http begin failed: %s", url.c_str());
    xSemaphoreGive(g_mutex);
    return false;
  }
  return true;
}

HTTPClient& http() { return g_http; }

void end() {
  g_http.end();
  xSemaphoreGive(g_mutex);
}

}  // namespace net

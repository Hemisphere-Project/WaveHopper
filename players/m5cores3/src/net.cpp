#include "net.h"

#include <NetworkClientSecure.h>

#include "certs.h"
#include "config.h"

namespace {
NetworkClientSecure g_client;
HTTPClient g_http;
bool g_clockValid = false;
bool g_inited = false;
SemaphoreHandle_t g_mutex = xSemaphoreCreateMutex();

void initOnce() {
  if (g_inited) return;
  g_inited = true;
  g_client.setCACert(WH_ROOT_CAS);
  g_client.setHandshakeTimeout(10);  // seconds
  // No keep-alive: the server drops idle connections between 30 s polls
  // anyway, and a parked TLS session pins ~50 KB of internal heap. One
  // handshake per poll/sync-batch is the cheaper trade.
  g_http.setReuse(false);
  g_http.setConnectTimeout(5000);
  g_http.setTimeout(8000);
}
}  // namespace

namespace net {

void setClockValid(bool valid) { g_clockValid = valid; }
bool clockValid() { return g_clockValid; }

bool whBegin(const String& path) {
  if (!g_clockValid) return false;  // cert validation would fail pre-SNTP
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
  initOnce();
  String url = String("https://") + WH_CONTENT_HOST + path;
  if (!g_http.begin(g_client, url)) {
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

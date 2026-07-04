#include "now_playing.h"

#include <ArduinoJson.h>

#include "catalog.h"
#include "config.h"
#include "net.h"

namespace {

// Poll requests run on a worker task — a TLS handshake takes ~0.5-1 s and
// must never block the input/UI loop.
TaskHandle_t g_worker = nullptr;
SemaphoreHandle_t g_mux = xSemaphoreCreateMutex();  // String ops may malloc —
                                                    // no critical sections here
NowPlaying g_np;                       // guarded by g_mux
volatile int g_requestedStation = -1;  // set by tick(), consumed by worker

uint32_t g_nextPollAt = 0;
int g_lastStation = -1;

void publish(const String& title, const String& subtitle) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  bool changed = title != g_np.title || subtitle != g_np.subtitle;
  if (changed) {
    g_np.title = title;
    g_np.subtitle = subtitle;
    g_np.generation++;
  }
  xSemaphoreGive(g_mux);
  if (changed && !title.isEmpty()) log_i("now-playing: %s — %s", title.c_str(), subtitle.c_str());
}

void poll(const Station& s) {
  String path = String(WH_NOW_PLAYING_PATH) + s.id;
  if (!net::whBegin(path)) return;

  int code = net::http().GET();
  if (code < 0) {
    // Transient connect/socket error — one fresh retry.
    net::end();
    if (!net::whBegin(path)) return;
    code = net::http().GET();
  }
  if (code == 200) {
    // getString() — the API responds chunked over HTTP/1.1 and getStream()
    // would hand the raw chunk framing to the parser. Payload is ~250 B.
    String body = net::http().getString();
    JsonDocument filter;
    filter["title"] = true;
    filter["subtitle"] = true;
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err == DeserializationError::Ok) {
      publish((const char*)(doc["title"] | ""), (const char*)(doc["subtitle"] | ""));
    } else {
      log_e("now-playing parse: %s", err.c_str());
    }
  } else if (code == 204) {
    publish("", "");
  } else {
    log_e("now-playing HTTP %d", code);
  }
  net::end();
}

void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int idx = g_requestedStation;
    if (idx >= 0 && idx < (int)catalog::count()) poll(catalog::at(idx));
  }
}

}  // namespace

namespace now_playing {

void tick(bool playing, int stationIndex, bool stationChanged) {
  if (!g_worker) {
    xTaskCreatePinnedToCore(workerTask, "wh_nowplay", 6144, nullptr, 1, &g_worker, 0);
  }

  uint32_t now = millis();
  if (stationChanged) {
    publish("", "");
    g_lastStation = stationIndex;
    g_nextPollAt = now;  // poll as soon as we're playing
  }
  if (!playing || stationIndex < 0) return;
  if (now < g_nextPollAt) return;

  g_nextPollAt = now + WH_NP_POLL_MS;
  if (!catalog::at(stationIndex).pollable) {
    publish("", "");
    return;
  }
  g_requestedStation = stationIndex;
  xTaskNotifyGive(g_worker);
}

NowPlaying current() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  NowPlaying copy = g_np;
  xSemaphoreGive(g_mux);
  return copy;
}

}  // namespace now_playing

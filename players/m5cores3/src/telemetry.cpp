#include "telemetry.h"

#include <Preferences.h>

#include "config.h"
#include "net.h"

namespace {

struct Event {
  char ev[6];
  char st[40];
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_worker = nullptr;
String g_tid;

String installId() {
  if (!g_tid.isEmpty()) return g_tid;
  Preferences prefs;
  if (prefs.begin("wh", false)) {
    g_tid = prefs.getString("tid", "");
    if (g_tid.isEmpty()) {
      char buf[37];
      snprintf(buf, sizeof(buf), "%08lx-%04lx-4%03lx-%04lx-%012llx",
               (unsigned long)esp_random(), (unsigned long)(esp_random() & 0xffff),
               (unsigned long)(esp_random() & 0xfff),
               (unsigned long)(0x8000 | (esp_random() & 0x3fff)),
               ((uint64_t)esp_random() << 16 | (esp_random() & 0xffff)) & 0xffffffffffffULL);
      g_tid = buf;
      prefs.putString("tid", g_tid);
    }
    prefs.end();
  }
  return g_tid;
}

void post(const Event& e) {
  String id = installId();
  if (id.isEmpty()) return;
  char body[220];
  snprintf(body, sizeof(body),
           "{\"v\":1,\"id\":\"%s\",\"p\":\"m5cores3\",\"ev\":\"%s\",\"st\":\"%s\","
           "\"app\":\"%s+%d\"}",
           id.c_str(), e.ev, e.st, WH_FW_VERSION, WH_FW_BUILD);
  if (!net::whBegin(WH_TELEMETRY_PATH)) return;
  net::http().addHeader("Content-Type", "application/json");
  int code = net::http().POST(String(body));
  net::end();
  log_d("telemetry %s %s -> %d", e.ev, e.st, code);
}

void workerTask(void*) {
  Event e;
  for (;;) {
    if (xQueueReceive(g_queue, &e, portMAX_DELAY) == pdTRUE) post(e);
  }
}

void enqueue(const char* ev, const String& st) {
  if (!g_queue) {
    g_queue = xQueueCreate(6, sizeof(Event));
    xTaskCreatePinnedToCore(workerTask, "wh_telemetry", 10240, nullptr, 1, &g_worker, 0);
  }
  Event e{};
  strlcpy(e.ev, ev, sizeof(e.ev));
  strlcpy(e.st, st.c_str(), sizeof(e.st));
  xQueueSend(g_queue, &e, 0);  // full queue = drop, never block
}

}  // namespace

namespace telemetry {

void tick(bool playing, const String& stationId) {
  static bool wasPlaying = false;
  static String lastStation;
  static uint32_t nextHb = 0;

  if (playing && (!wasPlaying || stationId != lastStation)) {
    if (wasPlaying && !lastStation.isEmpty()) enqueue("stop", lastStation);
    enqueue("start", stationId);
    lastStation = stationId;
    nextHb = millis() + WH_TELEMETRY_HB_MS;
  } else if (!playing && wasPlaying && !lastStation.isEmpty()) {
    enqueue("stop", lastStation);
    lastStation = "";
  } else if (playing && millis() > nextHb) {
    nextHb = millis() + WH_TELEMETRY_HB_MS;
    enqueue("hb", stationId);
  }
  wasPlaying = playing;
}

}  // namespace telemetry

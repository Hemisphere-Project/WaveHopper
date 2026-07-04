#include "fw_update.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#include "config.h"
#include "net.h"
#include "ui.h"

namespace {

// The contract publishes an absolute URL; the device only ever fetches from
// its own host — reduce to a path and reject anything else.
String hostRelativePath(const String& url) {
  String prefix = String("https://") + WH_CONTENT_HOST;
  if (url.startsWith(prefix + "/")) return url.substring(prefix.length());
  if (url.startsWith("/")) return url;
  return "";
}

}  // namespace

namespace fw_update {

void checkAndUpdate() {
  if (!net::whBegin(WH_FIRMWARE_MANIFEST_PATH)) return;
  int code = net::http().GET();
  if (code != 200) {
    log_e("fw: manifest GET -> %d", code);
    net::end();
    return;
  }
  String body = net::http().getString();
  net::end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    log_e("fw: manifest parse failed");
    return;
  }

  String board = (const char*)(doc["board"] | "");
  int build = doc["build"] | 0;
  String version = (const char*)(doc["version"] | "");
  String url = (const char*)(doc["url"] | "");
  String sha256 = (const char*)(doc["sha256"] | "");
  uint32_t size = doc["size"] | 0;

  if (board != "m5cores3") {
    log_e("fw: manifest board '%s' mismatch", board.c_str());
    return;
  }
  if (build <= WH_FW_BUILD || url.isEmpty() || size == 0 || sha256.length() != 64) {
    log_i("fw: up to date (remote build %d, ours %d)", build, WH_FW_BUILD);
    return;
  }

  String path = hostRelativePath(url);
  if (path.isEmpty()) {
    log_e("fw: refusing off-host url %s", url.c_str());
    return;
  }

  ui::bootLine("firmware: %s (build %d) ...", version.c_str(), build);
  if (!net::whBegin(path)) return;
  code = net::http().GET();
  if (code != 200) {
    log_e("fw: binary GET -> %d", code);
    net::end();
    return;
  }
  uint32_t contentLen = net::http().getSize();
  if (contentLen != size) {
    log_e("fw: size mismatch (manifest %lu, served %lu)", (unsigned long)size,
          (unsigned long)contentLen);
    net::end();
    return;
  }
  if (!Update.begin(size)) {
    log_e("fw: Update.begin failed (%s)", Update.errorString());
    net::end();
    return;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  Stream& stream = net::http().getStream();
  uint8_t buf[4096];
  uint32_t remaining = size;
  uint32_t lastPct = 0;
  uint32_t idleSince = millis();
  bool ioError = false;
  while (remaining > 0 && millis() - idleSince < 15000) {
    size_t got = stream.readBytes(buf, min((uint32_t)sizeof(buf), remaining));
    if (got == 0) continue;
    idleSince = millis();
    mbedtls_sha256_update(&sha, buf, got);
    if (Update.write(buf, got) != got) {
      ioError = true;
      break;
    }
    remaining -= got;
    uint32_t pct = 100 - remaining * 100 / size;
    if (pct >= lastPct + 20) {
      lastPct = pct;
      ui::bootLine("firmware: %lu%%", (unsigned long)pct);
    }
  }
  net::end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hexDigest[65];
  for (int i = 0; i < 32; ++i) sprintf(hexDigest + i * 2, "%02x", digest[i]);

  if (ioError || remaining > 0 || !sha256.equalsIgnoreCase(hexDigest)) {
    log_e("fw: verify failed (io=%d remaining=%lu) — aborting", ioError,
          (unsigned long)remaining);
    Update.abort();
    return;
  }
  if (!Update.end()) {  // finalizes + activates the inactive slot
    log_e("fw: Update.end failed (%s)", Update.errorString());
    return;
  }
  ui::bootLine("firmware: verified, rebooting ...");
  delay(500);
  ESP.restart();
}

}  // namespace fw_update

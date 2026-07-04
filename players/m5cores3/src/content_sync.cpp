#include "content_sync.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <mbedtls/sha256.h>

#include <vector>

#include "config.h"
#include "net.h"
#include "ui.h"

namespace {

struct RemoteFile {
  String path;
  String sha256;
  uint32_t size;
};

bool pathIsSafe(const String& p) {
  return !p.isEmpty() && p[0] != '/' && p.indexOf("..") < 0;
}

String hex(const uint8_t* digest, size_t len) {
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    char b[3];
    snprintf(b, sizeof(b), "%02x", digest[i]);
    out += b;
  }
  return out;
}

void mkdirs(const String& filePath) {
  // Create every parent directory (LittleFS mkdir is single-level).
  for (int i = 1; i < (int)filePath.length(); ++i) {
    if (filePath[i] == '/') LittleFS.mkdir(filePath.substring(0, i));
  }
}

void removeRecursive(const String& dirPath) {
  File dir = LittleFS.open(dirPath);
  if (!dir) return;
  if (!dir.isDirectory()) {
    dir.close();
    LittleFS.remove(dirPath);
    return;
  }
  std::vector<String> children;
  File entry;
  while ((entry = dir.openNextFile())) {
    children.push_back(dirPath + "/" + entry.name());
    entry.close();
  }
  dir.close();
  for (auto& child : children) removeRecursive(child);
  LittleFS.rmdir(dirPath);
}

// Streaming GET → staging file with sha256+size verification. 3 attempts.
bool downloadVerified(const String& packPath, const RemoteFile& rf,
                      const String& contentVersion) {
  String url = String(WH_CONTENT_BASE_PATH) + rf.path + "?v=" + contentVersion;
  String stagePath = String(WH_FS_STAGING_DIR) + "/" + rf.path;
  mkdirs(stagePath);

  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (!net::whBegin(url)) return false;
    int code = net::http().GET();
    if (code != 200) {
      log_e("sync GET %s -> %d (attempt %d)", rf.path.c_str(), code, attempt);
      net::end();
      continue;
    }

    File out = LittleFS.open(stagePath, "w");
    if (!out) {
      log_e("sync: cannot open %s", stagePath.c_str());
      net::end();
      return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    // Pack files are served static (Content-Length present) — read to length.
    Stream& stream = net::http().getStream();
    uint32_t remaining = net::http().getSize();
    uint8_t buf[2048];
    uint32_t written = 0;
    uint32_t idleSince = millis();
    while (remaining > 0 && millis() - idleSince < 10000) {
      size_t got = stream.readBytes(buf, min((uint32_t)sizeof(buf), remaining));
      if (got == 0) continue;  // readBytes already waited its timeout slice
      idleSince = millis();
      mbedtls_sha256_update(&sha, buf, got);
      out.write(buf, got);
      written += got;
      remaining -= got;
    }
    out.close();
    net::end();

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (written == rf.size && hex(digest, 32) == rf.sha256) return true;
    log_e("sync: %s verify failed (%lu/%lu bytes, attempt %d)", rf.path.c_str(),
          (unsigned long)written, (unsigned long)rf.size, attempt);
    LittleFS.remove(stagePath);
  }
  return false;
}

bool commitRename(const String& from, const String& to) {
  mkdirs(to);
  if (LittleFS.exists(to)) LittleFS.remove(to);  // lfs rename replaces, but be explicit
  if (!LittleFS.rename(from, to)) {
    log_e("sync: rename %s -> %s failed", from.c_str(), to.c_str());
    return false;
  }
  return true;
}

void pruneAgainst(const std::vector<RemoteFile>& files, const String& dirPath,
                  const String& relPrefix) {
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  std::vector<String> names;
  std::vector<bool> isDir;
  File entry;
  while ((entry = dir.openNextFile())) {
    names.push_back(entry.name());
    isDir.push_back(entry.isDirectory());
    entry.close();
  }
  dir.close();

  for (size_t i = 0; i < names.size(); ++i) {
    String rel = relPrefix.isEmpty() ? names[i] : relPrefix + "/" + names[i];
    if (isDir[i]) {
      pruneAgainst(files, dirPath + "/" + names[i], rel);
      continue;
    }
    if (rel == "manifest.json") continue;  // the commit marker
    bool listed = false;
    for (auto& f : files) {
      if (f.path == rel) {
        listed = true;
        break;
      }
    }
    if (!listed) {
      log_i("sync: pruning %s", rel.c_str());
      LittleFS.remove(dirPath + "/" + names[i]);
    }
  }
}

}  // namespace

namespace content_sync {

void wipeStagingOnBoot() { removeRecursive(WH_FS_STAGING_DIR); }

Result run() {
  // ---- fetch remote manifest (raw body kept — it becomes the local copy) --
  if (!net::whBegin(WH_CONTENT_MANIFEST_PATH)) return Result::Skipped;
  int code = net::http().GET();
  if (code != 200) {
    log_e("sync: manifest GET -> %d", code);
    net::end();
    return Result::Failed;
  }
  String manifestBody = net::http().getString();
  net::end();

  JsonDocument doc;
  if (deserializeJson(doc, manifestBody) != DeserializationError::Ok) {
    log_e("sync: manifest parse failed");
    return Result::Failed;
  }

  int schemaVersion = doc["schemaVersion"] | 0;
  String contentVersion = (const char*)(doc["contentVersion"] | "");
  uint32_t totalSize = doc["totalSize"] | 0;
  if (schemaVersion > WH_CONTENT_SCHEMA_SUPPORTED) {
    log_e("sync: remote schema %d > supported %d — skipping (fw update may help)",
          schemaVersion, WH_CONTENT_SCHEMA_SUPPORTED);
    return Result::Skipped;
  }
  if (contentVersion.isEmpty()) return Result::Failed;

  // ---- equality check against the local commit marker ---------------------
  String localVersion;
  std::vector<String> localShas;  // path\nsha pairs from the local manifest
  {
    File lf = LittleFS.open(String(WH_FS_CONTENT_DIR) + "/manifest.json", "r");
    if (lf) {
      JsonDocument ldoc;
      if (deserializeJson(ldoc, lf) == DeserializationError::Ok) {
        localVersion = (const char*)(ldoc["contentVersion"] | "");
        for (JsonObject f : ldoc["files"].as<JsonArray>()) {
          localShas.push_back(String((const char*)(f["path"] | "")) + "\n" +
                              (const char*)(f["sha256"] | ""));
        }
      }
      lf.close();
    }
  }
  if (localVersion == contentVersion) {
    log_i("sync: up to date (%.12s)", contentVersion.c_str());
    return Result::Unchanged;
  }
  ui::bootLine("content: updating to %.12s ...", contentVersion.c_str());

  // ---- free space ----------------------------------------------------------
  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (freeBytes < totalSize + 65536) {
    log_e("sync: not enough space (%u free, %lu needed)", freeBytes,
          (unsigned long)totalSize + 65536);
    return Result::Failed;
  }

  // ---- plan + download deltas into staging ---------------------------------
  std::vector<RemoteFile> files;
  std::vector<bool> changed;
  for (JsonObject f : doc["files"].as<JsonArray>()) {
    RemoteFile rf;
    rf.path = (const char*)(f["path"] | "");
    rf.sha256 = (const char*)(f["sha256"] | "");
    rf.size = f["size"] | 0;
    if (!pathIsSafe(rf.path) || rf.sha256.length() != 64) {
      log_e("sync: rejecting entry '%s'", rf.path.c_str());
      return Result::Failed;
    }
    String key = rf.path + "\n" + rf.sha256;
    bool have = LittleFS.exists(String(WH_FS_CONTENT_DIR) + "/" + rf.path);
    bool sameSha = false;
    for (auto& k : localShas) {
      if (k == key) {
        sameSha = true;
        break;
      }
    }
    files.push_back(rf);
    changed.push_back(!(have && sameSha));
  }

  int toFetch = 0;
  for (bool c : changed) toFetch += c;
  int fetched = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    if (!changed[i]) continue;
    fetched++;
    ui::bootLine("content: %d/%d %s", fetched, toFetch, files[i].path.c_str());
    if (!downloadVerified(files[i].path, files[i], contentVersion)) {
      log_e("sync: giving up on %s", files[i].path.c_str());
      removeRecursive(WH_FS_STAGING_DIR);
      return Result::Failed;
    }
  }

  // ---- ordered commit: everything else first, stations.json second, ------
  // ---- manifest last (the marker) -----------------------------------------
  for (int phase = 0; phase < 2; ++phase) {
    for (size_t i = 0; i < files.size(); ++i) {
      if (!changed[i]) continue;
      bool isCatalog = files[i].path == "stations.json";
      if ((phase == 0) == isCatalog) continue;
      if (!commitRename(String(WH_FS_STAGING_DIR) + "/" + files[i].path,
                        String(WH_FS_CONTENT_DIR) + "/" + files[i].path)) {
        return Result::Failed;  // marker not written — next boot re-syncs
      }
    }
  }
  {
    String markerPath = String(WH_FS_CONTENT_DIR) + "/manifest.json";
    mkdirs(markerPath);
    File mf = LittleFS.open(markerPath, "w");
    if (!mf) return Result::Failed;
    mf.print(manifestBody);
    mf.close();
  }

  pruneAgainst(files, WH_FS_CONTENT_DIR, "");
  removeRecursive(WH_FS_STAGING_DIR);
  ui::bootLine("content: updated (%.12s)", contentVersion.c_str());
  return Result::Updated;
}

}  // namespace content_sync

// Content pack sync — implements docs/CONTENT-API.md §Device sync verbatim:
// staging dir, sha256-verified streaming downloads with ?v= cache busting,
// ordered commit (icons → stations.json → local manifest last as the commit
// marker), implicit deletions, boot-time staging wipe.
#pragma once

namespace content_sync {

enum class Result {
  Unchanged,  // remote contentVersion == local — nothing to do
  Updated,    // sync committed
  Skipped,    // no net/clock, or remote schemaVersion newer than supported
  Failed,     // network/verify/commit error — local state untouched or self-healing
};

// Call once at boot before run(): clears half-committed leftovers.
void wipeStagingOnBoot();

// Blocking; run at boot before playback starts (TLS heap discipline).
Result run();

}  // namespace content_sync

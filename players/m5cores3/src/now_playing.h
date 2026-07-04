// Now-playing metadata: polls the shared /api/now-playing.php dispatcher every
// 30 s while playing (stations with a pollable nowPlaying type only), with the
// stream's ICY title as fallback. Contract: docs/CONTENT-API.md §Now-playing.
#pragma once

#include <Arduino.h>

struct NowPlaying {
  String title;
  String subtitle;
  uint32_t generation = 0;  // bumps on change
};

namespace now_playing {
// Call every loop. Polls when: playing, station pollable, poll interval due.
// stationChanged forces an immediate poll (and clears stale data).
void tick(bool playing, int stationIndex, bool stationChanged);
NowPlaying current();
}  // namespace now_playing

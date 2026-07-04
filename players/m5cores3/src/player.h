// Playback engine: owns the Audio instance and its network-pump task, plus the
// auto-play supervisor (retry once → skip; all failed → periodic sweep).
// Commands are queued to the player task (connecttohost blocks up to ~3 s);
// the supervisor runs in the UI task via tick().
#pragma once

#include <Arduino.h>

#include "audio_out.h"

enum class PlayerState : uint8_t { Idle, Tuning, Playing, AllFailed, WifiLost };

struct PlayerSnapshot {
  PlayerState state = PlayerState::Idle;
  int stationIndex = -1;
  uint8_t volume = 12;
  uint32_t generation = 0;  // bumps on any visible change — cheap UI dirty flag
  char streamTitle[160] = "";  // latest ICY title ("" if none)
};

namespace player {
// Spawns the player task. Catalog must be loaded first.
void begin(AudioProfile profile, uint8_t volume, int firstStationIndex);

void next();
void prev();
void setVolume(uint8_t v);  // 0..21, clamped

// Supervisor pump — call from loop() at ~10 Hz+. Handles tune deadlines,
// retry/skip, stall detection, all-failed sweeps, amp health, wifi loss.
void tick();

PlayerSnapshot snapshot();
const char* stateName(PlayerState s);
}  // namespace player

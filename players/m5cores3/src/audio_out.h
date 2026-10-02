// Audio output: Module Audio (M144, ES8388 headphone/line) or the RCA module
// (Module13.2, PCM5102A line-out) on the M-Bus — the same two on every board.
// The CoreS3's built-in speaker (AW88298 amp) is not an output any more
// (dropped 2026-10-02); it shares the I2S data line, so init() keeps it dark.
// Pin sets per board: include/board.h; chip facts: CLAUDE.md.
#pragma once

#include <Arduino.h>

#include "wh_nvs.h"

enum class AudioProfile : uint8_t { Rca, ModuleAudio };

struct AudioPins {
  int8_t bclk, lrck, dout, mclk;  // mclk -1 = unused
};

namespace audio_out {

// Map the stored setting to a profile. Auto = Module Audio when its STM32
// helper answers at I2C 0x33 (never probe 0x10 — the BMM150 answers there),
// else RCA (a bare PCM5102A: unprobeable, so it is the default). fellBack is
// set when an explicit Module Audio choice failed the probe.
AudioProfile resolve(AudioOutSetting setting, bool& fellBack);

AudioPins pins(AudioProfile p);
const char* name(AudioProfile p);  // "rca" | "module-audio"

// One-time hardware bring-up (codec registers, speaker amp off). Returns
// false if the hardware refused — the caller falls back to RCA.
bool init(AudioProfile p);

}  // namespace audio_out

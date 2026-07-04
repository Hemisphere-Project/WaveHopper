// Audio output profile selection + hardware bring-up.
// Three outputs share GPIO13 as I2S data — exactly one profile is active.
// Facts and pin table: include/config.h + CLAUDE.md.
#pragma once

#include <Arduino.h>

#include "wh_nvs.h"

enum class AudioProfile : uint8_t { Internal, Rca, ModuleAudio };

struct AudioPins {
  int8_t bclk, lrck, dout, mclk;  // mclk -1 = unused
};

namespace audio_out {

// Map the stored setting to a concrete profile. Auto probes for Module Audio
// (I2C 0x33 — its STM32 helper; never 0x10, the internal BMM150 collides).
// RCA is unprobeable (dumb PCM5102A) and only ever reached explicitly.
// fellBack is set when an explicit ModuleAudio setting failed the probe.
AudioProfile resolve(AudioOutSetting setting, bool& fellBack);

AudioPins pins(AudioProfile p);
const char* name(AudioProfile p);

// One-time hardware init for the profile (I2C amp/codec setup, power rails).
// Returns false if the hardware refused — caller should fall back to Internal.
bool init(AudioProfile p, uint32_t initialSampleRate);

// The internal AW88298 is BCK-clocked but its AGC/boost timing tracks a rate
// register — call whenever the stream sample rate changes. No-op for others.
void onSampleRate(AudioProfile p, uint32_t rate);

}  // namespace audio_out

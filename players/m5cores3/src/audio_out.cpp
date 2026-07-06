#include "audio_out.h"

#include <M5Module_Audio.h>
#include <M5Unified.h>
#include <Wire.h>

#include "config.h"

namespace {

constexpr uint32_t kI2cFreq = 400000;

// ES8388 codec on the Module Audio (M144). Wire (port 0) shares the physical
// bus with M5.In_I2C (port 1): only ever touched here, at boot, before the UI
// loop starts polling the touch controller — sequential access, no collisions.
M5ModuleAudio g_moduleAudio;

void aw88298Write(uint8_t reg, uint16_t val) {
  // MSB first on the wire — matches M5Unified's aw88298_write_reg, which
  // bswap16s the value before writing. Verified against live register dumps.
  uint8_t buf[2] = {uint8_t(val >> 8), uint8_t(val & 0xff)};
  if (!M5.In_I2C.writeRegister(WH_I2C_AW88298, reg, buf, 2, kI2cFreq)) {
    log_e("AW88298 write reg 0x%02x failed", reg);
  }
}

uint16_t aw88298Read(uint8_t reg) {
  uint8_t buf[2] = {0, 0};
  M5.In_I2C.readRegister(WH_I2C_AW88298, reg, buf, 2, kI2cFreq);
  return (uint16_t(buf[0]) << 8) | buf[1];
}

// AW88298 reg 0x06 rate index — M5Unified's exact computation: rounded
// division, then the first table entry ≥ it. 44.1k → 7, 48k → 8.
uint16_t aw88298RateIndex(uint32_t rate) {
  static constexpr uint8_t tbl[] = {4, 5, 6, 8, 10, 11, 15, 20, 22, 44};
  uint32_t target = (rate + 1102) / 2205;
  uint16_t i = 0;
  while (i < sizeof(tbl) - 1 && tbl[i] < target) ++i;
  return i;
}

// Reg 0x06 (I2SCTRL) base: I2SRXEN=1, CHSEL=11 (mono mix L+R — one speaker),
// I2SMD=00 (Philips), I2SFS=11 (32-bit), I2SBCK=10 (64*fs). ESP32-audioI2S
// drives 32-bit I2S slots, so BCLK is 64*fs — M5Unified's 0x14C0 (I2SBCK=32*fs,
// for M5.Speaker's 16-bit slots) leaves the amp PLL unlocked and silent here.
constexpr uint16_t kAmpI2sCtrlBase = 0x1CE0;

void internalAmpEnable(uint32_t sampleRate) {
  // Speaker power rail via the AW9523 IO expander, then the amp registers —
  // sequence mirrors M5Unified's _speaker_enabled_cb_cores3 (with the I2SCTRL
  // value adapted to audioI2S's slot format, see kAmpI2sCtrlBase).
  M5.In_I2C.bitOn(WH_I2C_AW9523, 0x02, 0b00000100, kI2cFreq);
  aw88298Write(0x61, 0x0673);  // boost config
  aw88298Write(0x04, 0x4040);  // I2SEN=1
  aw88298Write(0x05, 0x0008);  // unmute
  aw88298Write(0x06, aw88298RateIndex(sampleRate) | kAmpI2sCtrlBase);
  aw88298Write(0x0C, 0x0064);  // amp volume full (soft volume lives in audioI2S)
  log_i("internal amp enabled (rate=%lu)", (unsigned long)sampleRate);
}

void internalAmpDisable() {
  aw88298Write(0x04, 0x4000);
  M5.In_I2C.bitOff(WH_I2C_AW9523, 0x02, 0b00000100, kI2cFreq);
}

}  // namespace

namespace audio_out {

AudioProfile resolve(AudioOutSetting setting, bool& fellBack) {
  fellBack = false;
  switch (setting) {
    case AudioOutSetting::Internal:
      return AudioProfile::Internal;
    case AudioOutSetting::Rca:
      return AudioProfile::Rca;
    case AudioOutSetting::ModuleAudio:
      if (M5.In_I2C.scanID(WH_I2C_MODAUDIO)) return AudioProfile::ModuleAudio;
      log_e("Module Audio selected but not found at 0x33 — falling back to internal");
      fellBack = true;
      return AudioProfile::Internal;
    case AudioOutSetting::Auto:
    default:
      if (M5.In_I2C.scanID(WH_I2C_MODAUDIO)) {
        log_i("auto: Module Audio detected at 0x33");
        return AudioProfile::ModuleAudio;
      }
      return AudioProfile::Internal;
  }
}

AudioPins pins(AudioProfile p) {
  switch (p) {
    case AudioProfile::Rca:         return AudioPins WH_PINS_RCA;
    case AudioProfile::ModuleAudio: return AudioPins WH_PINS_MODULE;
    case AudioProfile::Internal:
    default:                        return AudioPins WH_PINS_INTERNAL;
  }
}

const char* name(AudioProfile p) {
  switch (p) {
    case AudioProfile::Rca:         return "rca";
    case AudioProfile::ModuleAudio: return "module-audio";
    case AudioProfile::Internal:
    default:                        return "internal";
  }
}

bool init(AudioProfile p, uint32_t initialSampleRate) {
  switch (p) {
    case AudioProfile::Internal:
      internalAmpEnable(initialSampleRate);
      return true;
    case AudioProfile::Rca:
      // PCM5102A needs nothing; make sure the internal amp stays dark even
      // though it shares GPIO13 (it has no BCLK either, belt and braces).
      internalAmpDisable();
      return true;
    case AudioProfile::ModuleAudio: {
      internalAmpDisable();  // shared GPIO13 data line — keep the amp dark
      // I2C-only begin: configures the ES8388, leaves I2S to ESP32-audioI2S.
      // Prerequisite: the module's physical pin switch must be on B (CoreS3).
      if (!g_moduleAudio.begin(Wire, 12, 11, WH_I2C_MODAUDIO, kI2cFreq)) {
        log_e("Module Audio codec init failed");
        return false;
      }
      g_moduleAudio.setSpeakerOutput(DAC_OUTPUT_OUT1);  // TRRS headphone out
      g_moduleAudio.setSampleRate(SAMPLE_RATE_48K);     // matches pinned I2S clock
      g_moduleAudio.setSpeakerVolume(80);  // codec level; soft volume in audioI2S
      // The lib's codec init leaves a mic monitor path wide open: output
      // mixers at 0xd0 (= DAC + mic/line amp mixed in — its own comment says
      // 0x90 is DAC-only) with micbias powered and the PGA at 24 dB
      // (ADCPOWER 0x00, ADCCONTROL1 0x88). On a TRRS headset the biased,
      // amplified mic feeds the headphones = constant background noise. This
      // radio never records: DAC-only mixers, whole ADC/micbias path off.
      {
        auto es8388Write = [](uint8_t reg, uint8_t val) {
          Wire.beginTransmission(0x10);  // ES8388 (lib writes it the same way)
          Wire.write(reg);
          Wire.write(val);
          Wire.endTransmission();
        };
        es8388Write(0x27, 0x90);  // DACCONTROL17: left mixer = DAC only
        es8388Write(0x2A, 0x90);  // DACCONTROL20: right mixer = DAC only
        es8388Write(0x03, 0xFF);  // ADCPOWER: ADC + PGA + micbias all down
      }
      // Wire (I2C0) and M5.In_I2C (I2C1) are two separate ESP32 I2C
      // peripherals wired to the SAME physical pins (12/11). Wire.begin()
      // above re-muxed those GPIOs onto I2C0, which silently detaches I2C1 —
      // left attached, it corrupts every later In_I2C transaction (the touch
      // controller runs on In_I2C and starts reading garbage coordinates).
      // The codec needs Wire only for this one-shot register setup — release
      // it and re-claim the pins for In_I2C now, before the UI loop starts.
      Wire.end();
      M5.In_I2C.begin(I2C_NUM_1, 12, 11);
      log_i("Module Audio (ES8388) initialized");
      return true;
    }
  }
  return false;
}

void onSampleRate(AudioProfile p, uint32_t rate) {
  if (p != AudioProfile::Internal) return;
  // audioI2S restarts the I2S clock when the stream's real rate differs from
  // the current one. The clock interruption faults the AW88298 (SYSST.SWS
  // drops and never returns via plain register writes — verified on hardware).
  // Recovery = hard power cycle through the AW9523 rail with clocks live, then
  // the full enable sequence, then wait for the switcher (SWS, bit8).
  uint16_t sysst = aw88298Read(0x01);
  if ((sysst & 0x0100) == 0) {
    log_i("AW88298 faulted (sysst=%04x) — power cycling", sysst);
    internalAmpDisable();
    vTaskDelay(pdMS_TO_TICKS(20));
    internalAmpEnable(rate);
    uint32_t deadline = millis() + 300;
    while ((aw88298Read(0x01) & 0x0100) == 0 && millis() < deadline) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    log_i("AW88298 recovered (sysst=%04x)", aw88298Read(0x01));
    return;
  }
  // Healthy: touch I2SCTRL only when the target actually changed — rewriting
  // a live amp register glitches the output audibly.
  uint16_t want = aw88298RateIndex(rate) | kAmpI2sCtrlBase;
  if (aw88298Read(0x06) != want) {
    aw88298Write(0x06, want);
    log_i("AW88298 rate set %lu Hz", (unsigned long)rate);
  }
}

}  // namespace audio_out

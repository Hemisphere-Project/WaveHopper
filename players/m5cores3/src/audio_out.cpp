#include "audio_out.h"

#include <M5Module_Audio.h>
#include <M5Unified.h>
#include <Wire.h>

#include "config.h"

namespace {

constexpr uint32_t kI2cFreq = 400000;

// ES8388 codec on the Module Audio (M144). Wire (port 0) shares the physical
// bus with M5.In_I2C: only ever touched here, at boot, before the UI loop
// starts polling the touch controller — sequential access, no collisions.
M5ModuleAudio g_moduleAudio;

void speakerAmpOff() {
#if WH_HAS_SPEAKER_AMP
  // CoreS3: the AW88298 sits on the same I2S data line (GPIO13) — disable it
  // (reg 0x04 I2SEN off, 16-bit register written MSB first) and drop its
  // power rail (AW9523 reg 0x02 bit2), so only the module drives the audio.
  uint8_t i2sOff[2] = {0x40, 0x00};
  if (!M5.In_I2C.writeRegister(WH_I2C_AW88298, 0x04, i2sOff, 2, kI2cFreq))
    log_w("AW88298 not answering (amp off write)");
  M5.In_I2C.bitOff(WH_I2C_AW9523, 0x02, 0b00000100, kI2cFreq);
#endif
}

}  // namespace

namespace audio_out {

AudioProfile resolve(AudioOutSetting setting, bool& fellBack) {
  fellBack = false;
  const bool module = M5.In_I2C.scanID(WH_I2C_MODAUDIO);
  if (setting == AudioOutSetting::Rca) return AudioProfile::Rca;
  if (setting == AudioOutSetting::ModuleAudio && !module) {
    log_e("Module Audio selected but not found at 0x33 — falling back to RCA");
    fellBack = true;
    return AudioProfile::Rca;
  }
  if (module) {
    log_i("Module Audio detected at 0x33");
    return AudioProfile::ModuleAudio;
  }
  return AudioProfile::Rca;  // auto without a Module Audio
}

AudioPins pins(AudioProfile p) {
  return p == AudioProfile::ModuleAudio ? AudioPins WH_PINS_MODULE : AudioPins WH_PINS_RCA;
}

const char* name(AudioProfile p) { return p == AudioProfile::ModuleAudio ? "module-audio" : "rca"; }

bool init(AudioProfile p) {
  speakerAmpOff();
  switch (p) {
    case AudioProfile::Rca:
      return true;  // PCM5102A: no control interface, clocks itself from BCLK
    case AudioProfile::ModuleAudio: {
      // I2C-only begin: configures the ES8388, leaves I2S to ESP32-audioI2S.
      // Prerequisite: the module's physical pin switch — B on the CoreS3,
      // A (factory default) on the Fire.
      const int sda = M5.getPin(m5::pin_name_t::in_i2c_sda);
      const int scl = M5.getPin(m5::pin_name_t::in_i2c_scl);
      const i2c_port_t inPort = (i2c_port_t)M5.In_I2C.getPort();
      if (!g_moduleAudio.begin(Wire, sda, scl, WH_I2C_MODAUDIO, kI2cFreq)) {
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
      // Wire (Arduino driver, I2C0) and M5.In_I2C (M5Unified's own driver)
      // sit on the SAME physical pins — CoreS3: I2C1 on 12/11, Fire: even the
      // same I2C0 port on 21/22. Wire.begin() above re-muxed those GPIOs to
      // its own driver, which silently detaches In_I2C — left that way, every
      // later In_I2C transaction is corrupt (on the CoreS3 the touch
      // controller starts reading garbage coordinates). The codec needs Wire
      // only for this one-shot register setup — release it and re-claim the
      // pins for In_I2C now, before the UI loop starts.
      Wire.end();
      M5.In_I2C.begin(inPort, sda, scl);
      log_i("Module Audio (ES8388) initialized");
      return true;
    }
  }
  return false;
}

}  // namespace audio_out

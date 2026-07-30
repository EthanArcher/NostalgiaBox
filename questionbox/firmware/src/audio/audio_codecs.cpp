/*****************************************************************************
 * V2 board audio codec setup (ES7210 mic + ES8311 speaker).
 *
 * The ESP32-S3-Touch-LCD-1.85C-BOX (V2) does NOT expose a raw MEMS mic or a
 * simple DAC. Both the microphone and the speaker are behind I2C codecs that
 * must be programmed before any audio flows:
 *
 *   - ES7210 (addr 0x40): 4-channel ADC used for the mics. Configured for
 *     16 kHz / 16-bit I2S capture with mic bias + gain (proven in mic_test).
 *   - ES8311 (addr 0x18): mono DAC used for the speaker. Configured for
 *     24 kHz / 16-bit I2S playback (proven in Waveshare's audio-out example).
 *
 * MCLK is supplied by the I2S peripheral on GPIO2 at 256x the sample rate, so
 * both codecs are told mclk = rate * 256. The speaker amplifier is enabled by
 * driving GPIO15 HIGH.
 *
 * These codecs share ONE I2S bus (see audio_bus.c). Because record (16 kHz) and
 * playback (24 kHz) never happen at the same time, the bus clock is switched
 * between the two rates; MCLK follows at 256x, matching each codec while it is
 * the one in use.
 *****************************************************************************/
#include "audio_codecs.h"
#include <Arduino.h>
#include "es7210.h"
#include "es8311.h"

#define PA_ENABLE_PIN 15
#define MIC_RATE      16000
#define SPK_RATE      16000   // match the mic rate: one clock, less audio data
#define MCLK_MULTIPLE 256

static es7210_dev_handle_t s_es7210 = NULL;
static es8311_handle_t     s_es8311 = NULL;

static bool init_mic_codec(void)
{
  es7210_i2c_config_t i2c_conf = {};
  i2c_conf.i2c_addr = ES7210_ADDRRES_00;   // 0x40
  if (es7210_new_codec(&i2c_conf, &s_es7210) != ESP_OK) {
    Serial.println("[codec] ES7210 new_codec FAILED");
    return false;
  }
  es7210_codec_config_t cc = {};
  cc.i2s_format     = ES7210_I2S_FMT_I2S;
  cc.mclk_ratio     = MCLK_MULTIPLE;
  cc.sample_rate_hz = MIC_RATE;
  cc.bit_width      = ES7210_I2S_BITS_16B;
  cc.mic_bias       = ES7210_MIC_BIAS_2V87;
  cc.mic_gain       = ES7210_MIC_GAIN_36DB;
  cc.flags.tdm_enable = false;
  if (es7210_config_codec(s_es7210, &cc) != ESP_OK) {
    Serial.println("[codec] ES7210 config FAILED");
    return false;
  }
  es7210_config_volume(s_es7210, 40);
  Serial.println("[codec] ES7210 mic configured @16kHz");
  return true;
}

static bool init_spk_codec(void)
{
  s_es8311 = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);   // 0x18; uses Wire internally
  if (!s_es8311) {
    Serial.println("[codec] ES8311 create FAILED");
    return false;
  }
  es8311_clock_config_t clk = {};
  clk.mclk_inverted     = false;
  clk.sclk_inverted     = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency    = SPK_RATE * MCLK_MULTIPLE;   // 4.096 MHz @16kHz
  clk.sample_frequency  = SPK_RATE;
  if (es8311_init(s_es8311, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    Serial.println("[codec] ES8311 init FAILED");
    return false;
  }
  es8311_voice_volume_set(s_es8311, 60, NULL);  // codec base level; app gain rides on top
  es8311_microphone_config(s_es8311, false);   // don't use ES8311's own mic
  Serial.println("[codec] ES8311 speaker configured @16kHz");
  return true;
}

bool AudioCodecs_Init(void)
{
  bool mic_ok = init_mic_codec();
  bool spk_ok = init_spk_codec();

  // Enable the speaker amplifier.
  pinMode(PA_ENABLE_PIN, OUTPUT);
  digitalWrite(PA_ENABLE_PIN, HIGH);

  return mic_ok && spk_ok;
}

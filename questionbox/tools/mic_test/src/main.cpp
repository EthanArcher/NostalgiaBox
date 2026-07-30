/*****************************************************************************
 * WonderBox microphone test — V2 board (ES7210 codec).
 *
 * The board is a V2: the mic is an ES7210 codec that must be configured over
 * I2C, and audio uses the SHARED I2S bus (MCLK=2, BCK=48, WS=38, DIN=39).
 * This test sets up the ES7210 exactly like Waveshare's example, then prints
 * the mic level. Talk near the mic; if "mic peak" rises, the mic works.
 *****************************************************************************/
#include <Arduino.h>
#include <Wire.h>
#include "ESP_I2S.h"
#include "es7210.h"

#define I2S_MCK 2
#define I2S_BCK 48
#define I2S_WS  38
#define I2S_DIN 39
#define I2S_DOUT 47
#define PIN_SDA 11
#define PIN_SCL 10

static I2SClass i2s;
static es7210_dev_handle_t es7210 = NULL;
static bool s_ok = false;

static void scanI2C() {
  Serial.println("--- I2C scan ---");
  for (uint8_t a = 1; a < 0x7F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf("  found 0x%02X\n", a);
  }
  Serial.println("----------------");
}

static void es7210_setup() {
  es7210_i2c_config_t i2c_conf = {};
  i2c_conf.i2c_addr = 0x40;
  if (es7210_new_codec(&i2c_conf, &es7210) != ESP_OK) {
    Serial.println("ES7210 new_codec FAILED");
    return;
  }
  es7210_codec_config_t cc = {};
  cc.i2s_format = ES7210_I2S_FMT_I2S;
  cc.mclk_ratio = 256;
  cc.sample_rate_hz = 16000;
  cc.bit_width = ES7210_I2S_BITS_16B;
  cc.mic_bias = ES7210_MIC_BIAS_2V87;
  cc.mic_gain = ES7210_MIC_GAIN_36DB;
  cc.flags.tdm_enable = false;
  es7210_config_codec(es7210, &cc);
  es7210_config_volume(es7210, 40);
  Serial.println("ES7210 configured");
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n=== WonderBox MIC TEST (V2 / ES7210) ===");

  Wire.begin(PIN_SDA, PIN_SCL);
  scanI2C();
  es7210_setup();

  i2s.setPins(I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCK);   // note: MCLK included
  i2s.setTimeout(200);
  s_ok = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  Serial.println(s_ok ? "I2S started. Talk into the mic and watch 'mic peak'."
                      : "ERROR: i2s.begin FAILED");
}

void loop() {
  if (!s_ok) {
    static uint32_t t = 0;
    if (millis() - t > 1000) { t = millis(); Serial.println("i2s begin FAILED"); }
    return;
  }
  static uint8_t buf[2048];
  int n = i2s.readBytes((char *)buf, sizeof(buf));
  if (n <= 0) return;

  int16_t *s = (int16_t *)buf;
  int count = n / 2;
  int32_t peak = 0;
  int64_t sumsq = 0;
  for (int i = 0; i < count; i++) {
    int32_t a = s[i] < 0 ? -s[i] : s[i];
    if (a > peak) peak = a;
    sumsq += (int64_t)s[i] * s[i];
  }
  uint32_t rms = count ? (uint32_t)sqrt((double)sumsq / count) : 0;

  static uint32_t last = 0;
  if (millis() - last > 500) {
    last = millis();
    Serial.printf("mic peak = %5d   rms = %5u   %s\n",
                  (int)peak, (unsigned)rms, peak > 500 ? "<-- HEARING SOUND" : "(quiet)");
  }
}

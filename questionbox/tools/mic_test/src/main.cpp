/*****************************************************************************
 * WonderBox microphone test.
 *
 * Uses the SAME microphone driver Waveshare's official demo uses (ESP_I2S,
 * new I2S driver), on the board's mic pins. It records continuously and prints
 * the audio level twice a second. Talk/tap near the mic:
 *
 *   - If "mic peak" jumps to hundreds/thousands when you speak -> the mic
 *     hardware WORKS (and I'll switch WonderBox to this exact driver).
 *   - If it stays ~0 no matter how loud you are -> the mic hardware is faulty.
 *
 * Mic pins (Waveshare wiki): BCK=GPIO15, WS=GPIO2, DIN=GPIO39.
 *****************************************************************************/
#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>

static I2SClass i2s;
static bool s_ok = false;

// Scan the I2C bus and report which audio chips are present. This tells us the
// board revision: V1 (PCM5101 + MEMS mic) vs V2 (ES8311 + ES7210 codecs).
static void scanI2C() {
  Wire.begin(11 /*SDA*/, 10 /*SCL*/);
  Serial.println("--- I2C scan ---");
  int found = 0;
  bool es8311 = false, es7210 = false;
  for (uint8_t a = 1; a < 0x7F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X\n", a);
      found++;
      if (a == 0x18) es8311 = true;                 // ES8311 codec
      if (a == 0x40 || a == 0x41 || a == 0x42 || a == 0x43) es7210 = true; // ES7210 ADC
    }
  }
  if (found == 0) Serial.println("  (no I2C devices found)");
  if (es8311 || es7210) {
    Serial.println(">>> This looks like a V2 board (ES8311/ES7210 codecs). The");
    Serial.println(">>> mic/speaker need I2C codec setup - tell your assistant!");
  } else {
    Serial.println(">>> No audio codecs found -> looks like a V1 board (PCM5101 + MEMS mic).");
  }
  Serial.println("----------------");
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n=== WonderBox MIC TEST (ESP_I2S, no PSRAM) ===");
  scanI2C();

  // SDOUT = -1 (we only receive), MCLK = -1 (not needed for this mic).
  i2s.setPins(15 /*BCK*/, 2 /*WS*/, -1 /*SDOUT*/, 39 /*DIN*/, -1 /*MCLK*/);
  i2s.setTimeout(200);
  s_ok = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  Serial.println(s_ok ? "Mic started OK. Talk into it and watch 'mic peak'."
                      : "ERROR: i2s.begin FAILED");
}

void loop() {
  if (!s_ok) {
    static uint32_t t = 0;
    if (millis() - t > 1000) { t = millis(); Serial.println("mic begin FAILED - driver did not start"); }
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
                  (int)peak, (unsigned)rms,
                  peak > 500 ? "<-- HEARING SOUND" : "(quiet)");
  }
}

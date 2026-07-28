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

static I2SClass i2s;

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n=== WonderBox MIC TEST (ESP_I2S) ===");

  // SDOUT = -1 (we only receive), MCLK = -1 (not needed for this mic).
  i2s.setPins(15 /*BCK*/, 2 /*WS*/, -1 /*SDOUT*/, 39 /*DIN*/, -1 /*MCLK*/);
  i2s.setTimeout(200);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("ERROR: i2s.begin failed");
  } else {
    Serial.println("Mic started. Talk into the mic and watch 'mic peak' below.");
  }
}

void loop() {
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

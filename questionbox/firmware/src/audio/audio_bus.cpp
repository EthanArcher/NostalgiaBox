#include "audio_bus.h"
#include <Arduino.h>

// Mic (I2S MEMS)
#define MIC_BCK 15
#define MIC_WS  2
#define MIC_DIN 39
// Speaker (PCM5101 DAC)
#define SPK_BCK 48
#define SPK_WS  38
#define SPK_DOUT 47

I2SClass wb_i2s;
static bool s_active = false;

bool AudioBus_BeginMic()
{
  if (s_active) AudioBus_End();
  wb_i2s.setPins(MIC_BCK, MIC_WS, -1 /*dout*/, MIC_DIN, -1 /*mclk*/);
  if (!wb_i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[audio] mic begin FAILED");
    return false;
  }
  s_active = true;
  return true;
}

bool AudioBus_BeginSpeaker()
{
  if (s_active) AudioBus_End();
  wb_i2s.setPins(SPK_BCK, SPK_WS, SPK_DOUT, -1 /*din*/, -1 /*mclk*/);
  if (!wb_i2s.begin(I2S_MODE_STD, 24000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[audio] speaker begin FAILED");
    return false;
  }
  s_active = true;
  return true;
}

void AudioBus_End()
{
  if (!s_active) return;
  wb_i2s.end();
  s_active = false;
}

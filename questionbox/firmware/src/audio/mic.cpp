#include "mic.h"
#include "audio_bus.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

#define REC_RATE        16000
#define REC_MAX_SEC     15                  // safety cap; normally you tap to stop
#define REC_MAX_SAMPLES (REC_RATE * REC_MAX_SEC)
#define WAV_HEADER_LEN  44

static uint8_t *s_buf = nullptr;     // [WAV header | mono int16 PCM]
static size_t   s_count = 0;
static bool     s_recording = false;

static inline int16_t clamp16(int32_t v)
{
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static void write_wav_header(uint8_t *h, uint32_t dataLen, uint32_t rate)
{
  const uint16_t channels = 1, bits = 16;
  const uint32_t byteRate = rate * channels * bits / 8;
  const uint16_t blockAlign = channels * bits / 8;
  memcpy(h + 0, "RIFF", 4);
  *(uint32_t *)(h + 4) = 36 + dataLen;
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  *(uint32_t *)(h + 16) = 16;
  *(uint16_t *)(h + 20) = 1;
  *(uint16_t *)(h + 22) = channels;
  *(uint32_t *)(h + 24) = rate;
  *(uint32_t *)(h + 28) = byteRate;
  *(uint16_t *)(h + 32) = blockAlign;
  *(uint16_t *)(h + 34) = bits;
  memcpy(h + 36, "data", 4);
  *(uint32_t *)(h + 40) = dataLen;
}

bool Mic_Begin()
{
  s_buf = (uint8_t *)heap_caps_malloc(WAV_HEADER_LEN + REC_MAX_SAMPLES * 2, MALLOC_CAP_SPIRAM);
  if (!s_buf) {
    Serial.println("[mic] FAILED to allocate PSRAM record buffer");
    return false;
  }
  AudioBus_Init();          // mic + speaker channels are created once, here
  Serial.println("[mic] buffer ready");
  return true;
}

void Mic_Start()
{
  AudioBus_MicFlush();      // drop stale samples so we start clean
  s_count = 0;
  s_recording = true;
  Serial.println("[mic] recording started");
}

void Mic_Stop()
{
  if (!s_recording) return;
  s_recording = false;
  Serial.printf("[mic] recording stopped (%u samples, %.2fs)\n",
                (unsigned)s_count, (float)s_count / REC_RATE);
}

bool Mic_Poll()
{
  if (!s_recording) return false;

  uint8_t tmp[2048];
  size_t got = AudioBus_MicRead(tmp, sizeof(tmp));
  if (got >= 4) {
    int frames = got / 4;
    int16_t *in = (int16_t *)tmp;
    int16_t *out = (int16_t *)(s_buf + WAV_HEADER_LEN);
    for (int i = 0; i < frames && s_count < REC_MAX_SAMPLES; i++) {
      // Average the two slots (not sum) so a mic that duplicates onto both
      // channels doesn't clip/distort the recording.
      int32_t mono = ((int32_t)in[i * 2] + (int32_t)in[i * 2 + 1]) / 2;
      out[s_count++] = clamp16(mono);
    }
  }

  // Stop only when the child taps again (handled in main) or the safety cap hits.
  if (s_count >= REC_MAX_SAMPLES) {
    Mic_Stop();
    return false;
  }
  return true;
}

bool Mic_GetWav(const uint8_t **data, size_t *len)
{
  if (!s_buf || s_count == 0) return false;
  uint32_t dataLen = s_count * 2;
  write_wav_header(s_buf, dataLen, REC_RATE);
  *data = s_buf;
  *len = WAV_HEADER_LEN + dataLen;
  return true;
}

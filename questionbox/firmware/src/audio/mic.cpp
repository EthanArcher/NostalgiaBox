#include "mic.h"
#include "audio_bus.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

#define REC_RATE        16000
#define REC_MAX_SEC     10
#define REC_MAX_SAMPLES (REC_RATE * REC_MAX_SEC)
#define WAV_HEADER_LEN  44

#define SILENCE_RMS      700
#define SILENCE_MS       1200
#define MIN_RECORD_MS    400

static uint8_t *s_buf = nullptr;     // [WAV header | mono int16 PCM]
static size_t   s_count = 0;
static bool     s_recording = false;

static bool     s_speech = false;
static uint32_t s_start_ms = 0;
static uint32_t s_last_loud_ms = 0;

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
  // Only allocate the record buffer here. The I2S bus is enabled on demand in
  // Mic_Start() (shared with the speaker), so nothing fights over the hardware.
  s_buf = (uint8_t *)heap_caps_malloc(WAV_HEADER_LEN + REC_MAX_SAMPLES * 2, MALLOC_CAP_SPIRAM);
  if (!s_buf) {
    Serial.println("[mic] FAILED to allocate PSRAM record buffer");
    return false;
  }
  Serial.println("[mic] buffer ready");
  return true;
}

void Mic_Start()
{
  if (!AudioBus_BeginMic()) {
    Serial.println("[mic] could not enable mic bus");
    s_recording = false;
    return;
  }
  s_count = 0;
  s_recording = true;
  s_speech = false;
  s_start_ms = millis();
  s_last_loud_ms = s_start_ms;
  Serial.println("[mic] recording started");
}

void Mic_Stop()
{
  if (!s_recording) return;
  s_recording = false;
  AudioBus_End();
  Serial.printf("[mic] recording stopped (%u samples, %.2fs)\n",
                (unsigned)s_count, (float)s_count / REC_RATE);
}

bool Mic_Poll()
{
  if (!s_recording) return false;

  uint8_t tmp[2048];
  int avail = wb_i2s.available();
  if (avail > 0) {
    int n = avail;
    if (n > (int)sizeof(tmp)) n = sizeof(tmp);
    n &= ~0x3;
    if (n > 0) {
      int got = wb_i2s.readBytes((char *)tmp, n);
      int frames = got / 4;
      int16_t *in = (int16_t *)tmp;
      int16_t *out = (int16_t *)(s_buf + WAV_HEADER_LEN);
      uint64_t energy = 0;
      for (int i = 0; i < frames && s_count < REC_MAX_SAMPLES; i++) {
        int32_t mono = (int32_t)in[i * 2] + (int32_t)in[i * 2 + 1];
        int16_t m = clamp16(mono);
        out[s_count++] = m;
        energy += (uint64_t)((int32_t)m * m);
      }
      if (frames > 0) {
        uint32_t rms = (uint32_t)sqrt((double)energy / frames);
        if (rms > SILENCE_RMS) {
          s_speech = true;
          s_last_loud_ms = millis();
        }
      }
    }
  }

  uint32_t now = millis();
  bool max_reached = s_count >= REC_MAX_SAMPLES;
  bool silence_stop = s_speech && (now - s_start_ms > MIN_RECORD_MS) &&
                      (now - s_last_loud_ms > SILENCE_MS);
  if (max_reached || silence_stop) {
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

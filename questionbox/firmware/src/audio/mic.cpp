#include "mic.h"
#include "audio_bus.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

#define REC_RATE        16000
#define REC_MAX_SEC     15
#define REC_MAX_SAMPLES (REC_RATE * REC_MAX_SEC)
#define WAV_HEADER_LEN  44

static int32_t *s_cap = nullptr;     // captured mono, 32-bit (pre-normalization)
static uint8_t *s_buf = nullptr;     // output WAV: [44-byte header | mono int16 PCM]
static size_t   s_count = 0;
static bool     s_recording = false;

// Level metering.
static int32_t  s_peakL = 0, s_peakR = 0, s_peakMono = 0;

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
  s_cap = (int32_t *)heap_caps_malloc(REC_MAX_SAMPLES * sizeof(int32_t), MALLOC_CAP_SPIRAM);
  s_buf = (uint8_t *)heap_caps_malloc(WAV_HEADER_LEN + REC_MAX_SAMPLES * 2, MALLOC_CAP_SPIRAM);
  if (!s_cap || !s_buf) {
    Serial.println("[mic] FAILED to allocate PSRAM buffers");
    return false;
  }
  AudioBus_Init();
  Serial.println("[mic] buffer ready");
  return true;
}

void Mic_Start()
{
  AudioBus_MicFlush();
  s_count = 0;
  s_peakL = s_peakR = s_peakMono = 0;
  s_recording = true;
  Serial.println("[mic] recording started");
}

void Mic_Stop()
{
  if (!s_recording) return;
  s_recording = false;
  Serial.printf("[mic] stopped: %u samples %.2fs | peakL=%d peakR=%d (32-bit)\n",
                (unsigned)s_count, (float)s_count / REC_RATE, (int)s_peakL, (int)s_peakR);
}

bool Mic_Poll()
{
  if (!s_recording) return false;

  uint8_t tmp[2048];                       // 32-bit stereo frames = 8 bytes each
  size_t got = AudioBus_MicRead(tmp, sizeof(tmp));
  if (got >= 8) {
    int frames = got / 8;
    int32_t *in = (int32_t *)tmp;
    for (int i = 0; i < frames && s_count < REC_MAX_SAMPLES; i++) {
      int32_t L = in[i * 2];
      int32_t R = in[i * 2 + 1];
      int32_t aL = L < 0 ? -L : L;
      int32_t aR = R < 0 ? -R : R;
      if (aL > s_peakL) s_peakL = aL;
      if (aR > s_peakR) s_peakR = aR;
      int32_t mono = (L >> 1) + (R >> 1);    // combine channels without overflow
      int32_t am = mono < 0 ? -mono : mono;
      if (am > s_peakMono) s_peakMono = am;
      s_cap[s_count++] = mono;
    }
  }

  if (s_count >= REC_MAX_SAMPLES) {
    Mic_Stop();
    return false;
  }
  return true;
}

bool Mic_GetWav(const uint8_t **data, size_t *len)
{
  if (!s_buf || !s_cap || s_count == 0) return false;

  int16_t *pcm = (int16_t *)(s_buf + WAV_HEADER_LEN);

  if (s_peakMono <= 0) {
    Serial.println("[mic] SILENT - mic produced no signal (check mic hardware/pins)");
    memset(pcm, 0, s_count * 2);
  } else {
    // Normalize whatever level the mic gave us up to a healthy 16-bit target,
    // so transcription gets clear speech regardless of the mic's raw scale.
    const int32_t target = 22000;
    for (size_t i = 0; i < s_count; i++) {
      int64_t v = (int64_t)s_cap[i] * target / s_peakMono;
      pcm[i] = clamp16((int32_t)v);
    }
    Serial.printf("[mic] normalized (peakMono=%d -> %d)\n", (int)s_peakMono, (int)target);
  }

  uint32_t dataLen = s_count * 2;
  write_wav_header(s_buf, dataLen, REC_RATE);
  *data = s_buf;
  *len = WAV_HEADER_LEN + dataLen;
  return true;
}

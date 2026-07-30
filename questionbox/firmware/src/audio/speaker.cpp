#include "speaker.h"
#include "audio_bus.h"
#include <esp_heap_caps.h>
#include <math.h>

// Software gain applied before samples reach the DAC. 1.0 = original level.
static float s_gain = 0.6f;

// Largest answer we buffer whole into PSRAM before playing (gap-free). ~30s of
// 24 kHz/16-bit mono. Anything bigger falls back to direct streaming.
#define MAX_BUFFERED_BYTES (1500 * 1024)

void Speaker_SetGain(float g)
{
  if (g < 0.0f) g = 0.0f;
  if (g > 4.0f) g = 4.0f;
  s_gain = g;
}
float Speaker_GetGain() { return s_gain; }

void Speaker_SetVolumePercent(int pct)
{
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  // Perceptual (squared) curve so low settings get MUCH quieter, and a modest
  // ceiling so even max isn't harsh. 100% -> ~1.3x, 50% -> ~0.33x, 20% -> ~0.05x.
  float f = pct / 100.0f;
  s_gain = f * f * 1.3f;
}

static inline int16_t apply_gain(int16_t sample)
{
  if (s_gain == 1.0f) return sample;
  int32_t v = (int32_t)(sample * s_gain);
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return (int16_t)v;
}

bool Speaker_Begin()
{
  // The shared I2S bus is set up by AudioBus_Init() (via Mic_Begin). Nothing to
  // do here.
  return true;
}

// Read exactly `want` bytes from the stream (blocking with a bounded wait),
// pumping the UI while we wait. Returns bytes actually read.
static int read_exact(Stream &s, uint8_t *buf, int want, void (*pump)())
{
  int got = 0;
  uint32_t deadline = millis() + 4000;
  while (got < want && millis() < deadline) {
    int n = s.readBytes((char *)(buf + got), want - got);
    if (n > 0) {
      got += n;
      deadline = millis() + 4000;
    } else {
      if (pump) pump();
      delay(2);
    }
  }
  return got;
}

void Speaker_TestBeep()
{
  const int rate = 16000;            // whole audio path runs at 16 kHz now
  AudioBus_SetSampleRate(rate);
  const int total = rate / 2;        // 0.5 seconds
  int16_t buf[256 * 2];
  int done = 0;
  while (done < total) {
    int cnt = (total - done) < 256 ? (total - done) : 256;
    for (int i = 0; i < cnt; i++) {
      float t = (float)(done + i) / rate;
      float env = sinf(3.14159265f * (done + i) / total);   // gentle fade in/out
      // Quiet, friendly chime (~10% of the old level) just to confirm the path.
      float s = sinf(2.0f * 3.14159265f * 700.0f * t) * 0.06f * env;
      int16_t v = (int16_t)(s * 32767);
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    AudioBus_SpeakerWrite(buf, cnt * 2 * sizeof(int16_t));   // raw, ignores volume
    done += cnt;
  }
  AudioBus_SetSampleRate(16000);
  Serial.println("[spk] test beep played");
}

// ---- Dual-task playback (producer + consumer) ----
// A "producer" task downloads the answer into PSRAM as fast as the link allows,
// and a "consumer" task plays it to I2S. BOTH run on core 0, apart from the UI
// (Arduino loop, core 1), so neither the network nor LVGL rendering can starve
// the audio — that combination is what fixes the choppiness. Playback starts
// after a small pre-buffer, so it also begins quickly. The main thread only
// pumps the UI and waits.
static volatile bool s_playDone = true;   // consumer finished
static volatile bool s_dlDone   = false;  // producer finished filling
static volatile int  s_filled   = 0;      // bytes downloaded so far
static uint8_t      *s_buf      = nullptr;
static int           s_total    = 0;      // total content length (WAV header + PCM)
static Stream       *s_stream   = nullptr;
static void        (*s_onStart)() = nullptr;
static int16_t s_stereo[512 * 2];         // static so the task stacks stay small

// ~0.3s head start. The producer usually outruns playback, so this stays small
// (small pre-buffer = quicker start), but it still absorbs first-packet jitter.
#define PREBUF_BYTES (16 * 1024)

static void producer_task(void *)
{
  int got = 0;
  uint32_t stall = millis() + 6000;
  while (got < s_total) {
    int want = s_total - got;
    if (want > 4096) want = 4096;
    // Read only what's already arrived so we never block waiting for a full
    // chunk (that would let the consumer catch up and stutter).
    int av = s_stream->available();
    if (av <= 0) { vTaskDelay(1); if (millis() > stall) break; continue; }
    if (want > av) want = av;
    int n = s_stream->readBytes((char *)(s_buf + got), want);
    if (n > 0) { got += n; s_filled = got; stall = millis() + 6000; }
    else { vTaskDelay(1); if (millis() > stall) break; }
  }
  s_dlDone = true;
  vTaskDelete(nullptr);
}

static void consumer_task(void *)
{
  // The whole audio path is fixed at 16 kHz now, so we don't retune the bus off
  // the WAV header — we just skip the 44-byte header and play the PCM.
  // Small pre-buffer so a first-packet hiccup can't clip the opening word.
  int prebuf = 44 + PREBUF_BYTES;
  if (prebuf > s_total) prebuf = s_total;
  while (s_filled < prebuf && !s_dlDone) vTaskDelay(1);

  if (s_onStart) s_onStart();              // start the mouth as sound starts

  int pos = 44;                            // skip the WAV header
  for (;;) {
    int filled = s_filled;
    int avail = filled - pos;
    if (avail < 2) {
      if (s_dlDone && pos >= filled) break;
      vTaskDelay(1);
      continue;
    }
    int nbytes = avail;
    if (nbytes > (int)sizeof(s_stereo) / 2) nbytes = (int)sizeof(s_stereo) / 2;
    nbytes &= ~0x1;
    int nsamp = nbytes / 2;
    const int16_t *mono = (const int16_t *)(s_buf + pos);
    for (int k = 0; k < nsamp; k++) {
      int16_t v = apply_gain(mono[k]);
      s_stereo[k * 2] = v;
      s_stereo[k * 2 + 1] = v;
    }
    AudioBus_SpeakerWrite((uint8_t *)s_stereo, nsamp * 2 * sizeof(int16_t));
    pos += nbytes;
  }
  delay(120);                              // let the DMA tail drain
  s_playDone = true;
  vTaskDelete(nullptr);
}

// Direct streaming fallback (used only if buffering isn't possible). Prone to
// network-induced choppiness, so we prefer the buffered path above.
static void play_stream(Stream &s, int dataRemaining, void (*pump)())
{
  int16_t mono[512];
  int16_t stereo[512 * 2];
  while (dataRemaining > 0) {
    int want = (int)sizeof(mono);
    if (want > dataRemaining) want = dataRemaining;
    want &= ~0x1;
    if (want <= 0) break;
    int got = read_exact(s, (uint8_t *)mono, want, pump);
    if (got <= 0) break;
    dataRemaining -= got;
    int samples = got / 2;
    for (int i = 0; i < samples; i++) {
      int16_t v = apply_gain(mono[i]);
      stereo[i * 2] = v;
      stereo[i * 2 + 1] = v;
    }
    AudioBus_SpeakerWrite((uint8_t *)stereo, samples * 2 * sizeof(int16_t));
    if (pump) pump();
  }
}

void Speaker_PlayWavStream(Stream &s, int contentLen, void (*pump)(), void (*onAudioStart)())
{
  // Preferred path: download + play on two core-0 tasks (see above) while the UI
  // keeps rendering on core 1. Needs a known content length to size the buffer.
  if (contentLen > 44 && contentLen <= MAX_BUFFERED_BYTES) {
    uint8_t *buf = (uint8_t *)heap_caps_malloc(contentLen, MALLOC_CAP_SPIRAM);
    if (buf) {
      s_buf = buf;
      s_total = contentLen;
      s_stream = &s;
      s_filled = 0;
      s_dlDone = false;
      s_playDone = false;
      s_onStart = onAudioStart;
      // Consumer runs at a slightly higher priority so feeding I2S always wins.
      xTaskCreatePinnedToCore(producer_task, "spkdl",  4096, nullptr, 5, nullptr, 0);
      xTaskCreatePinnedToCore(consumer_task, "spkplay", 3072, nullptr, 6, nullptr, 0);
      while (!s_playDone) { if (pump) pump(); delay(5); }
      heap_caps_free(buf);
      s_buf = nullptr;
      Serial.printf("[spk] played %d bytes (dual-task)\n", s_filled);
      return;
    }
    Serial.println("[spk] PSRAM buffer alloc failed - streaming instead");
  }

  // Fallback: read the header then stream synchronously (unknown length, etc.).
  uint8_t header[44];
  if (read_exact(s, header, 44, pump) < 44) {
    Serial.println("[spk] short WAV header");
    return;
  }
  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    Serial.println("[spk] not a WAV stream");
    return;
  }
  int dataLen = (contentLen > 44) ? (contentLen - 44) : -1;
  if (onAudioStart) onAudioStart();
  play_stream(s, (dataLen > 0) ? dataLen : INT32_MAX, pump);
  Serial.println("[spk] playback done (streamed)");
}

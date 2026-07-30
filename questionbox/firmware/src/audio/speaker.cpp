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
  const int rate = 24000;
  AudioBus_SetSampleRate(rate);      // clock the speaker codec for playback
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

// Play mono 16-bit PCM already resident in RAM. Because the data is local, the
// I2S DMA never starves on the network -> smooth, gap-free playback. The UI is
// pumped between chunks (DMA holds enough cushion to cover a render).
static void play_pcm_ram(const uint8_t *data, int bytes, void (*pump)())
{
  const int CHUNK = 512;                 // mono samples per write
  int16_t stereo[CHUNK * 2];
  const int16_t *mono = (const int16_t *)data;
  int total = bytes / 2;
  int i = 0, sinceLastPump = 0;
  while (i < total) {
    int n = (total - i) < CHUNK ? (total - i) : CHUNK;
    for (int k = 0; k < n; k++) {
      int16_t v = apply_gain(mono[i + k]);
      stereo[k * 2] = v;
      stereo[k * 2 + 1] = v;
    }
    AudioBus_SpeakerWrite((uint8_t *)stereo, n * 2 * sizeof(int16_t));
    i += n;
    sinceLastPump += n;
    if (pump && sinceLastPump >= 1024) { pump(); sinceLastPump = 0; }
  }
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
  uint8_t header[44];
  if (read_exact(s, header, 44, pump) < 44) {
    Serial.println("[spk] short WAV header");
    return;
  }
  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    Serial.println("[spk] not a WAV stream");
    return;
  }

  uint32_t wavRate = *(uint32_t *)(header + 24);
  int dataLen = (contentLen > 44) ? (contentLen - 44) : -1;

  // Preferred path: pull the entire answer into PSRAM first (UI shows THINKING),
  // then play it back smoothly with no network in the audio loop.
  if (dataLen > 0 && dataLen <= MAX_BUFFERED_BYTES) {
    uint8_t *buf = (uint8_t *)heap_caps_malloc(dataLen, MALLOC_CAP_SPIRAM);
    if (buf) {
      int got = read_exact(s, buf, dataLen, pump);
      if (wavRate >= 8000 && wavRate <= 48000) AudioBus_SetSampleRate(wavRate);
      if (onAudioStart) onAudioStart();           // start the mouth when sound starts
      play_pcm_ram(buf, got, pump);
      heap_caps_free(buf);
      delay(120);                                  // let the DMA tail drain
      AudioBus_SetSampleRate(16000);               // hand the bus back to the mic
      Serial.printf("[spk] played %d bytes (buffered)\n", got);
      return;
    }
    Serial.println("[spk] PSRAM buffer alloc failed - streaming instead");
  }

  // Fallback: stream directly.
  if (wavRate >= 8000 && wavRate <= 48000) AudioBus_SetSampleRate(wavRate);
  if (onAudioStart) onAudioStart();
  play_stream(s, (dataLen > 0) ? dataLen : INT32_MAX, pump);
  AudioBus_SetSampleRate(16000);
  Serial.println("[spk] playback done (streamed)");
}

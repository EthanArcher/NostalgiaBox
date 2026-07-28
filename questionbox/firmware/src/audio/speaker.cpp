#include "speaker.h"
#include "audio_bus.h"

// Software gain applied before the samples reach the DAC. 1.0 = original level.
// A gentle boost helps if the answer sounds quiet even with the amp turned up.
static float s_gain = 1.6f;

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
  // 0% -> silent, ~65% -> unity, 100% -> ~2.6x boost.
  s_gain = (pct / 100.0f) * 2.6f;
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
  // The I2S bus is enabled on demand in Speaker_PlayWavStream() (shared with the
  // mic). Nothing to do at boot.
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
  const int total = rate / 2;        // 0.5 seconds
  int16_t buf[256 * 2];
  int done = 0;
  while (done < total) {
    int cnt = (total - done) < 256 ? (total - done) : 256;
    for (int i = 0; i < cnt; i++) {
      float t = (float)(done + i) / rate;
      float env = sinf(3.14159265f * (done + i) / total);   // gentle fade in/out
      float s = sinf(2.0f * 3.14159265f * 700.0f * t) * 0.6f * env;   // louder for audibility
      int16_t v = (int16_t)(s * 32767);
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    AudioBus_SpeakerWrite(buf, cnt * 2 * sizeof(int16_t));   // raw, ignores volume
    done += cnt;
  }
  Serial.println("[spk] test beep played");
}

void Speaker_PlayWavStream(Stream &s, int contentLen, void (*pump)())
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

  int dataRemaining = (contentLen > 44) ? (contentLen - 44) : INT32_MAX;

  int16_t mono[512];
  int16_t stereo[512 * 2];

  while (dataRemaining > 0) {
    int want = (int)sizeof(mono);
    if (want > dataRemaining) want = dataRemaining;
    want &= ~0x1;                       // whole 16-bit samples
    if (want <= 0) break;

    int got = read_exact(s, (uint8_t *)mono, want, pump);
    if (got <= 0) break;
    dataRemaining -= got;

    int samples = got / 2;
    for (int i = 0; i < samples; i++) {
      int16_t s = apply_gain(mono[i]);
      stereo[i * 2] = s;
      stereo[i * 2 + 1] = s;
    }
    AudioBus_SpeakerWrite((uint8_t *)stereo, samples * 2 * sizeof(int16_t));

    if (pump) pump();
  }
  Serial.println("[spk] playback done");
}

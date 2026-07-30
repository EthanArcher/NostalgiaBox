#pragma once
#include <Arduino.h>

// Plays answer audio out of the ES8311 speaker codec over the shared I2S bus.
//
// Answer audio is standardized as 16-bit PCM WAV, MONO, 24 kHz (what the server
// produces). The 44-byte WAV header is read to pick the playback rate; mono
// samples are duplicated to both stereo channels for the codec.

bool Speaker_Begin();

// Plays a WAV response body on the speaker. The whole answer is pulled into
// PSRAM first, then played gap-free (no Wi-Fi in the audio loop = smooth). `pump`
// keeps the UI running during download + playback. `onAudioStart` (optional) is
// called the moment audible playback begins - use it to start the mouth so the
// animation lines up with the sound. Audio is transient and never stored.
void Speaker_PlayWavStream(Stream &s, int contentLen, void (*pump)(),
                           void (*onAudioStart)() = nullptr);

// Software loudness. gain 1.0 = as-is; >1.0 boosts (clamped). Range ~0.0..4.0.
void  Speaker_SetGain(float gain);
float Speaker_GetGain();

// Volume as a friendly 0..100 percent (mapped to a safe gain range).
void Speaker_SetVolumePercent(int pct);

// Plays a short tone at boot to verify the speaker/amplifier path works
// (independent of the network). If you hear it, the speaker is wired and on.
void Speaker_TestBeep();

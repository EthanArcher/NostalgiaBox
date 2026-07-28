#pragma once
#include <ESP_I2S.h>

// A SINGLE shared I2S peripheral for both the microphone (record) and the
// PCM5101 speaker (playback). They use different pins, so we (re)configure and
// enable the one bus only for the operation we're doing right now, and disable
// it afterwards. This avoids two drivers fighting over the hardware (which was
// leaving the mic channel disabled -> 0 samples recorded).
//
// WonderBox is push-to-talk / half-duplex, so we never record and play at once.

extern I2SClass wb_i2s;

// Mic pins (Waveshare wiki): BCK 15, WS 2, DIN 39.
bool AudioBus_BeginMic();      // enable RX @ 16 kHz stereo

// Speaker pins (Waveshare wiki): BCK 48, WS 38, DOUT 47.
bool AudioBus_BeginSpeaker();  // enable TX @ 24 kHz stereo

void AudioBus_End();           // disable/free the bus

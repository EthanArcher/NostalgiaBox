#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Microphone and speaker each get their OWN dedicated I2S controller, set up
// once at boot and kept enabled. This avoids (a) two drivers fighting over one
// controller (which disabled the mic) and (b) opening/closing channels
// repeatedly (which leaked channels until "no available channel found").
//
//   mic     -> I2S_NUM_0, RX, 16 kHz stereo   (BCK 15, WS 2, DIN 39)
//   speaker -> I2S_NUM_1, TX, 24 kHz stereo   (BCK 48, WS 38, DOUT 47)

bool   AudioBus_Init(void);                          // create + enable both, once
size_t AudioBus_MicRead(void *buf, size_t len);      // returns bytes read
size_t AudioBus_SpeakerWrite(const void *buf, size_t len);
void   AudioBus_MicFlush(void);                      // drop stale mic samples

#ifdef __cplusplus
}
#endif

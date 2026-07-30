#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// V2 board: the ES7210 mic and ES8311 speaker share ONE full-duplex I2S bus,
// configured once at boot (see audio_bus.c). Audio flows 16-bit, stereo-framed.
//
//   shared bus -> I2S_NUM_0, RX+TX   (MCLK 2, BCK 48, WS 38, DOUT 47, DIN 39)
//
// Record runs at 16 kHz; playback switches the bus to 24 kHz. Both directions
// are 16-bit stereo frames. AudioBus_Init() also programs the codecs over I2C.

bool   AudioBus_Init(void);                          // codecs + shared bus, once
void   AudioBus_SetSampleRate(uint32_t rate);        // switch mic(16k)<->spk(24k)
size_t AudioBus_MicRead(void *buf, size_t len);      // returns bytes read
size_t AudioBus_SpeakerWrite(const void *buf, size_t len);
void   AudioBus_MicFlush(void);                      // drop stale mic samples

#ifdef __cplusplus
}
#endif

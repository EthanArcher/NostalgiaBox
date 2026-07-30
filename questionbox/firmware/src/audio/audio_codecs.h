#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configures the V2 board's audio codecs over I2C (Arduino Wire, which the touch
// controller already brought up). Must be called AFTER I2C_Init() and BEFORE the
// I2S bus is installed, matching Waveshare's example order.
//
//   ES7210 (0x40) -> the microphone ADC   (configured for 16 kHz capture)
//   ES8311 (0x18) -> the speaker DAC       (configured for 24 kHz playback)
//   GPIO15        -> speaker amplifier enable (driven HIGH)
//
// Returns true if both codecs acknowledged their configuration.
bool AudioCodecs_Init(void);

#ifdef __cplusplus
}
#endif

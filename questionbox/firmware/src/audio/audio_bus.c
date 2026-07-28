#include "audio_bus.h"
#include <stdio.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Mic (I2S MEMS)
#define MIC_BCK  15
#define MIC_WS   2
#define MIC_DIN  39
// Speaker (PCM5101 DAC)
#define SPK_BCK  48
#define SPK_WS   38
#define SPK_DOUT 47

static i2s_chan_handle_t s_mic = NULL;
static i2s_chan_handle_t s_spk = NULL;
static bool s_inited = false;

static bool init_mic(void)
{
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&cc, NULL, &s_mic) != ESP_OK) {
    printf("[audio] mic new_channel failed\n");
    return false;
  }
  i2s_std_config_t std = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = MIC_BCK,
      .ws = MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = MIC_DIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(s_mic, &std) != ESP_OK) {
    printf("[audio] mic init_std failed\n");
    return false;
  }
  i2s_channel_enable(s_mic);
  return true;
}

static bool init_spk(void)
{
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&cc, &s_spk, NULL) != ESP_OK) {
    printf("[audio] speaker new_channel failed\n");
    return false;
  }
  i2s_std_config_t std = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = SPK_BCK,
      .ws = SPK_WS,
      .dout = SPK_DOUT,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(s_spk, &std) != ESP_OK) {
    printf("[audio] speaker init_std failed\n");
    return false;
  }
  i2s_channel_enable(s_spk);
  return true;
}

bool AudioBus_Init(void)
{
  if (s_inited) return true;
  bool ok = init_mic() && init_spk();
  s_inited = ok;
  if (ok) printf("[audio] mic + speaker ready\n");
  return ok;
}

size_t AudioBus_MicRead(void *buf, size_t len)
{
  if (!s_mic) return 0;
  size_t got = 0;
  i2s_channel_read(s_mic, buf, len, &got, pdMS_TO_TICKS(100));
  return got;
}

size_t AudioBus_SpeakerWrite(const void *buf, size_t len)
{
  if (!s_spk) return 0;
  size_t wrote = 0;
  i2s_channel_write(s_spk, buf, len, &wrote, pdMS_TO_TICKS(400));
  return wrote;
}

void AudioBus_MicFlush(void)
{
  if (!s_mic) return;
  uint8_t tmp[1024];
  size_t got = 0;
  for (int i = 0; i < 8; i++) {
    if (i2s_channel_read(s_mic, tmp, sizeof(tmp), &got, 0) != ESP_OK || got == 0) break;
  }
}

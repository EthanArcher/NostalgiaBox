#include "audio_bus.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// Legacy I2S driver: lets us pick a dedicated controller per device AND avoids
// the new driver's PSRAM/GDMA "user context not in internal RAM" failure.
#include "driver/i2s.h"

// Mic (I2S MEMS)  -> I2S_NUM_0 (RX)
#define MIC_PORT I2S_NUM_0
#define MIC_BCK  15
#define MIC_WS   2
#define MIC_DIN  39
// Speaker (PCM5101 DAC) -> I2S_NUM_1 (TX)
#define SPK_PORT I2S_NUM_1
#define SPK_BCK  48
#define SPK_WS   38
#define SPK_DOUT 47

static bool s_inited = false;

static bool init_mic(void)
{
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    // MEMS mic outputs 24-bit data; read 32-bit slots and scale down in mic.cpp.
    // (Reading 16-bit grabbed the empty low bits -> pure silence.)
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,   // stereo frames (L,R)
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };
  if (i2s_driver_install(MIC_PORT, &cfg, 0, NULL) != ESP_OK) {
    printf("[audio] mic driver_install failed\n");
    return false;
  }
  i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = MIC_BCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_DIN,
  };
  if (i2s_set_pin(MIC_PORT, &pins) != ESP_OK) {
    printf("[audio] mic set_pin failed\n");
    return false;
  }
  return true;
}

static bool init_spk(void)
{
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 24000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
  };
  if (i2s_driver_install(SPK_PORT, &cfg, 0, NULL) != ESP_OK) {
    printf("[audio] speaker driver_install failed\n");
    return false;
  }
  i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = SPK_BCK,
    .ws_io_num = SPK_WS,
    .data_out_num = SPK_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE,
  };
  if (i2s_set_pin(SPK_PORT, &pins) != ESP_OK) {
    printf("[audio] speaker set_pin failed\n");
    return false;
  }
  i2s_zero_dma_buffer(SPK_PORT);
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
  if (!s_inited) return 0;
  size_t got = 0;
  i2s_read(MIC_PORT, buf, len, &got, pdMS_TO_TICKS(100));
  return got;
}

size_t AudioBus_SpeakerWrite(const void *buf, size_t len)
{
  if (!s_inited) return 0;
  size_t wrote = 0;
  i2s_write(SPK_PORT, buf, len, &wrote, pdMS_TO_TICKS(400));
  return wrote;
}

void AudioBus_MicFlush(void)
{
  if (!s_inited) return;
  uint8_t tmp[1024];
  size_t got = 0;
  for (int i = 0; i < 8; i++) {
    if (i2s_read(MIC_PORT, tmp, sizeof(tmp), &got, 0) != ESP_OK || got == 0) break;
  }
}

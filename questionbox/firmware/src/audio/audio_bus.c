#include "audio_bus.h"
#include "audio_codecs.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// Legacy I2S driver: unlike the newer ESP_I2S driver, it coexists with PSRAM
// (the new driver's DMA object lands in PSRAM and GDMA rejects it -> "channel
// not enabled"). The main firmware needs PSRAM for the record buffer + LVGL, so
// we drive the shared V2 audio bus with the legacy driver.
#include "driver/i2s.h"

// V2 board: the mic (ES7210) and speaker (ES8311) share ONE I2S bus.
//   MCLK 2, BCK 48, WS 38, DOUT 47 (to speaker), DIN 39 (from mic)
// The bus runs full-duplex on a single controller so both directions share the
// same clocks. Record uses 16 kHz; playback switches to 24 kHz (MCLK follows at
// 256x, matching whichever codec is active).
#define AUDIO_PORT I2S_NUM_0
#define PIN_MCLK   2
#define PIN_BCK    48
#define PIN_WS     38
#define PIN_DOUT   47
#define PIN_DIN    39

#define MIC_RATE   16000

static bool     s_inited = false;
static uint32_t s_rate = MIC_RATE;

static bool init_i2s(void)
{
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
    .sample_rate = MIC_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,   // stereo frames both ways
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = true,                               // accurate MCLK for the codecs
    .tx_desc_auto_clear = true,                     // output silence on underrun
    .fixed_mclk = 0,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
  };
  if (i2s_driver_install(AUDIO_PORT, &cfg, 0, NULL) != ESP_OK) {
    printf("[audio] i2s driver_install failed\n");
    return false;
  }
  i2s_pin_config_t pins = {
    .mck_io_num = PIN_MCLK,
    .bck_io_num = PIN_BCK,
    .ws_io_num = PIN_WS,
    .data_out_num = PIN_DOUT,
    .data_in_num = PIN_DIN,
  };
  if (i2s_set_pin(AUDIO_PORT, &pins) != ESP_OK) {
    printf("[audio] i2s set_pin failed\n");
    return false;
  }
  i2s_zero_dma_buffer(AUDIO_PORT);
  return true;
}

bool AudioBus_Init(void)
{
  if (s_inited) return true;
  // Codecs must be programmed over I2C before the bus starts clocking.
  bool codecs_ok = AudioCodecs_Init();
  bool i2s_ok = init_i2s();
  s_inited = i2s_ok;               // audio can still limp along if a codec NAKs
  if (i2s_ok) printf("[audio] shared I2S bus ready (%s codecs)\n",
                     codecs_ok ? "with" : "WITHOUT");
  return i2s_ok;
}

void AudioBus_SetSampleRate(uint32_t rate)
{
  if (!s_inited || rate == 0 || rate == s_rate) return;
  // 16-bit, stereo framing; MCLK follows at the configured 256x multiple.
  if (i2s_set_clk(AUDIO_PORT, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) == ESP_OK) {
    s_rate = rate;
  } else {
    printf("[audio] set_clk %u failed\n", (unsigned)rate);
  }
}

size_t AudioBus_MicRead(void *buf, size_t len)
{
  if (!s_inited) return 0;
  size_t got = 0;
  // Short wait: the main loop calls this every iteration, and a long block here
  // would make the "tap again to stop" feel laggy. 20 ms keeps taps snappy.
  i2s_read(AUDIO_PORT, buf, len, &got, pdMS_TO_TICKS(20));
  return got;
}

size_t AudioBus_SpeakerWrite(const void *buf, size_t len)
{
  if (!s_inited) return 0;
  size_t wrote = 0;
  i2s_write(AUDIO_PORT, buf, len, &wrote, pdMS_TO_TICKS(400));
  return wrote;
}

void AudioBus_MicFlush(void)
{
  if (!s_inited) return;
  uint8_t tmp[1024];
  size_t got = 0;
  for (int i = 0; i < 8; i++) {
    if (i2s_read(AUDIO_PORT, tmp, sizeof(tmp), &got, 0) != ESP_OK || got == 0) break;
  }
}

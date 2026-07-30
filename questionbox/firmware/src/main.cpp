/*****************************************************************************
 * WonderBox firmware - main loop.
 *
 * Tap the FACE to talk. The device records the question, sends it to the server
 * (speech -> safety -> answer -> speech), and plays the answer back. The HTTPS
 * request runs on a background task so the THINKING animation keeps moving.
 *
 * Extras:
 *  - Smiling face by default; big grin + ripples while listening.
 *  - Auto-sleep: the screen goes dark after 30s idle; tap to wake.
 *  - Software loudness boost (physical side buttons control the amplifier).
 *
 * PUSH-TO-TALK ONLY. Audio is recorded, sent, and discarded - never stored.
 *****************************************************************************/
#include <Arduino.h>

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST77916.h"
#include "LVGL_Driver.h"

#include "wonderbox_state.h"
#include "face.h"

#include "wifi_conn.h"
#include "wonder_client.h"
#include "mic.h"
#include "speaker.h"

// ---- Background request handoff ----
enum ReqStatus { REQ_IDLE, REQ_RUNNING, REQ_DONE, REQ_FAILED };
static volatile ReqStatus g_req = REQ_IDLE;
static AnswerInfo    g_ans;
static const uint8_t *g_wav = nullptr;
static size_t        g_wav_len = 0;

// ---- Auto-sleep ----
#define SCREEN_BRIGHTNESS 80
#define SLEEP_AFTER_MS    30000
static uint32_t g_last_activity = 0;
static bool     g_asleep = false;

// ---- Volume (software) ----
static int      g_volume = 55;           // 0..100
static uint32_t g_last_gesture_ms = 0;

// ---- BOOT button (GPIO0) as a volume control ----
// It's the only side button the chip can read (the other is hardware RESET, and
// the slide switch is a hardware battery power switch). Short press = volume up,
// long press = volume down.
#define BTN_BOOT       0
static bool     g_btn_down = false;
static uint32_t g_btn_down_ms = 0;
static bool     g_btn_long_fired = false;

// Set by the audio (consumer) task to ask the main loop to switch the face to
// SPEAKING. LVGL is single-threaded, so the audio task must NEVER call Face_*
// itself — it only raises this flag, and pump_ui() (main loop) acts on it.
static volatile bool g_want_speaking = false;

static void note_activity() { g_last_activity = millis(); }

static void request_speaking() { g_want_speaking = true; }

static void pump_ui()
{
  if (g_want_speaking) { g_want_speaking = false; Face_SetState(WB_SPEAKING); }
  Lvgl_Loop();
  Face_Tick();
}

static void go_idle()
{
  Face_SetState(WB_IDLE);
  note_activity();
}

static void sleep_now()
{
  g_asleep = true;
  Set_Backlight(0);
  Serial.println("[app] sleeping (screen off)");
}

static void wake_up()
{
  g_asleep = false;
  Set_Backlight(SCREEN_BRIGHTNESS);
  Face_SetState(WB_IDLE);
  note_activity();
  Serial.println("[app] awake");
}

static void ask_task(void *)
{
  AnswerInfo ans;
  int code = WonderClient_Ask(g_wav, g_wav_len, ans);
  g_ans = ans;
  g_req = (code == 200) ? REQ_DONE : REQ_FAILED;
  vTaskDelete(nullptr);
}

static void stop_and_send()
{
  Mic_Stop();
  if (!Mic_GetWav(&g_wav, &g_wav_len)) {
    Serial.println("[app] nothing recorded");
    go_idle();
    return;
  }
  Face_SetState(WB_THINKING);
  g_req = REQ_RUNNING;
  xTaskCreatePinnedToCore(ask_task, "ask", 16384, nullptr, 4, nullptr, 0);
}

static void play_answer()
{
  Stream *body = WonderClient_GetStream();
  int len = WonderClient_GetLength();
  g_want_speaking = false;
  if (g_ans.isSpelling && g_ans.spellWord.length() > 0) {
    Face_ShowSpelling(g_ans.spellWord.c_str());        // letters stay up during audio
    if (body) Speaker_PlayWavStream(*body, len, pump_ui, nullptr);
  } else {
    // Stay THINKING while the answer downloads; the mouth starts talking the
    // instant audible playback begins (request_speaking flips a flag the main
    // loop reads, so LVGL is only ever touched from the main thread).
    if (body) Speaker_PlayWavStream(*body, len, pump_ui, request_speaking);
    else Face_SetState(WB_SPEAKING);
  }
  WonderClient_End();
  go_idle();
}

// Tap anywhere on the face.
static void handle_tap()
{
  switch (Face_GetState()) {
    case WB_IDLE:
      if (!WiFiConn_IsConnected()) {
        Serial.println("[app] no Wi-Fi - trying to reconnect");
        WiFiConn_Connect(8000);
      }
      Mic_Start();
      Face_SetState(WB_LISTENING);
      break;
    case WB_LISTENING:
      stop_and_send();
      break;
    default:
      break; // busy (thinking/speaking/spelling) - ignore taps
  }
}

static void tap_event_cb(lv_event_t *e)
{
  (void)e;
  note_activity();
  if (g_asleep) {            // first tap just wakes the screen
    wake_up();
    return;
  }
  if (millis() - g_last_gesture_ms < 300) return;  // ignore the click that ends a swipe
  handle_tap();
}

static void change_volume(int delta)
{
  g_volume += delta;
  if (g_volume < 0) g_volume = 0;
  if (g_volume > 100) g_volume = 100;
  Speaker_SetVolumePercent(g_volume);
  Face_ShowVolume(g_volume);
  note_activity();
  Serial.printf("[app] volume %d%%\n", g_volume);
}

// Poll the BOOT button: short press = volume up, long press = volume down.
static void poll_boot_button()
{
  bool down = digitalRead(BTN_BOOT) == LOW;
  uint32_t now = millis();
  if (down && !g_btn_down) {
    g_btn_down = true;
    g_btn_down_ms = now;
    g_btn_long_fired = false;
    if (g_asleep) wake_up();
  } else if (down && g_btn_down) {
    if (!g_btn_long_fired && (now - g_btn_down_ms) > 700) {
      g_btn_long_fired = true;
      change_volume(-10);            // long press -> down
    }
  } else if (!down && g_btn_down) {
    g_btn_down = false;
    if (!g_btn_long_fired && (now - g_btn_down_ms) > 40) {
      change_volume(+10);            // short press -> up
    }
  }
}

// Swipe up/down anywhere = volume up/down, with a curved volume bar.
static void gesture_event_cb(lv_event_t *e)
{
  (void)e;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);

  note_activity();
  if (g_asleep) { wake_up(); return; }
  if (Face_GetState() == WB_LISTENING) return;   // don't fight an active recording

  if (dir == LV_DIR_TOP) change_volume(+10);
  else if (dir == LV_DIR_BOTTOM) change_volume(-10);
  else return;

  g_last_gesture_ms = millis();
}

static void Board_Init()
{
  I2C_Init();
  TCA9554PWR_Init(0x00);
  Backlight_Init();
  LCD_Init();
  Set_Backlight(SCREEN_BRIGHTNESS);
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== WonderBox firmware ===");

  Board_Init();
  Lvgl_Init();

  Face_Create();

  // A transparent, full-screen button on top of the face captures taps anywhere.
  lv_obj_t *touch = lv_btn_create(lv_scr_act());
  lv_obj_remove_style_all(touch);
  lv_obj_set_size(touch, 360, 360);
  lv_obj_center(touch);
  lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(touch, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(touch, tap_event_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(touch, gesture_event_cb, LV_EVENT_GESTURE, nullptr);

  Face_SetState(WB_IDLE);

  pinMode(BTN_BOOT, INPUT_PULLUP);   // BOOT button used for volume

  Mic_Begin();
  Speaker_Begin();
  Speaker_SetVolumePercent(g_volume);
  Speaker_TestBeep();   // if you hear a short beep, the speaker path works

  WiFiConn_Connect();

  note_activity();
  Serial.println("=== setup complete ===");
}

void loop()
{
  Lvgl_Loop();
  Face_Tick();
  poll_boot_button();

  // While listening, keep capturing and auto-stop on silence.
  if (Face_GetState() == WB_LISTENING) {
    if (!Mic_Poll()) stop_and_send();
  }

  // Handle the background request result.
  if (g_req == REQ_DONE) {
    g_req = REQ_IDLE;
    play_answer();
  } else if (g_req == REQ_FAILED) {
    g_req = REQ_IDLE;
    Serial.println("[app] request failed");
    WonderClient_End();
    go_idle();
  }

  // Auto-sleep only when calm and idle.
  if (!g_asleep && g_req == REQ_IDLE && Face_GetState() == WB_IDLE &&
      (millis() - g_last_activity) > SLEEP_AFTER_MS) {
    sleep_now();
  }

  delay(3);
}

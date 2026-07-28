/*****************************************************************************
 * The WonderBox face.
 *
 * A warm, friendly, smiling face for the 360x360 round LCD. Everything is drawn
 * with plain LVGL objects (no image assets). One look per state:
 *
 *   IDLE      gentle blink, soft SMILE       -> ready and calm
 *   LISTENING big grin + expanding ripples   -> "I'm listening"
 *   THINKING  eyes glance up, bouncing dots  -> a charming wait
 *   SPEAKING  mouth opens and closes         -> talking
 *   SPELLING  one big letter at a time        -> spelling a word
 *
 * The smile is a crescent: a dark mouth shape with a background-colored shape
 * nudged over the top of it, leaving a smiling curve. Only the small, moving
 * parts animate each frame (eyes, ripples, talking mouth), so it stays fluid.
 *****************************************************************************/
#include "face.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

// ---- Warm, friendly palette ----
#define COL_BG      0xFFF3E0
#define COL_INK     0x3A3F58
#define COL_CHEEK   0xFFB4A2
#define COL_ACCENT  0xFF7A59
#define COL_DOT     0x6FA8FF
#define COL_LETTER  0x2E7DE1

// ---- Base geometry (center 180,180) ----
static const int EYE_D    = 66;
static const int EYE_DX   = 62;
static const int EYE_DY   = -34;
static const int MOUTH_DY = 46;

// ---- Objects ----
static lv_obj_t *eye_l, *eye_r;
static lv_obj_t *cheek_l, *cheek_r;
static lv_obj_t *mouth;       // dark mouth shape (smile base, and the talking mouth)
static lv_obj_t *smile_mask;  // background-colored shape that carves the smile curve
static lv_obj_t *ripples[3];  // expanding "listening" waves
static lv_obj_t *dots[3];     // thinking
static lv_obj_t *letter_lbl, *word_lbl;

static WbState current = WB_IDLE;
static uint32_t blink_next_ms = 0;
static uint32_t blink_start_ms = 0;
static const uint32_t BLINK_MS = 150;

static char spell_word[32] = {0};
static int  spell_len = 0;
static int  spell_index = 0;
static uint32_t spell_last_ms = 0;
static const uint32_t SPELL_STEP_MS = 800;

static lv_obj_t *make_blob(lv_obj_t *parent, uint32_t color, lv_opa_t opa)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  return o;
}

static inline void show(lv_obj_t *o, bool v)
{
  if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void schedule_blink(uint32_t now)
{
  blink_next_ms = now + 2400 + (esp_random() % 2600);
}

// Configure the crescent smile. `grin` = how big/open the smile is (pixels the
// mask is lifted above the mouth). Bigger grin = bigger smile.
static void set_smile(int width, int grin)
{
  lv_obj_set_size(mouth, width, 66);
  lv_obj_align(mouth, LV_ALIGN_CENTER, 0, MOUTH_DY);
  lv_obj_set_size(smile_mask, width + 24, 66);
  lv_obj_align(smile_mask, LV_ALIGN_CENTER, 0, MOUTH_DY - grin);
}

void Face_Create(void)
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Ripples first (behind everything).
  for (int i = 0; i < 3; i++) {
    ripples[i] = lv_obj_create(scr);
    lv_obj_remove_style_all(ripples[i]);
    lv_obj_clear_flag(ripples[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ripples[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(ripples[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ripples[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ripples[i], lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(ripples[i], 6, 0);
    lv_obj_set_style_border_opa(ripples[i], LV_OPA_TRANSP, 0);
  }

  cheek_l = make_blob(scr, COL_CHEEK, LV_OPA_50);
  cheek_r = make_blob(scr, COL_CHEEK, LV_OPA_50);
  lv_obj_set_size(cheek_l, 34, 34);
  lv_obj_set_size(cheek_r, 34, 34);
  lv_obj_align(cheek_l, LV_ALIGN_CENTER, -96, 12);
  lv_obj_align(cheek_r, LV_ALIGN_CENTER, 96, 12);

  eye_l = make_blob(scr, COL_INK, LV_OPA_COVER);
  eye_r = make_blob(scr, COL_INK, LV_OPA_COVER);
  lv_obj_set_size(eye_l, EYE_D, EYE_D);
  lv_obj_set_size(eye_r, EYE_D, EYE_D);
  lv_obj_align(eye_l, LV_ALIGN_CENTER, -EYE_DX, EYE_DY);
  lv_obj_align(eye_r, LV_ALIGN_CENTER, EYE_DX, EYE_DY);

  mouth = make_blob(scr, COL_INK, LV_OPA_COVER);
  smile_mask = make_blob(scr, COL_BG, LV_OPA_COVER); // same as background -> carves a smile

  for (int i = 0; i < 3; i++) {
    dots[i] = make_blob(scr, COL_DOT, LV_OPA_COVER);
    lv_obj_set_size(dots[i], 22, 22);
    lv_obj_align(dots[i], LV_ALIGN_CENTER, (i - 1) * 34, 40);
  }

  letter_lbl = lv_label_create(scr);
  lv_obj_set_style_text_color(letter_lbl, lv_color_hex(COL_LETTER), 0);
  lv_obj_set_style_text_font(letter_lbl, &lv_font_montserrat_48, 0);
  lv_label_set_text(letter_lbl, "");
  lv_obj_align(letter_lbl, LV_ALIGN_CENTER, 0, -18);

  word_lbl = lv_label_create(scr);
  lv_obj_set_style_text_color(word_lbl, lv_color_hex(COL_INK), 0);
  lv_obj_set_style_text_font(word_lbl, &lv_font_montserrat_28, 0);
  lv_label_set_text(word_lbl, "");
  lv_obj_align(word_lbl, LV_ALIGN_CENTER, 0, 70);

  schedule_blink(lv_tick_get());
  Face_SetState(WB_IDLE);
}

WbState Face_GetState(void) { return current; }

void Face_SetState(WbState state)
{
  current = state;
  Serial.printf("[face] state -> %s\n", wb_state_name(state));

  const bool is_face  = (state != WB_SPELLING);
  const bool is_think = (state == WB_THINKING);
  const bool is_spk   = (state == WB_SPEAKING);
  const bool is_listen = (state == WB_LISTENING);
  const bool is_spell = (state == WB_SPELLING);

  show(eye_l, is_face);
  show(eye_r, is_face);
  show(cheek_l, is_face);
  show(cheek_r, is_face);
  // Mouth shows for idle/listening (as a smile) and speaking (as an open mouth).
  show(mouth, is_face && !is_think);
  // The mask is only shown to carve the smile (idle/listening), not while talking.
  show(smile_mask, (state == WB_IDLE || is_listen));
  for (int i = 0; i < 3; i++) show(dots[i], is_think);
  for (int i = 0; i < 3; i++) show(ripples[i], is_listen);
  show(letter_lbl, is_spell);
  show(word_lbl, is_spell);

  // Static smile geometry (set once per state so it isn't redrawn every frame).
  if (state == WB_IDLE) set_smile(110, 30);       // soft, friendly smile
  else if (is_listen)   set_smile(124, 46);       // big, happy grin
  else if (is_spk) {                              // speaking: reset mouth position
    lv_obj_set_size(mouth, 84, 26);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, MOUTH_DY);
  }

  uint32_t now = lv_tick_get();
  blink_start_ms = 0;
  schedule_blink(now);
  spell_last_ms = now;
}

void Face_ShowSpelling(const char *word)
{
  strncpy(spell_word, word ? word : "", sizeof(spell_word) - 1);
  spell_word[sizeof(spell_word) - 1] = '\0';
  spell_len = strlen(spell_word);
  spell_index = 0;
  lv_label_set_text(word_lbl, spell_word);
  Face_SetState(WB_SPELLING);
  if (spell_len > 0) {
    char c[2] = { (char)toupper((unsigned char)spell_word[0]), '\0' };
    lv_label_set_text(letter_lbl, c);
  } else {
    lv_label_set_text(letter_lbl, "");
  }
  spell_last_ms = lv_tick_get();
}

static void set_eye_open(float open)
{
  int h = (int)(6 + (EYE_D - 6) * open);
  if (h < 6) h = 6;
  lv_obj_set_height(eye_l, h);
  lv_obj_set_height(eye_r, h);
}

static void set_eye_look(int dy)
{
  lv_obj_align(eye_l, LV_ALIGN_CENTER, -EYE_DX, EYE_DY + dy);
  lv_obj_align(eye_r, LV_ALIGN_CENTER, EYE_DX, EYE_DY + dy);
}

static float blink_factor(uint32_t now)
{
  if (blink_start_ms == 0 && now >= blink_next_ms) blink_start_ms = now;
  if (blink_start_ms != 0) {
    uint32_t t = now - blink_start_ms;
    if (t >= BLINK_MS) {
      blink_start_ms = 0;
      schedule_blink(now);
      return 1.0f;
    }
    float p = (float)t / BLINK_MS;
    float closed = 1.0f - fabsf(1.0f - 2.0f * p);
    return 1.0f - closed;
  }
  return 1.0f;
}

void Face_Tick(void)
{
  uint32_t now = lv_tick_get();
  float t = now / 1000.0f;

  switch (current) {
    case WB_IDLE:
      set_eye_look(0);
      set_eye_open(blink_factor(now));
      break;

    case WB_LISTENING: {
      set_eye_look(0);
      set_eye_open(blink_factor(now));
      // Expanding ripples out to the edge = "I'm listening" waves.
      for (int i = 0; i < 3; i++) {
        float p = fmodf(t * 0.8f - i * 0.34f, 1.0f);
        if (p < 0) p += 1.0f;
        int d = 210 + (int)(180 * p);
        lv_obj_set_size(ripples[i], d, d);
        lv_obj_center(ripples[i]);
        lv_obj_set_style_border_opa(ripples[i], (lv_opa_t)((1.0f - p) * 170), 0);
      }
      break;
    }

    case WB_THINKING:
      set_eye_look(-10);
      set_eye_open(blink_factor(now));
      for (int i = 0; i < 3; i++) {
        float ph = t * 4.0f - i * 0.6f;
        float bob = sinf(ph);
        int dy = 40 - (int)(10 * fmaxf(0.0f, bob));
        lv_obj_align(dots[i], LV_ALIGN_CENTER, (i - 1) * 34, dy);
        lv_obj_set_style_bg_opa(dots[i], (lv_opa_t)(120 + 120 * (0.5f + 0.5f * bob)), 0);
      }
      break;

    case WB_SPEAKING: {
      set_eye_look(0);
      set_eye_open(blink_factor(now));
      float m = 0.5f + 0.5f * sinf(t * 11.0f);
      int h = 16 + (int)(42 * m);
      lv_obj_set_size(mouth, 84, h);
      lv_obj_align(mouth, LV_ALIGN_CENTER, 0, MOUTH_DY);
      break;
    }

    case WB_SPELLING:
      if (spell_len > 0 && now - spell_last_ms >= SPELL_STEP_MS) {
        spell_last_ms = now;
        spell_index = (spell_index + 1) % spell_len;
        char c[2] = { (char)toupper((unsigned char)spell_word[spell_index]), '\0' };
        lv_label_set_text(letter_lbl, c);
      }
      break;
  }
}

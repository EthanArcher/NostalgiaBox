/*****************************************************************************
 * The WonderBox face — a cute cartoon character on a blue background.
 *
 * Two looks, matching the reference art:
 *   SITTING STILL (idle/listening/thinking):
 *       two big white eyes with black pupils that wander + blink, and a small
 *       gentle smile.
 *   ANSWERING (speaking):
 *       happy closed "∩ ∩" eyes and an open black mouth with a pink tongue that
 *       opens and closes as it talks.
 *
 *   SPELLING shows one big letter at a time.
 *
 * A soft flowing edge glow appears only while listening / thinking (feedback
 * that it's busy); the idle and speaking faces are clean, like the reference.
 *****************************************************************************/
#include "face.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

// ---- Palette ----
#define COL_BG      0x4C90D9   // friendly blue
#define COL_INK     0x1E1E24   // near-black: pupils, eye arcs, smile, mouth
#define COL_EYE     0xFFFFFF   // eye white
#define COL_TONGUE  0xEE6F9E   // pink tongue
#define COL_LETTER  0xFFFFFF
// Cool, Siri-like flowing edge colors (listening/thinking only)
#define COL_GLOW1   0x9BE8FF
#define COL_GLOW2   0xBFD4FF
#define COL_GLOW3   0xE7C7FF
#define COL_VOLTRK  0x3C79B8
#define COL_VOLFILL 0xFFFFFF

// ---- Geometry (screen 360x360, center 180,180) ----
static const int EYE_D    = 118;   // big eyes
static const int EYE_DX   = 52;    // horizontal offset (eyes overlap a little)
static const int EYE_DY   = -34;
static const int PUPIL_D  = 48;
static const int MOUTH_W  = 148;
static const int MOUTH_DY = 82;    // mouth center offset from screen center
static const int MOUTH_MIN = 38;
static const int MOUTH_MAX = 108;
static const int EDGE_D   = 352;

// ---- Objects ----
static lv_obj_t *eye_l, *eye_r;        // white eyes (still)
static lv_obj_t *pupil_l, *pupil_r;    // pupils (still)
static lv_obj_t *eyearc_l, *eyearc_r;  // happy closed eyes (speaking)
static lv_obj_t *smile;                // gentle smile (still)
static lv_obj_t *mouth, *tongue;       // open talking mouth (speaking)
static lv_obj_t *comets[3];            // edge glow (listening/thinking)
static lv_obj_t *vol_arc;
static lv_obj_t *letter_lbl, *word_lbl;

static uint32_t vol_hide_ms = 0;
static WbState current = WB_IDLE;

// blink
static uint32_t blink_next_ms = 0, blink_start_ms = 0;
static const uint32_t BLINK_MS = 150;

// gaze
static float gaze_cx = 0, gaze_cy = 0, gaze_tx = 0, gaze_ty = 0;
static uint32_t gaze_next_ms = 0;

// spelling
static char spell_word[32] = {0};
static int spell_len = 0, spell_index = 0;
static uint32_t spell_last_ms = 0;
static const uint32_t SPELL_STEP_MS = 800;

static lv_obj_t *make_rect(lv_obj_t *parent, uint32_t fill, bool circle)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(o, circle ? LV_RADIUS_CIRCLE : 0, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

// A stroked arc segment (used for the smile and the happy closed eyes).
static lv_obj_t *make_arc_seg(lv_obj_t *parent, uint32_t color, int size,
                              int width, int a0, int a1, bool rounded)
{
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_remove_style_all(a);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(a, size, size);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 0);
  lv_arc_set_bg_angles(a, a0, a1);
  lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, rounded, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
  return a;
}

static inline void show(lv_obj_t *o, bool v)
{
  if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void schedule_blink(uint32_t now) { blink_next_ms = now + 2400 + (esp_random() % 2600); }

// Shape the open mouth by `open` (0 = nearly closed, 1 = wide open), keeping the
// pink tongue filling the bottom.
static void set_mouth(float open)
{
  if (open < 0.10f) open = 0.10f;
  if (open > 1.0f) open = 1.0f;
  int mh = MOUTH_MIN + (int)((MOUTH_MAX - MOUTH_MIN) * open);
  int rad = mh / 2;
  if (rad > 46) rad = 46;

  lv_obj_set_size(mouth, MOUTH_W, mh);
  lv_obj_set_style_radius(mouth, rad, 0);
  lv_obj_align(mouth, LV_ALIGN_CENTER, 0, MOUTH_DY);

  int tgw = (int)(MOUTH_W * 0.62f);
  int tgh = (int)(mh * 0.5f);
  if (tgh > 46) tgh = 46;
  if (tgh < 12) tgh = 12;
  lv_obj_set_size(tongue, tgw, tgh);
  lv_obj_align(tongue, LV_ALIGN_CENTER, 0, MOUTH_DY + mh / 2 - tgh / 2 - 3);
}

void Face_Create(void)
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Edge glow (behind everything).
  const uint32_t glow_cols[3] = {COL_GLOW1, COL_GLOW2, COL_GLOW3};
  for (int i = 0; i < 3; i++) {
    comets[i] = make_arc_seg(scr, glow_cols[i], EDGE_D, 10, 0, 110, true);
  }

  // Talking mouth (speaking): black cavity + pink tongue (drawn on top).
  mouth  = make_rect(scr, COL_INK, false);
  tongue = make_rect(scr, COL_TONGUE, true);
  set_mouth(0.6f);

  // Gentle smile (still): a shallow upturned arc.
  smile = make_arc_seg(scr, COL_INK, 96, 9, 35, 145, true);
  lv_obj_align(smile, LV_ALIGN_CENTER, 0, MOUTH_DY - 40);

  // Happy closed eyes (speaking): two upward arcs "∩ ∩".
  eyearc_l = make_arc_seg(scr, COL_INK, 92, 12, 210, 330, true);
  eyearc_r = make_arc_seg(scr, COL_INK, 92, 12, 210, 330, true);
  lv_obj_align(eyearc_l, LV_ALIGN_CENTER, -EYE_DX, EYE_DY + 18);
  lv_obj_align(eyearc_r, LV_ALIGN_CENTER,  EYE_DX, EYE_DY + 18);

  // Big white eyes with pupils (still).
  eye_l = make_rect(scr, COL_EYE, true);
  eye_r = make_rect(scr, COL_EYE, true);
  lv_obj_set_size(eye_l, EYE_D, EYE_D);
  lv_obj_set_size(eye_r, EYE_D, EYE_D);
  pupil_l = make_rect(scr, COL_INK, true);
  pupil_r = make_rect(scr, COL_INK, true);
  lv_obj_set_size(pupil_l, PUPIL_D, PUPIL_D);
  lv_obj_set_size(pupil_r, PUPIL_D, PUPIL_D);

  letter_lbl = lv_label_create(scr);
  lv_obj_set_style_text_color(letter_lbl, lv_color_hex(COL_LETTER), 0);
  lv_obj_set_style_text_font(letter_lbl, &lv_font_montserrat_48, 0);
  lv_label_set_text(letter_lbl, "");
  lv_obj_align(letter_lbl, LV_ALIGN_CENTER, 0, -18);

  word_lbl = lv_label_create(scr);
  lv_obj_set_style_text_color(word_lbl, lv_color_hex(COL_LETTER), 0);
  lv_obj_set_style_text_font(word_lbl, &lv_font_montserrat_28, 0);
  lv_label_set_text(word_lbl, "");
  lv_obj_align(word_lbl, LV_ALIGN_CENTER, 0, 70);

  // Minimal curved volume bar.
  vol_arc = lv_arc_create(scr);
  lv_obj_remove_style_all(vol_arc);
  lv_obj_clear_flag(vol_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(vol_arc, 300, 300);
  lv_obj_center(vol_arc);
  lv_arc_set_rotation(vol_arc, 0);
  lv_arc_set_bg_angles(vol_arc, 40, 140);
  lv_arc_set_range(vol_arc, 0, 100);
  lv_obj_set_style_arc_color(vol_arc, lv_color_hex(COL_VOLTRK), LV_PART_MAIN);
  lv_obj_set_style_arc_width(vol_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(vol_arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_color(vol_arc, lv_color_hex(COL_VOLFILL), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(vol_arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(vol_arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(vol_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(vol_arc, 0, LV_PART_KNOB);
  lv_obj_add_flag(vol_arc, LV_OBJ_FLAG_HIDDEN);

  schedule_blink(lv_tick_get());
  Face_SetState(WB_IDLE);
}

WbState Face_GetState(void) { return current; }

void Face_SetState(WbState state)
{
  current = state;
  Serial.printf("[face] state -> %s\n", wb_state_name(state));

  const bool listen = (state == WB_LISTENING);
  const bool think  = (state == WB_THINKING);
  const bool speak  = (state == WB_SPEAKING);
  const bool spell  = (state == WB_SPELLING);
  const bool rest   = (state == WB_IDLE || listen || think);  // "sitting still" look
  const bool glow   = (listen || think);

  // Still look: white eyes + pupils + smile.
  show(eye_l, rest);
  show(eye_r, rest);
  show(pupil_l, rest);
  show(pupil_r, rest);
  show(smile, rest);
  // Answering look: happy closed eyes + open mouth + tongue.
  show(eyearc_l, speak);
  show(eyearc_r, speak);
  show(mouth, speak);
  show(tongue, speak);

  for (int i = 0; i < 3; i++) show(comets[i], glow);
  show(letter_lbl, spell);
  show(word_lbl, spell);

  if (speak) set_mouth(0.5f);

  uint32_t now = lv_tick_get();
  blink_start_ms = 0;
  schedule_blink(now);
  gaze_next_ms = now + 400;
  gaze_tx = gaze_ty = 0;
  spell_last_ms = now;
}

void Face_ShowVolume(int pct)
{
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  lv_arc_set_value(vol_arc, pct);
  lv_obj_clear_flag(vol_arc, LV_OBJ_FLAG_HIDDEN);
  vol_hide_ms = lv_tick_get() + 1500;
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

static float blink_factor(uint32_t now)
{
  if (blink_start_ms == 0 && now >= blink_next_ms) blink_start_ms = now;
  if (blink_start_ms != 0) {
    uint32_t t = now - blink_start_ms;
    if (t >= BLINK_MS) { blink_start_ms = 0; schedule_blink(now); return 1.0f; }
    float p = (float)t / BLINK_MS;
    return 1.0f - (1.0f - fabsf(1.0f - 2.0f * p));
  }
  return 1.0f;
}

static void update_gaze(uint32_t now, bool wander)
{
  if (wander && now >= gaze_next_ms) {
    if (esp_random() % 3 == 0) { gaze_tx = 0; gaze_ty = 0; }
    else {
      gaze_tx = ((int)(esp_random() % 25) - 12);
      gaze_ty = ((int)(esp_random() % 13) - 6);
    }
    gaze_next_ms = now + 900 + (esp_random() % 1500);
  }
  gaze_cx += (gaze_tx - gaze_cx) * 0.12f;
  gaze_cy += (gaze_ty - gaze_cy) * 0.12f;
}

static void place_eyes(float open)
{
  int eh = (int)(EYE_D * open);
  if (eh < 6) eh = 6;
  lv_obj_set_size(eye_l, EYE_D, eh);
  lv_obj_set_size(eye_r, EYE_D, eh);
  lv_obj_align(eye_l, LV_ALIGN_CENTER, -EYE_DX, EYE_DY);
  lv_obj_align(eye_r, LV_ALIGN_CENTER,  EYE_DX, EYE_DY);

  bool pv = open > 0.4f;
  show(pupil_l, pv);
  show(pupil_r, pv);
  if (pv) {
    int px = (int)gaze_cx;
    int py = (int)gaze_cy + 6;
    int ph = (int)(PUPIL_D * open);
    if (ph < 6) ph = 6;
    lv_obj_set_size(pupil_l, PUPIL_D, ph);
    lv_obj_set_size(pupil_r, PUPIL_D, ph);
    lv_obj_align(pupil_l, LV_ALIGN_CENTER, -EYE_DX + px, EYE_DY + py);
    lv_obj_align(pupil_r, LV_ALIGN_CENTER,  EYE_DX + px, EYE_DY + py);
  }
}

static void spin_edge(float base_deg, lv_opa_t opa)
{
  for (int i = 0; i < 3; i++) {
    int rot = ((int)base_deg + i * 120) % 360;
    if (rot < 0) rot += 360;
    lv_arc_set_rotation(comets[i], rot);
    lv_obj_set_style_arc_opa(comets[i], opa, LV_PART_MAIN);
  }
}

void Face_Tick(void)
{
  uint32_t now = lv_tick_get();
  float t = now / 1000.0f;

  if (vol_hide_ms != 0 && now >= vol_hide_ms) {
    lv_obj_add_flag(vol_arc, LV_OBJ_FLAG_HIDDEN);
    vol_hide_ms = 0;
  }

  switch (current) {
    case WB_IDLE:
      update_gaze(now, true);
      place_eyes(blink_factor(now));
      break;

    case WB_LISTENING:
      update_gaze(now, true);
      place_eyes(blink_factor(now));
      spin_edge(t * 210.0f, 220);
      break;

    case WB_THINKING:
      gaze_tx = 0; gaze_ty = -8;
      update_gaze(now, false);
      place_eyes(blink_factor(now));
      spin_edge(t * 75.0f, 110);
      break;

    case WB_SPEAKING: {
      // Happy closed eyes stay put; the mouth opens/closes like talking.
      float m = 0.55f + 0.30f * sinf(t * 13.0f) + 0.15f * sinf(t * 7.0f);
      set_mouth(m);
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

/*****************************************************************************
 * The WonderBox face — clean, cute, and alive.
 *
 * Design goals (Apple-clean + cutesy):
 *   - a happy face by default that gently blinks and LOOKS AROUND
 *   - a smooth, flowing "Siri-style" glow around the round edge while it is
 *     listening / thinking / speaking (rotating soft light, not choppy rings)
 *   - clear per-state looks:
 *       IDLE      calm smile, eyes wander, no edge glow
 *       LISTENING big smile + bright fast flowing edge
 *       THINKING  eyes glance up + slow, dim flowing edge
 *       SPEAKING  talking mouth + medium flowing edge
 *       SPELLING  one big letter at a time
 *
 * Everything is drawn with LVGL vector primitives (arcs + rounded shapes), so
 * it stays crisp and anti-aliased. Only moving parts update each frame.
 *****************************************************************************/
#include "face.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

// ---- Warm, friendly palette ----
#define COL_BG      0xFFF3E0
#define COL_INK     0x3A3F58
#define COL_LETTER  0x2E7DE1
// Cool, Siri-like flowing edge colors (blend as they overlap)
#define COL_GLOW1   0x37D6D6  // teal
#define COL_GLOW2   0x4AA8FF  // cyan-blue
#define COL_GLOW3   0x8E7BFF  // indigo
#define COL_VOLTRK  0xE7DECB  // volume bar track
#define COL_VOLFILL 0x4AA8FF  // volume bar fill

// ---- Geometry (center 180,180) ----
static const int EYE_D    = 60;
static const int EYE_DX   = 60;
static const int EYE_DY   = -30;
static const int MOUTH_DY = 34;
static const int EDGE_D   = 352;

// ---- Objects ----
static lv_obj_t *eye_l, *eye_r;
static lv_obj_t *smile;        // arc smile (idle/listening/thinking)
static lv_obj_t *talk;         // filled mouth (speaking)
static lv_obj_t *comets[3];    // flowing edge glow
static lv_obj_t *vol_arc;      // curved volume bar
static lv_obj_t *letter_lbl, *word_lbl;

static uint32_t vol_hide_ms = 0;   // when to auto-hide the volume bar

static WbState current = WB_IDLE;

// blink
static uint32_t blink_next_ms = 0, blink_start_ms = 0;
static const uint32_t BLINK_MS = 150;

// gaze (look-around)
static float gaze_cx = 0, gaze_cy = 0, gaze_tx = 0, gaze_ty = 0;
static uint32_t gaze_next_ms = 0;

// spelling
static char spell_word[32] = {0};
static int spell_len = 0, spell_index = 0;
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

static lv_obj_t *make_arc(lv_obj_t *parent, uint32_t color, int size, int width)
{
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_remove_style_all(a);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(a, size, size);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 0);
  lv_arc_set_bg_angles(a, 0, 10);
  lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 0, LV_PART_INDICATOR);          // hide indicator
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);      // hide knob
  lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
  return a;
}

static inline void show(lv_obj_t *o, bool v)
{
  if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void schedule_blink(uint32_t now) { blink_next_ms = now + 2400 + (esp_random() % 2600); }

void Face_Create(void)
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Edge glow (behind everything): three cool arc segments hugging the edge.
  const uint32_t glow_cols[3] = {COL_GLOW1, COL_GLOW2, COL_GLOW3};
  for (int i = 0; i < 3; i++) {
    comets[i] = make_arc(scr, glow_cols[i], EDGE_D, 10);
    lv_arc_set_bg_angles(comets[i], 0, 110);  // fixed segment; we spin via rotation
  }

  eye_l = make_blob(scr, COL_INK, LV_OPA_COVER);
  eye_r = make_blob(scr, COL_INK, LV_OPA_COVER);
  lv_obj_set_size(eye_l, EYE_D, EYE_D);
  lv_obj_set_size(eye_r, EYE_D, EYE_D);

  // Smile arc (bottom curve of a circle = a happy smile).
  smile = make_arc(scr, COL_INK, 96, 12);

  // Talking mouth (open/close during speech).
  talk = make_blob(scr, COL_INK, LV_OPA_COVER);
  lv_obj_set_size(talk, 78, 26);
  lv_obj_align(talk, LV_ALIGN_CENTER, 0, MOUTH_DY);

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

  // Minimal curved volume bar (a rounded arc that fills as volume rises).
  vol_arc = lv_arc_create(scr);
  lv_obj_remove_style_all(vol_arc);
  lv_obj_clear_flag(vol_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(vol_arc, 300, 300);
  lv_obj_center(vol_arc);
  lv_arc_set_rotation(vol_arc, 0);
  lv_arc_set_bg_angles(vol_arc, 40, 140);       // a curved bar across the bottom
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

// Position the smile arc. `wide` = broader, happier smile.
static void set_smile(bool wide)
{
  int a = wide ? 28 : 34;          // start angle
  int b = wide ? 152 : 146;        // end angle (through 90 = bottom = smile)
  lv_arc_set_bg_angles(smile, a, b);
  int size = wide ? 108 : 96;
  lv_obj_set_size(smile, size, size);
  lv_obj_align(smile, LV_ALIGN_CENTER, 0, MOUTH_DY - size / 2 + 8);
}

void Face_SetState(WbState state)
{
  current = state;
  Serial.printf("[face] state -> %s\n", wb_state_name(state));

  const bool face   = (state != WB_SPELLING);
  const bool listen = (state == WB_LISTENING);
  const bool think  = (state == WB_THINKING);
  const bool speak  = (state == WB_SPEAKING);
  const bool spell  = (state == WB_SPELLING);
  const bool glow   = (listen || think || speak);

  show(eye_l, face);
  show(eye_r, face);
  show(smile, face && !speak);        // smile for idle/listening/thinking
  show(talk, speak);                  // open mouth only while speaking
  for (int i = 0; i < 3; i++) show(comets[i], glow);
  show(letter_lbl, spell);
  show(word_lbl, spell);

  if (face && !speak) set_smile(listen);   // big happy smile while listening

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
  vol_hide_ms = lv_tick_get() + 1500;   // auto-hide after 1.5s
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

// Smoothly wander the eyes to make the face feel alive.
static void update_gaze(uint32_t now, bool wander)
{
  if (wander && now >= gaze_next_ms) {
    if (esp_random() % 3 == 0) { gaze_tx = 0; gaze_ty = 0; }      // often re-center
    else {
      gaze_tx = ((int)(esp_random() % 29) - 14);                  // -14..14
      gaze_ty = ((int)(esp_random() % 15) - 7);                   // -7..7
    }
    gaze_next_ms = now + 900 + (esp_random() % 1500);
  }
  gaze_cx += (gaze_tx - gaze_cx) * 0.12f;                          // ease toward target
  gaze_cy += (gaze_ty - gaze_cy) * 0.12f;
}

static void place_eyes(float open)
{
  int h = (int)(6 + (EYE_D - 6) * open);
  if (h < 6) h = 6;
  int dx = (int)gaze_cx, dy = (int)gaze_cy;
  lv_obj_set_height(eye_l, h);
  lv_obj_set_height(eye_r, h);
  lv_obj_align(eye_l, LV_ALIGN_CENTER, -EYE_DX + dx, EYE_DY + dy);
  lv_obj_align(eye_r, LV_ALIGN_CENTER, EYE_DX + dx, EYE_DY + dy);
}

// Rotate the three glow arcs to make a smooth flowing edge of light.
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

  // Auto-hide the volume bar.
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
      spin_edge(t * 210.0f, 220);            // bright, fast, flowing
      break;

    case WB_THINKING:
      gaze_tx = 0; gaze_ty = -8;             // glance up, pondering
      update_gaze(now, false);
      place_eyes(blink_factor(now));
      spin_edge(t * 75.0f, 110);             // slow, gentle
      break;

    case WB_SPEAKING: {
      gaze_tx = 0; gaze_ty = 0;
      update_gaze(now, false);
      place_eyes(blink_factor(now));
      float m = 0.5f + 0.5f * sinf(t * 11.0f);
      int h = 16 + (int)(40 * m);
      lv_obj_set_size(talk, 80, h);
      lv_obj_align(talk, LV_ALIGN_CENTER, 0, MOUTH_DY);
      spin_edge(t * 130.0f, 170);            // medium
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

/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * pet_status_view.c — 圆屏像素宠物状态环（M7 差异化页，纯 LVGL 退路）。
 *
 * 设计见 pet_status_view.h。一只像素风小动物（pet_sprite.h，CC0 资源）作为
 * 待机首屏的宠物主体，配一个随 agent 状态变色的状态环 + 状态文案。视觉对齐
 * HachimoDock 设备端设计语言：近黑底 + 柔和低饱和状态色（ui_palette.h），
 * 摒弃 qemu 搬来的深色霓虹风。状态语义镜像 agent 生命周期：灰=待机、teal=
 * 聆听、蓝=思考、橙=执行、琥珀=等待、红=错误。所有 widget 操作经 lv_async_call
 * 调度到 LVGL 线程，动画一律用 lv_anim_t（v9 API），避免裸定时器抖动。
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_PET_STATUS

#include "ui/pet_status_view.h"
#include "ui/ui_palette.h"
#include "ui/lvgl_ui_channel.h"
#include "ui/pet_sprite.h"
#include "ui/frog_sprite.h"
#include "ui/dog_sprite.h"
#include "ui/pig_sprite.h"
#include "config/config_store.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <time.h>

#include <lvgl/lvgl.h>

#define TAG "pet_view"

/* 屏幕切换淡入时长(ms)。硬 lv_screen_load() 的整帧 SPI flush 会与面板 60Hz
 * GRAM 扫描竞争,表现为从上到下的白底刷屏;改用淡入交叉渐变把新屏的出现分散到
 * 多帧(慢 SPI 约 3.5fps),把刷屏变成优雅的渐隐渐显。 */
#define SCREEN_FADE_MS 500

/* The MiSans CJK static font compiled into liblvgl.a on CP (and into the
 * ai_agent .o on qemu/AP-LVGL_UI). Declared by the font .c via LV_FONT_DECLARE. */
LV_FONT_DECLARE(lv_font_misans_12_cjk);

/* Config key holding the one species this flash runs with. */
#define PET_TYPE_CONFIG_KEY "pet_type"

/* 出厂形象。旧行为是「每次烧录随机一只」;2026-11-10 用户要求先固定为
 * 白猫,故改为常量。要恢复随机,把下面的赋值换回 rnd % PET_TYPE_COUNT。 */
#define PET_TYPE_FACTORY_DEFAULT PET_TYPE_CAT

/* Pick the species for this boot.
 *
 * The pet must NOT change as the user moves between pages, so the species is
 * a boot-time constant rather than a UI toggle: read it back from the config
 * store (survives reboots, wiped by a full flash), or pick one at random the
 * first time and persist it.  The pick is logged so a test run can tell which
 * sprite should be on screen.
 */
static int pet_type_fixed(void)
{
  char buf[8];
  int type;

  if (claw_config_get(PET_TYPE_CONFIG_KEY, buf, sizeof(buf)) == OK &&
      buf[0] != '\0')
    {
      type = atoi(buf);
      if (type >= 0 && type < PET_TYPE_COUNT)
        {
          syslog(LOG_INFO, "[%s] fixed type %d (from config)\n", TAG, type);
          return type;
        }
    }

  type = PET_TYPE_FACTORY_DEFAULT;

  snprintf(buf, sizeof(buf), "%d", type);
  if (claw_config_set(PET_TYPE_CONFIG_KEY, buf) != OK)
    {
      syslog(LOG_WARNING, "[%s] type %d not persisted (no config store)\n",
             TAG, type);
    }
  syslog(LOG_INFO, "[%s] fixed type %d (factory default)\n", TAG, type);
  return type;
}

/* Layout — sized for the 240×296 round LCD but centers on any display
 * (qemu 466 round included) via lv_display_get_*res at init. */
#define PET_RING_SIZE     150
#define PET_SPRITE_SCALE  6     /* 16px source → 96px on screen (nearest-neighbor) */
#define PET_LIFT_UP       36   /* raise sprite+ring so the 3-line idle legend fits
                                * below: ring bottom y=187, legend top y≈194 */
#define PET_LABEL_DY      76   /* legend block center from screen center: 3 lines
                                * × 20px = 60px → spans y=194..254, clear of the
                                * lifted ring (bottom 187) and round mask (~268). */

/* State palette — HachimoDock 柔和低饱和状态色（见 ui_palette.h）。 */
static const uint32_t s_state_color[] = {
  UI_PAL_IDLE,   /* IDLE      grey   */
  UI_PAL_LISTEN, /* LISTENING teal   */
  UI_PAL_THINK,  /* THINKING  blue   */
  UI_PAL_EXEC,   /* EXECUTING orange */
  UI_PAL_WAIT,   /* WAITING   amber  */
  UI_PAL_ERROR,  /* ERROR     red    */
};
/* Short state names — used for the status caption on non-idle states and for
 * the syslog line.  IDLE shows the on-device button legend below instead. */
static const char *const s_state_text[] = {
  "待机", "聆听中", "思考中", "执行中", "等待确认", "出错了",
};

/* Idle caption: the on-device button legend.  Each line is a compact key
 * caption ("S1"/"S2"/"S3") followed by the action text, and only the three
 * *action texts* are justified to a common left and right edge (两端对齐) —
 * the key captions stay tight and left-aligned, never stretched.
 *
 * S1 (GPIO13) = push-to-talk; S2/S3 long-press = model / new session; the
 * plain tap on S2/S3 scrolls the chat transcript.
 *
 * LVGL v9 has no "justify" text align, so each action text is its own
 * fixed-width, centre-aligned label and the slack between its natural width
 * and the block width becomes that label's letter spacing (see
 * pet_view_create). */
static const char *const s_hint_key[]  = { "S1", "S2", "S3" };
static const char *const s_hint_text[] = {
  "按住说话",
  "长按切换模型",
  "长按新建会话",
};

#define IDLE_HINT_LINES \
  ((int)(sizeof(s_hint_text) / sizeof(s_hint_text[0])))

/* Glyph count of a UTF-8 string (continuation bytes 10xxxxxx don't count). */
static uint32_t utf8_glyph_count(const char *s)
{
  uint32_t n = 0;

  for (; *s != '\0'; s++)
    {
      if (((uint8_t)*s & 0xc0u) != 0x80u) n++;
    }

  return n;
}

static const char *display_text_for(enum pet_state st)
{
  /* IDLE is drawn by the justified hint block, not by the caption label. */
  return (st == PET_STATE_IDLE) ? "" : s_state_text[st];
}

struct pet_state_s {
  bool initialized;
  bool visible;
  enum pet_state state;
  enum pet_type type;
  lv_obj_t *screen;
  lv_obj_t *ring;     /* lv_arc: status ring around sprite */
  lv_obj_t *sprite;   /* lv_image: pixel-art pet (scaled) */
  lv_obj_t *label;
  lv_obj_t *status_dot; /* colored dot next to the state caption */
  lv_obj_t *hint_box;   /* IDLE only: button legend (key captions + justified text) */
  lv_obj_t *hint_key[IDLE_HINT_LINES];  /* per-line "S1"/"S2"/"S3" captions */
  lv_obj_t *hint_lbl[IDLE_HINT_LINES];  /* per-line justified action text */
  lv_obj_t *comet;    /* lv_obj: orbiting dot (busy-state spinner) */
  lv_obj_t *prev_screen;
  /* active animation tag — one effect at a time, stopped on state change */
  bool anim_active;
};

static struct pet_state_s s_pet;

/* Concrete ERROR reason (e.g. "未联网\n请先连接 Wi-Fi"), replacing the
 * generic "出错了" caption so the round screen says what actually failed.
 * Empty = no reason set, fall back to the generic caption.  Written and
 * read only in the LVGL thread (via lv_async_call). */
static char s_error_text[64];

/* Per-type expression frame tables.  All six faces share the same state→face
 * mapping (base/blink/eyes_up/eyes_down/focused/error); the pet type only
 * swaps which sprite sheet supplies those faces. */
struct pet_frames_s {
  const lv_image_dsc_t *base;      /* IDLE / WAITING open-eye frame */
  const lv_image_dsc_t *blink;     /* WAITING closed-eye frame */
  const lv_image_dsc_t *eyes_up;   /* THINKING */
  const lv_image_dsc_t *eyes_down; /* LISTENING */
  const lv_image_dsc_t *focused;   /* EXECUTING */
  const lv_image_dsc_t *error;     /* ERROR */
};

static const struct pet_frames_s s_frames_by_type[PET_TYPE_COUNT] = {
  [PET_TYPE_CAT]  = {
    &pet_sprite_base, &pet_sprite_blink, &pet_sprite_eyes_up,
    &pet_sprite_eyes_down, &pet_sprite_focused, &pet_sprite_error,
  },
  [PET_TYPE_FROG] = {
    &frog_sprite_base, &frog_sprite_blink, &frog_sprite_eyes_up,
    &frog_sprite_eyes_down, &frog_sprite_focused, &frog_sprite_error,
  },
  [PET_TYPE_DOG] = {
    &dog_sprite_base, &dog_sprite_blink, &dog_sprite_eyes_up,
    &dog_sprite_eyes_down, &dog_sprite_focused, &dog_sprite_error,
  },
  [PET_TYPE_PIG] = {
    &pig_sprite_base, &pig_sprite_blink, &pig_sprite_eyes_up,
    &pig_sprite_eyes_down, &pig_sprite_focused, &pig_sprite_error,
  },
};

static const struct pet_frames_s *s_frames = &s_frames_by_type[PET_TYPE_CAT];

/* ---------- animation exec callbacks (LVGL v9: (obj, value)) ---------- */

/* LVGL v9 exec_cb signature is lv_anim_exec_xcb_t = void(*)(void*, lv_anim_value_t),
 * i.e. the first arg is the animated var (void*), NOT lv_obj_t* nor lv_anim_t*.
 * We cast to lv_obj_t* inside for the lv_obj and lv_arc calls. */
/* Orbit: a small dot travels around the ring (THINKING / LISTENING / EXECUTING).
 * value = angle 0..360.
 *
 * The previous spinner used lv_arc_set_rotation(), which calls
 * lv_obj_invalidate() on the whole 150px arc (lv_arc.c) every frame, so the
 * 13 MHz SPI re-flushed the entire ring each step and the motion read as a
 * segmented, stuttering jump.  A single orbiting dot redraws only its own
 * ~12px old/new rectangles, keeping the frame near the partial-refresh
 * ceiling so the orbit reads smooth. */
#define COMET_RADIUS 73   /* px from ring center, just inside the 4px band */
#define COMET_SIZE    12

static void comet_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* lv_trigo_sin is full-scale (LV_TRIGO_SIN_MAX); >> LV_TRIGO_SHIFT normalizes
   * to a ±1 multiplier.  v+90 gives cos: x = r·cos(v), y = r·sin(v). */
  int32_t dx = (COMET_RADIUS * lv_trigo_sin(v + 90)) >> LV_TRIGO_SHIFT;
  int32_t dy = (COMET_RADIUS * lv_trigo_sin(v))      >> LV_TRIGO_SHIFT;
  lv_obj_set_style_translate_x(o, dx, 0);
  lv_obj_set_style_translate_y(o, dy, 0);
}

/* Breathing: sprite opacity pulses (IDLE). value 0..100. */
static void breathe_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* 100% → 255, 0% → 190 (gentle). */
  lv_obj_set_style_opa(o, LV_OPA_COVER - (v * (LV_OPA_COVER - 190)) / 100, 0);
}

/* Float: gentle vertical bob (IDLE, layered with the opacity breathe so the
 * idle pet feels alive rather than a static icon). value 0..100. */
static void float_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* Triangle 0→+2→0→-2→0 across one cycle — a ±2px bob on a 96px sprite. */
  int32_t amp;
  if (v < 25)      amp = (v * 2) / 25;
  else if (v < 50) amp = ((50 - v) * 2) / 25;
  else if (v < 75) amp = -((v - 50) * 2) / 25;
  else             amp = -((100 - v) * 2) / 25;
  lv_obj_set_style_translate_y(o, amp, 0);
}

/* Blink: swap between open/closed eyes (WAITING). value 0..100, closed at mid. */
static void blink_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* open → closed(35..65) → open.  Two-frame swap reads as a pixel blink. */
  lv_image_set_src(o, (v >= 35 && v <= 65) ? s_frames->blink
                                            : s_frames->base);
}

/* Shake: sprite x jitter (ERROR). value 0..100 maps to a few oscillations. */
static void shake_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* 0..100 → four half-swings of ±5px. Simple triangle per 25-step. */
  int seg = v / 25;
  int phase = v % 25;
  int amp = (phase < 13) ? (phase * 5 / 12) : ((25 - phase) * 5 / 12);
  int sign = (seg % 2 == 0) ? 1 : -1;
  lv_obj_set_style_translate_x(o, sign * amp, 0);
}

static void stop_anim(void)
{
  /* Delete any anim bound to our widgets. lv_anim_delete is a no-op if none. */
  if (s_pet.comet)  lv_anim_delete(s_pet.comet,  comet_cb);
  if (s_pet.sprite) lv_anim_delete(s_pet.sprite, breathe_cb);
  if (s_pet.sprite) lv_anim_delete(s_pet.sprite, shake_cb);
  if (s_pet.sprite) lv_anim_delete(s_pet.sprite, blink_cb);
  if (s_pet.sprite) lv_anim_delete(s_pet.sprite, float_cb);
  s_pet.anim_active = false;
}

/* Per-state face: each agent state shows a distinct expression (eyes-only edits
 * on the CC0 base face — see gen_pet_expressions.py).  WAITING returns the
 * open-eye base because its blink anim swaps base↔blink itself. */
static const lv_image_dsc_t *sprite_for(enum pet_state st)
{
  switch (st)
    {
      case PET_STATE_LISTENING: return s_frames->eyes_down;
      case PET_STATE_THINKING:  return s_frames->eyes_up;
      case PET_STATE_EXECUTING: return s_frames->focused;
      case PET_STATE_ERROR:     return s_frames->error;
      case PET_STATE_WAITING:
      case PET_STATE_IDLE:
      default:                  return s_frames->base;
    }
}

static void start_anim_for(enum pet_state st)
{
  lv_anim_t a;
  uint32_t color = s_state_color[st];

  stop_anim();
  if (!s_pet.ring || !s_pet.sprite || !s_pet.comet) return;

  /* Indicator color always tracks state (the dark track stays constant);
   * base angles reset per effect.  Clear any rotation left by a previous
   * spinner so the static rings (0..360) draw where they should. */
  lv_obj_set_style_arc_color(s_pet.ring, lv_color_hex(color), LV_PART_INDICATOR);
  lv_arc_set_rotation(s_pet.ring, 0);
  /* Bright white bead on the state-colored ring so the orbit reads against
   * the tinted track (a state-colored dot would blend into the same-tone arc). */
  lv_obj_set_style_bg_color(s_pet.comet, lv_color_hex(UI_PAL_CARD), 0);
  lv_obj_add_flag(s_pet.comet, LV_OBJ_FLAG_HIDDEN);

  /* Face tracks state too — each state's expression frame (blink anim swaps it). */
  lv_image_set_src(s_pet.sprite, sprite_for(st));

  switch (st)
    {
      case PET_STATE_THINKING:
      case PET_STATE_LISTENING:
        /* Static faint ring + an orbiting dot.  Only the dot's small rect is
         * redrawn per frame, so the orbit stays smooth on the 13 MHz SPI. */
        lv_arc_set_angles(s_pet.ring, 0, 360);
        lv_obj_remove_flag(s_pet.comet, LV_OBJ_FLAG_HIDDEN);
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.comet);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a, comet_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        break;
      case PET_STATE_EXECUTING:
        /* Same orbiting dot, faster orbit for the "working hard" feel. */
        lv_arc_set_angles(s_pet.ring, 0, 360);
        lv_obj_remove_flag(s_pet.comet, LV_OBJ_FLAG_HIDDEN);
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.comet);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, 900);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a, comet_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        break;
      case PET_STATE_IDLE:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.sprite);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 1800);
        lv_anim_set_exec_cb(&a, breathe_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        /* Layered float: a slow bob on a distinct period so the two effects
         * don't phase-lock.  lv_anim_start copies `a`, so it is safe to reuse
         * the same local descriptor for a second anim. */
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.sprite);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 2600);
        lv_anim_set_exec_cb(&a, float_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_arc_set_angles(s_pet.ring, 0, 360);  /* full ring */
        break;
      case PET_STATE_WAITING:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.sprite);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 900);
        lv_anim_set_exec_cb(&a, blink_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_arc_set_angles(s_pet.ring, 0, 360);
        break;
      case PET_STATE_ERROR:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.sprite);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 350);
        lv_anim_set_exec_cb(&a, shake_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_arc_set_angles(s_pet.ring, 0, 360);
        break;
      default:
        lv_arc_set_angles(s_pet.ring, 0, 360);
        break;
    }
  s_pet.anim_active = true;
}

/* ---------- async apply (runs in LVGL thread) ---------- */

static void apply_state_async_cb(void *data)
{
  enum pet_state st = (enum pet_state)(intptr_t)data;
  if (!s_pet.initialized || !s_pet.sprite) return;

  s_pet.state = st;

  /* Reset the sprite to a clean base frame; per-state anim re-derives it. */
  lv_image_set_src(s_pet.sprite, s_frames->base);
  lv_obj_set_style_translate_x(s_pet.sprite, 0, 0);  /* clear any shake offset */
  lv_obj_set_style_translate_y(s_pet.sprite, 0, 0);  /* clear any float offset */
  lv_obj_set_style_opa(s_pet.sprite, LV_OPA_COVER, 0);

  if (s_pet.label) {
    if (st == PET_STATE_ERROR && s_error_text[0] != '\0') {
      lv_label_set_text(s_pet.label, s_error_text);
      lv_obj_set_style_text_color(s_pet.label,
                                  lv_color_hex(UI_PAL_ERROR), 0);
    } else {
      lv_label_set_text(s_pet.label, display_text_for(st));
      lv_obj_set_style_text_color(s_pet.label,
                                  lv_color_hex(UI_PAL_TEXT_DIM), 0);
    }
  }

  /* Leaving ERROR clears the reason: a later, unrelated error must not
   * resurrect an old explanation. */
  if (st != PET_STATE_ERROR) {
    s_error_text[0] = '\0';
  }
  if (s_pet.status_dot) {
    lv_obj_set_style_bg_color(s_pet.status_dot, lv_color_hex(s_state_color[st]), 0);
    /* Legend (idle) has no status dot; busy states show it. */
    if (st == PET_STATE_IDLE)
      lv_obj_add_flag(s_pet.status_dot, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_remove_flag(s_pet.status_dot, LV_OBJ_FLAG_HIDDEN);
  }

  /* The justified button legend replaces the status caption while idle. */
  if (s_pet.hint_box) {
    if (st == PET_STATE_IDLE)
      lv_obj_remove_flag(s_pet.hint_box, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_pet.hint_box, LV_OBJ_FLAG_HIDDEN);
  }

  start_anim_for(st);
  syslog(LOG_INFO, "[%s] state → %s\n", TAG, s_state_text[st]);
}

void pet_status_view_set_state(enum pet_state state)
{
  if ((int)state < 0 || (int)state > PET_STATE_ERROR) return;
  /* Async: widget mutation must happen in the LVGL thread. */
  lvgl_ui_async_call(apply_state_async_cb, (void *)(intptr_t)state);
}

/* Runs in the LVGL thread; owns and frees the heap copy handed over by
 * pet_status_view_set_error_text(). */
static void apply_error_text_async_cb(void *data)
{
  char *text = (char *)data;

  if (text) {
    strncpy(s_error_text, text, sizeof(s_error_text) - 1);
    s_error_text[sizeof(s_error_text) - 1] = '\0';
    free(text);
  } else {
    s_error_text[0] = '\0';
  }

  if (!s_pet.initialized || !s_pet.label || s_pet.state != PET_STATE_ERROR) {
    return;
  }

  if (s_error_text[0] != '\0') {
    lv_label_set_text(s_pet.label, s_error_text);
    lv_obj_set_style_text_color(s_pet.label, lv_color_hex(UI_PAL_ERROR), 0);
  } else {
    lv_label_set_text(s_pet.label, display_text_for(PET_STATE_ERROR));
    lv_obj_set_style_text_color(s_pet.label,
                                lv_color_hex(UI_PAL_TEXT_DIM), 0);
  }
}

void pet_status_view_set_error_text(const char *text)
{
  char *copy = NULL;

  if (text && text[0] != '\0') {
    copy = strdup(text);
    if (copy == NULL) {
      return;
    }
  }

  /* Async: widget mutation must happen in the LVGL thread.  Called before
   * set_state(ERROR) so the reason is in place when the caption updates. */
  lvgl_ui_async_call(apply_error_text_async_cb, copy);
}

static void apply_type_async_cb(void *data)
{
  enum pet_type type = (enum pet_type)(intptr_t)data;
  if (!s_pet.initialized || !s_pet.sprite) return;

  s_pet.type = type;
  s_frames = &s_frames_by_type[type];

  /* Re-derive the current state's expression under the new sprite sheet and
   * restart its anim (blink_cb reads s_frames, so it picks up the swap too). */
  lv_image_set_src(s_pet.sprite, s_frames->base);
  lv_obj_set_style_translate_x(s_pet.sprite, 0, 0);
  lv_obj_set_style_translate_y(s_pet.sprite, 0, 0);
  lv_obj_set_style_opa(s_pet.sprite, LV_OPA_COVER, 0);

  start_anim_for(s_pet.state);
  syslog(LOG_INFO, "[%s] type → %d\n", TAG, (int)type);
}

void pet_status_view_set_type(int type)
{
  char buf[8];

  /* Out of range means "no change" — the species is fixed, not a fallback
   * toggle.  A debug switch persists so the next boot keeps it. */
  if (type < 0 || type >= PET_TYPE_COUNT)
    {
      return;
    }

  snprintf(buf, sizeof(buf), "%d", type);
  if (claw_config_set(PET_TYPE_CONFIG_KEY, buf) != OK)
    {
      syslog(LOG_WARNING, "[%s] type %d not persisted (no config store)\n",
             TAG, type);
    }
  lvgl_ui_async_call(apply_type_async_cb, (void *)(intptr_t)type);
}

int pet_status_view_get_type(void)
{
  return (int)s_pet.type;
}

/* ---------- screen show / hide ---------- */

static void show_async_cb(void *data)
{
  (void)data;
  if (!s_pet.initialized || !s_pet.screen) return;
  /* "Already shown" must be judged by the active screen, not by the cached
   * s_pet.visible flag: the chat screen can take the foreground without
   * calling pet_status_view_hide(), and the idle-yield path then needs this
   * call to actually bring the pet back. */
  if (lv_screen_active() == s_pet.screen)
    {
      s_pet.visible = true;
      return;
    }
  s_pet.prev_screen = lv_screen_active();
  lv_screen_load_anim(s_pet.screen, LV_SCR_LOAD_ANIM_FADE_IN,
                      SCREEN_FADE_MS, 0, false);
  /* lv_screen_load already invalidates the full screen and schedules a redraw;
   * the explicit invalidate + lv_refr_now below was a qemu-goldfish-headless
   * lazy-flush workaround that is unnecessary on the real CP /dev/lcd0 panel,
   * where it caused a full-frame (top-to-bottom) white flicker on every
   * screen switch.  Real hardware renders screen switches fine without
   * the extra synchronous refresh, so we drop it to stop the standby flicker. */
  s_pet.visible = true;
  /* Re-assert current state's animation (lv_anim may have been paused off-screen). */
  start_anim_for(s_pet.state);
  syslog(LOG_INFO, "[%s] pet screen shown\n", TAG);
}

void pet_status_view_show(void)
{
  if (!s_pet.initialized) return;
  lvgl_ui_async_call(show_async_cb, NULL);
}

static void hide_async_cb(void *data)
{
  (void)data;
  if (!s_pet.initialized || !s_pet.visible) return;
  stop_anim();
  if (s_pet.prev_screen)
    lv_screen_load_anim(s_pet.prev_screen, LV_SCR_LOAD_ANIM_FADE_IN,
                        SCREEN_FADE_MS, 0, false);
  s_pet.visible = false;
  syslog(LOG_INFO, "[%s] pet screen hidden\n", TAG);
}

void pet_status_view_hide(void)
{
  if (!s_pet.initialized) return;
  lvgl_ui_async_call(hide_async_cb, NULL);
}

bool pet_status_view_is_visible(void)
{
  return s_pet.visible;
}

/* LVGL-thread only: compares against the live active screen so callers in the
 * timer thread see the truth even when another view took the foreground
 * without a matching hide(). */
bool pet_status_view_is_active(void)
{
  return s_pet.initialized && s_pet.screen != NULL &&
         lv_screen_active() == s_pet.screen;
}

/* ---------- init ---------- */

int pet_status_view_init(void)
{
  if (s_pet.initialized) return 0;

  memset(&s_pet, 0, sizeof(s_pet));
  s_pet.state = PET_STATE_IDLE;
  s_pet.type = (enum pet_type)pet_type_fixed();
  s_frames = &s_frames_by_type[s_pet.type];

  s_pet.screen = lv_obj_create(NULL);
  if (!s_pet.screen) return -ENOMEM;
  lv_obj_set_style_bg_color(s_pet.screen, lv_color_hex(UI_PAL_BG), 0);
  lv_obj_set_style_bg_opa(s_pet.screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_font(s_pet.screen, &lv_font_misans_12_cjk, 0);

  /* Ring (arc) — centered, subtle dark track, state-colored indicator. */
  s_pet.ring = lv_arc_create(s_pet.screen);
  if (s_pet.ring)
    {
      lv_obj_set_size(s_pet.ring, PET_RING_SIZE, PET_RING_SIZE);
      lv_obj_align(s_pet.ring, LV_ALIGN_CENTER, 0, -PET_LIFT_UP);
      lv_arc_set_rotation(s_pet.ring, 0);
      lv_arc_set_bg_angles(s_pet.ring, 0, 360);
      lv_arc_set_angles(s_pet.ring, 0, 360);
      lv_arc_set_range(s_pet.ring, 0, 360);
      /* Track (LV_PART_MAIN): a subtle dark full circle that stays put. */
      lv_obj_set_style_arc_width(s_pet.ring, 4, LV_PART_MAIN);
      lv_obj_set_style_arc_color(s_pet.ring, lv_color_hex(UI_PAL_TRACK), LV_PART_MAIN);
      lv_obj_set_style_arc_rounded(s_pet.ring, true, LV_PART_MAIN);
      /* Indicator (LV_PART_INDICATOR): the state-colored spinner / fill.
       * (Previously only LV_PART_MAIN was styled, so the animated segment fell
       * back to the theme's default blue arc — the "ugly" ring.) */
      lv_obj_set_style_arc_width(s_pet.ring, 4, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(s_pet.ring, lv_color_hex(UI_PAL_IDLE), LV_PART_INDICATOR);
      lv_obj_set_style_arc_rounded(s_pet.ring, true, LV_PART_INDICATOR);
      lv_obj_remove_flag(s_pet.ring, LV_OBJ_FLAG_CLICKABLE);
      /* Hide the arc knob. */
      lv_obj_set_style_bg_opa(s_pet.ring, LV_OPA_TRANSP, LV_PART_KNOB);
    }

  /* Pixel pet — CC0 sprite scaled 6x (nearest-neighbor for crisp pixels). */
  s_pet.sprite = lv_image_create(s_pet.screen);
  if (!s_pet.sprite) goto fail;
  lv_image_set_src(s_pet.sprite, s_frames->base);
  lv_image_set_antialias(s_pet.sprite, false);
  lv_image_set_scale(s_pet.sprite, PET_SPRITE_SCALE * 256);
  lv_obj_align(s_pet.sprite, LV_ALIGN_CENTER, 0, -PET_LIFT_UP);

  /* Orbiting comet dot for busy states — a small circle travelling around the
   * ring, translated from the ring center each animation frame.  Mirrors the
   * status_dot styling so it renders as a clean, borderless disc. */
  s_pet.comet = lv_obj_create(s_pet.screen);
  if (!s_pet.comet) goto fail;
  lv_obj_remove_flag(s_pet.comet, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_pet.comet, COMET_SIZE, COMET_SIZE);
  lv_obj_set_style_radius(s_pet.comet, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_pet.comet, lv_color_hex(UI_PAL_IDLE), 0);
  lv_obj_set_style_bg_opa(s_pet.comet, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_pet.comet, 0, 0);
  lv_obj_set_style_pad_all(s_pet.comet, 0, 0);
  lv_obj_align(s_pet.comet, LV_ALIGN_CENTER, 0, -PET_LIFT_UP);
  lv_obj_add_flag(s_pet.comet, LV_OBJ_FLAG_HIDDEN);

  /* Status caption row below the sprite: a colored status dot (HachimoDock
   * circular status marker) + the state text, laid out horizontally. */
  {
    lv_obj_t *caption = lv_obj_create(s_pet.screen);
    if (!caption) goto fail;
    lv_obj_remove_flag(caption, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(caption, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(caption, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(caption, 0, 0);
    lv_obj_set_style_pad_all(caption, 0, 0);
    lv_obj_set_flex_flow(caption, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(caption, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(caption, 6, 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, PET_LABEL_DY);

    s_pet.status_dot = lv_obj_create(caption);
    if (s_pet.status_dot)
      {
        lv_obj_remove_flag(s_pet.status_dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_pet.status_dot, 8, 8);
        lv_obj_set_style_radius(s_pet.status_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_pet.status_dot, lv_color_hex(UI_PAL_IDLE), 0);
        lv_obj_set_style_bg_opa(s_pet.status_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_pet.status_dot, 0, 0);
        lv_obj_set_style_pad_all(s_pet.status_dot, 0, 0);
        /* Idle shows the legend block, not a status dot. */
        lv_obj_add_flag(s_pet.status_dot, LV_OBJ_FLAG_HIDDEN);
      }

    s_pet.label = lv_label_create(caption);
    if (s_pet.label)
      {
        lv_label_set_text(s_pet.label, display_text_for(PET_STATE_IDLE));
        lv_obj_set_style_text_color(s_pet.label, lv_color_hex(UI_PAL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(s_pet.label, &lv_font_misans_12_cjk, 0);
        lv_obj_set_style_text_align(s_pet.label, LV_TEXT_ALIGN_LEFT, 0);
      }
  }

  /* IDLE button legend.  Only the action texts are justified (common left and
   * right edge); the "S1"/"S2"/"S3" key captions stay compact and left-aligned.
   *
   * LVGL v9 has no justify align, so each action text is its own fixed-width
   * label centred inside the block, and the slack between its natural width
   * and the block width is spread into its letter spacing.  Integer px letter
   * spacing means one or two px of the slack may be left over; the centre
   * align splits it between the two edges so both stay flush within half a
   * pixel of the gutter. */
  {
    int32_t text_w[IDLE_HINT_LINES];
    uint32_t glyphs[IDLE_HINT_LINES];
    int32_t block_w = 0;   /* widest action text -> all texts flush both edges */
    int32_t key_w   = 0;   /* widest key caption -> texts share one left edge */
    int i;

    for (i = 0; i < IDLE_HINT_LINES; i++)
      {
        const char *t = s_hint_text[i];
        int32_t kw    = lv_text_get_width(s_hint_key[i],
                                          (uint32_t)strlen(s_hint_key[i]),
                                          &lv_font_misans_12_cjk, 0);

        glyphs[i] = utf8_glyph_count(t);
        text_w[i] = lv_text_get_width(t, (uint32_t)strlen(t),
                                      &lv_font_misans_12_cjk, 0);
        if (text_w[i] > block_w) block_w = text_w[i];
        if (kw > key_w)           key_w   = kw;
      }

    s_pet.hint_box = lv_obj_create(s_pet.screen);
    if (!s_pet.hint_box) goto fail;
    lv_obj_remove_flag(s_pet.hint_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pet.hint_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_pet.hint_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pet.hint_box, 0, 0);
    lv_obj_set_style_pad_all(s_pet.hint_box, 0, 0);
    lv_obj_set_style_pad_row(s_pet.hint_box, 6, 0);
    lv_obj_set_flex_flow(s_pet.hint_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pet.hint_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_align(s_pet.hint_box, LV_ALIGN_CENTER, 0, PET_LABEL_DY);

    for (i = 0; i < IDLE_HINT_LINES; i++)
      {
        int32_t gaps = (int32_t)glyphs[i] - 1;
        int32_t ls   = 0;
        lv_obj_t *row = lv_obj_create(s_pet.hint_box);

        if (!row) goto fail;
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 4, 0);   /* gap after the key caption */
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        s_pet.hint_key[i] = lv_label_create(row);
        if (!s_pet.hint_key[i]) goto fail;
        lv_label_set_text(s_pet.hint_key[i], s_hint_key[i]);
        lv_obj_set_width(s_pet.hint_key[i], key_w);   /* common left edge */
        lv_obj_set_style_text_align(s_pet.hint_key[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(s_pet.hint_key[i],
                                    lv_color_hex(UI_PAL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(s_pet.hint_key[i], &lv_font_misans_12_cjk, 0);

        if (gaps > 0 && block_w > text_w[i])
          {
            /* Round to nearest px: (n + gaps/2) / gaps. */
            ls = (block_w - text_w[i] + gaps / 2) / gaps;
          }

        s_pet.hint_lbl[i] = lv_label_create(row);
        if (!s_pet.hint_lbl[i]) goto fail;
        lv_label_set_text(s_pet.hint_lbl[i], s_hint_text[i]);
        lv_obj_set_width(s_pet.hint_lbl[i], block_w);
        lv_obj_set_style_text_align(s_pet.hint_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_letter_space(s_pet.hint_lbl[i], ls, 0);
        lv_obj_set_style_text_color(s_pet.hint_lbl[i],
                                    lv_color_hex(UI_PAL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(s_pet.hint_lbl[i], &lv_font_misans_12_cjk, 0);

        syslog(LOG_INFO, "[%s] hint[%d] text_w=%d glyphs=%d letter_space=%d"
               " block=%d key_w=%d\n", TAG, i, (int)text_w[i], (int)glyphs[i],
               (int)ls, (int)block_w, (int)key_w);
      }
  }

  s_pet.initialized = true;
  syslog(LOG_INFO, "[%s] init OK (像素宠物状态环)\n", TAG);
  return 0;

fail:
  if (s_pet.screen) lv_obj_del(s_pet.screen);
  memset(&s_pet, 0, sizeof(s_pet));
  return -ENOMEM;
}

#endif /* CONFIG_AI_AGENT_PET_STATUS */

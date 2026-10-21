/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * particle_flow_view.c — 圆屏粒子流（M7 差异化页 ②，纯 LVGL）。
 *
 * 设计见 particle_flow_view.h。18 颗粒子在圆屏内做受控布朗流动，中央核心
 * 脉动；态切换改变粒子色 + 流向偏置（内聚 / 外散 / 旋转 / 混沌）+ 核心脉速。
 * 视觉与宠物脸+环刻意不同（抽象能量流），借同一套 agent 状态镜像语义。
 *
 * 物理：整数定点，位置以像素为单位，速度以 q8（256 = 1px/tick）为单位，
 * 每 tick (33ms≈30fps) 由 lv_timer 在 LVGL 线程推进；边界反弹用速度反转 +
 * 位置钳位（距离平方比较，无 sqrt）。无浮点 / math.h 依赖，跨核可移植。
 * 随机扰动用每粒子独立 LCG（确定性伪噪声，免 rand 依赖）。
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_PARTICLE_FLOW

#include "ui/particle_flow_view.h"
#include "ui/ui_palette.h"
#include "ui/lvgl_ui_channel.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

#include <lvgl/lvgl.h>

#define TAG "particle_view"

/* 屏幕切换淡入时长(ms)。硬 lv_screen_load() 的整帧 SPI flush 会与面板 60Hz
 * GRAM 扫描竞争,表现为从上到下的白底刷屏;改用淡入交叉渐变分散到多帧,
 * 把刷屏变成优雅的渐隐渐显。 */
#define SCREEN_FADE_MS 500

LV_FONT_DECLARE(lv_font_misans_16_cjk);

#define N_PARTICLES    18
#define PARTICLE_DIAM  7        /* px, odd so center pixel is exact */
#define CORE_DIAM      34
#define GUIDE_RING_DIAM 150
#define VEL_Q           8        /* velocity fixed-point: 256 = 1 px/tick */
#define ONE_PX          (1 << VEL_Q)
#define TICK_MS         33

/* State palette — HachimoDock 柔和低饱和状态色，语义同 pet_status_view。 */
static const uint32_t s_state_color[] = {
  UI_PAL_IDLE, UI_PAL_LISTEN, UI_PAL_THINK, UI_PAL_EXEC, UI_PAL_WAIT, UI_PAL_ERROR,
};
static const char *const s_state_text[] = {
  /* Mirrors pet_status_view's captions — and, critically, sticks to glyphs
   * present in the CP MiSans 16 subset (the old "粒子·" prefix used 粒/子,
   * which are NOT in the trimmed CP font and rendered as tofu on-device). */
  "待机", "聆听中", "思考中", "执行中", "等待确认", "出错了",
};

/* Per-state physics knobs:
 *   perturb  — LCG noise amplitude fed into velocity (q8 units)
 *   swirl    — tangential bias strength (rotation; +CW, -CCW)
 *   radial   — radial bias (+outward / -inward)
 *   damp     — velocity retention per tick (0..256) */
typedef struct {
  int32_t perturb;
  int32_t swirl;
  int32_t radial;
  int32_t damp;
} phys_t;

static const phys_t s_phys[] = {
  [PARTICLE_STATE_IDLE]      = { 10,   4,   0, 248 },  /* gentle drift */
  [PARTICLE_STATE_LISTENING] = { 16,  10, -18, 244 },  /* inward + slow swirl */
  [PARTICLE_STATE_THINKING] = { 20,  34,   0, 250 },  /* fast swirl */
  [PARTICLE_STATE_EXECUTING] = { 28,  -8,  26, 246 }, /* outward burst */
  [PARTICLE_STATE_WAITING]   = {  4,   2,   0, 220 },  /* near-frozen pulse */
  [PARTICLE_STATE_ERROR]     = { 90,   0,   0, 252 },  /* chaotic scatter */
};

struct particle_s {
  int32_t x, y;          /* position, pixels */
  int32_t vx, vy;        /* velocity, q8 (256 = 1px/tick) */
  uint32_t lcg;          /* per-particle pseudo-noise state */
  int32_t last_rx, last_ry;  /* last rendered top-left; skip set_pos if unchanged */
};

struct pf_state_s {
  bool initialized;
  bool visible;
  enum particle_state state;
  lv_obj_t *screen;
  lv_obj_t *guide_ring;  /* faint lv_arc, shared motif with pet */
  lv_obj_t *core;        /* central pulsing circle */
  lv_obj_t *particles[N_PARTICLES];
  lv_obj_t *label;
  lv_obj_t *status_dot;  /* colored dot paired with the state caption */
  lv_obj_t *prev_screen;
  struct particle_s p[N_PARTICLES];
  lv_timer_t *timer;
  uint32_t tick;
  /* display geometry (pixels) */
  int32_t cx, cy, R, R_sq;
};

static struct pf_state_s s_pf;

/* ---------- LCG (per-particle deterministic pseudo-noise) ---------- */

static inline int32_t lcg_next(uint32_t *s)
{
  *s = (*s * 1103515765u + 12345u) & 0x7fffffffu;
  return (int32_t)(*s >> 15) - 16384;   /* [-16384, 16383] */
}

/* ---------- physics step (runs in LVGL thread via timer) ---------- */

static void physics_step(void)
{
  const phys_t *ph = &s_phys[s_pf.state];
  int32_t cx = s_pf.cx, cy = s_pf.cy, R_sq = s_pf.R_sq;
  int32_t damp = ph->damp;
  int32_t perturb = ph->perturb;
  int32_t swirl = ph->swirl;
  int32_t radial = ph->radial;

  for (int i = 0; i < N_PARTICLES; i++)
    {
      struct particle_s *p = &s_pf.p[i];
      int32_t dx = p->x - cx;
      int32_t dy = p->y - cy;

      /* LCG noise → small velocity kick (perturb in q8; noise normalized to ~[-1,1] px). */
      int32_t nx = lcg_next(&p->lcg);
      int32_t ny = lcg_next(&p->lcg);
      p->vx += (nx * perturb) / 16384;
      p->vy += (ny * perturb) / 16384;

      /* Swirl: tangential bias ∝ radial offset (unnormalized → faster swirl
       * farther out, reads as rotation). tang = (-dy, dx). */
      p->vx += (-dy * swirl) / 64;
      p->vy += ( dx * swirl) / 64;

      /* Radial bias: outward (+) pulls particles to the rim, inward (-) sucks
       * them toward the core. */
      p->vx += (dx * radial) / 64;
      p->vy += (dy * radial) / 64;

      /* Damping. */
      p->vx = (p->vx * damp) / 256;
      p->vy = (p->vy * damp) / 256;

      /* Speed clamp so chaos stays bounded. */
      int32_t vmax = 3 * ONE_PX;
      if (p->vx >  vmax) p->vx =  vmax;
      if (p->vx < -vmax) p->vx = -vmax;
      if (p->vy >  vmax) p->vy =  vmax;
      if (p->vy < -vmax) p->vy = -vmax;

      /* Advance. */
      int32_t nxp = p->x + (p->vx >> VEL_Q);
      int32_t nyp = p->y + (p->vy >> VEL_Q);

      /* Round-bound bounce: if next step leaves the disc, reverse velocity
       * (cheap reflection) and don't move this tick. dist_sq in pixel units
       * fits in int32 (max ~217k). */
      int32_t ddx = nxp - cx;
      int32_t ddy = nyp - cy;
      int32_t dsq = ddx * ddx + ddy * ddy;
      if (dsq > R_sq)
        {
          p->vx = -p->vx;
          p->vy = -p->vy;
        }
      else
        {
          p->x = nxp;
          p->y = nyp;
        }

      /* Render — only invalidate when the integer pixel position actually
       * changes.  Particles move sub-pixel most ticks (q8 velocity), so an
       * unconditional set_pos would invalidate 18 regions every 33 ms even
       * when nothing visibly moved.  On flush paths that can't keep up
       * (qemu goldfish headless fb) the constant re-invalidation starves the
       * flush so the screen never reaches the framebuffer.  Skipping no-op
       * moves collapses invalidations to only real pixel crossings. */
      lv_obj_t *o = s_pf.particles[i];
      if (o)
        {
          int32_t rx = p->x - (PARTICLE_DIAM / 2);
          int32_t ry = p->y - (PARTICLE_DIAM / 2);
          if (rx != p->last_rx || ry != p->last_ry)
            {
              lv_obj_set_pos(o, rx, ry);
              p->last_rx = rx;
              p->last_ry = ry;
            }
        }
    }

  /* Core pulse: opacity breathing driven by tick (no lv_anim to keep the
   * pulsing phase-locked to the physics tick). */
  if (s_pf.core)
    {
      uint32_t t = s_pf.tick & 63;            /* 0..63 */
      int32_t tri = (t < 32) ? t : (63 - t);  /* triangle 0..31 */
      lv_opa_t opa = LV_OPA_60 + (tri * (LV_OPA_COVER - LV_OPA_60)) / 31;
      lv_obj_set_style_opa(s_pf.core, opa, 0);
    }

  s_pf.tick++;
}

static void timer_cb(lv_timer_t *t)
{
  (void)t;
  if (s_pf.initialized && s_pf.visible)
    physics_step();
}

/* ---------- async apply (runs in LVGL thread) ---------- */

static void apply_state_async_cb(void *data)
{
  enum particle_state st = (enum particle_state)(intptr_t)data;
  if (!s_pf.initialized) return;
  s_pf.state = st;
  uint32_t color = s_state_color[st];

  for (int i = 0; i < N_PARTICLES; i++)
    {
      if (s_pf.particles[i])
        {
          lv_obj_set_style_bg_color(s_pf.particles[i], lv_color_hex(color), 0);
        }
    }
  if (s_pf.core)
    {
      lv_obj_set_style_bg_color(s_pf.core, lv_color_hex(color), 0);
    }
  if (s_pf.label)
    {
      lv_label_set_text(s_pf.label, s_state_text[st]);
    }
  if (s_pf.status_dot)
    {
      lv_obj_set_style_bg_color(s_pf.status_dot, lv_color_hex(color), 0);
    }
  syslog(LOG_INFO, "[%s] state → %s\n", TAG, s_state_text[st]);
}

void particle_flow_view_set_state(enum particle_state state)
{
  if ((int)state < 0 || (int)state > PARTICLE_STATE_ERROR) return;
  lvgl_ui_async_call(apply_state_async_cb, (void *)(intptr_t)state);
}

/* ---------- screen show / hide ---------- */

static void show_async_cb(void *data)
{
  (void)data;
  if (!s_pf.initialized || !s_pf.screen || s_pf.visible) return;
  s_pf.prev_screen = lv_screen_active();
  lv_screen_load_anim(s_pf.screen, LV_SCR_LOAD_ANIM_FADE_IN,
                      SCREEN_FADE_MS, 0, false);
  /* lv_screen_load already invalidates the full screen and schedules a redraw;
   * the explicit invalidate + lv_refr_now below was a qemu-goldfish-headless
   * lazy-flush workaround that is unnecessary on the real CP /dev/lcd0 panel,
   * where it caused a full-frame (top-to-bottom) white flicker on every idle
   * pet↔particle rotate.  Real hardware renders screen switches fine without
   * the extra synchronous refresh, so we drop it to stop the standby flicker. */
  s_pf.visible = true;
  syslog(LOG_INFO, "[%s] particle screen shown\n", TAG);
}

void particle_flow_view_show(void)
{
  if (!s_pf.initialized) return;
  lvgl_ui_async_call(show_async_cb, NULL);
}

static void hide_async_cb(void *data)
{
  (void)data;
  if (!s_pf.initialized || !s_pf.visible) return;
  if (s_pf.prev_screen)
    lv_screen_load_anim(s_pf.prev_screen, LV_SCR_LOAD_ANIM_FADE_IN,
                        SCREEN_FADE_MS, 0, false);
  s_pf.visible = false;
  syslog(LOG_INFO, "[%s] particle screen hidden\n", TAG);
}

void particle_flow_view_hide(void)
{
  if (!s_pf.initialized) return;
  lvgl_ui_async_call(hide_async_cb, NULL);
}

bool particle_flow_view_is_visible(void)
{
  return s_pf.visible;
}

/* ---------- init ---------- */

static lv_obj_t *make_circle(lv_obj_t *parent, int32_t diam,
                              uint32_t color, lv_opa_t opa)
{
  lv_obj_t *o = lv_obj_create(parent);
  if (!o) return NULL;
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, diam, diam);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_center(o);
  return o;
}

int particle_flow_view_init(void)
{
  if (s_pf.initialized) return 0;

  memset(&s_pf, 0, sizeof(s_pf));
  s_pf.state = PARTICLE_STATE_IDLE;

  s_pf.screen = lv_obj_create(NULL);
  if (!s_pf.screen) return -ENOMEM;
  lv_obj_set_style_bg_color(s_pf.screen, lv_color_hex(UI_PAL_BG), 0);
  lv_obj_set_style_bg_opa(s_pf.screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_font(s_pf.screen, &lv_font_misans_16_cjk, 0);

  /* Display geometry: center + round bound. */
  lv_display_t *disp = lv_display_get_default();
  int32_t h = lv_display_get_horizontal_resolution(disp);
  int32_t v = lv_display_get_vertical_resolution(disp);
  s_pf.cx = h / 2;
  s_pf.cy = v / 2;
  int32_t half = (h < v ? h : v) / 2;
  /* Inscribe the particle travel bound INSIDE the round face: subtract the
   * particle radius (PARTICLE_DIAM/2) so a particle centred at the bound has
   * its outer edge on the circle, never spilling into the bezel. */
  s_pf.R = (half > 8) ? (half - 8 - PARTICLE_DIAM / 2) : 8;
  s_pf.R_sq = s_pf.R * s_pf.R;

  /* Faint guide ring (shared motif with pet_status_view): a barely-there dark
   * boundary circle.  The particles + core already carry the state color, so
   * the ring itself stays subtle rather than competing with them. */
  s_pf.guide_ring = lv_arc_create(s_pf.screen);
  if (s_pf.guide_ring)
    {
      lv_obj_set_size(s_pf.guide_ring, GUIDE_RING_DIAM, GUIDE_RING_DIAM);
      lv_obj_center(s_pf.guide_ring);
      lv_arc_set_bg_angles(s_pf.guide_ring, 0, 360);
      lv_arc_set_angles(s_pf.guide_ring, 0, 360);
      lv_obj_set_style_arc_width(s_pf.guide_ring, 2, LV_PART_MAIN);
      lv_obj_set_style_arc_width(s_pf.guide_ring, 0, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(s_pf.guide_ring, lv_color_hex(UI_PAL_TRACK), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(s_pf.guide_ring, LV_OPA_TRANSP, LV_PART_KNOB);
      lv_obj_remove_flag(s_pf.guide_ring, LV_OBJ_FLAG_CLICKABLE);
    }

  /* Central pulsing core. */
  s_pf.core = make_circle(s_pf.screen, CORE_DIAM, UI_PAL_IDLE, LV_OPA_80);

  /* Particles — scattered initially within the disc. */
  uint32_t seed = 0x1234abcd;
  for (int i = 0; i < N_PARTICLES; i++)
    {
      /* place around a ring with jitter so they don't overlap the core */
      int32_t ang = (i * 360 / N_PARTICLES) % 360;
      /* cheap sin/cos via a 4-bin triangle so no math.h: */
      int32_t qx = (ang < 90) ? ang : (ang < 270) ? (180 - ang) : (ang - 360);
      int32_t qy = ((ang + 270) % 360);
      qy = (qy < 90) ? qy : (qy < 270) ? (180 - qy) : (qy - 360);
      /* qx, qy ∈ [-90, 90] → normalize to [-1, 1] */
      int32_t rx = (qx * 0x7fff) / 90;
      int32_t ry = (qy * 0x7fff) / 90;
      int32_t radius = s_pf.R * 2 / 3;
      int32_t jitter = (lcg_next(&seed) * radius) / 16384;
      int32_t rr = radius + (jitter / 4);
      if (rr < 6) rr = 6;
      if (rr > s_pf.R - 2) rr = s_pf.R - 2;

      s_pf.p[i].x = s_pf.cx + (int32_t)(((int64_t)rx * rr) / 0x7fff);
      s_pf.p[i].y = s_pf.cy + (int32_t)(((int64_t)ry * rr) / 0x7fff);
      s_pf.p[i].vx = lcg_next(&seed) / 8;   /* small initial velocity */
      s_pf.p[i].vy = lcg_next(&seed) / 8;
      s_pf.p[i].lcg = seed;

      /* Soft particles — 80% opacity recedes them into the near-black field
       * (HachimoDock softness) instead of hard neon dots. */
      s_pf.particles[i] = make_circle(s_pf.screen, PARTICLE_DIAM,
                                       UI_PAL_IDLE, LV_OPA_80);
      if (s_pf.particles[i])
        {
          int32_t prx = s_pf.p[i].x - (PARTICLE_DIAM / 2);
          int32_t pry = s_pf.p[i].y - (PARTICLE_DIAM / 2);
          lv_obj_set_pos(s_pf.particles[i], prx, pry);
          s_pf.p[i].last_rx = prx;
          s_pf.p[i].last_ry = pry;
        }
    }

  /* State caption row (dot + text), mirroring pet_status_view's caption so the
   * particle page's status text is anchored to a UI block rather than floating
   * as a bare label.  Placed low inside the circle, clear of the core/ring. */
  {
    lv_obj_t *caption = lv_obj_create(s_pf.screen);
    if (caption)
      {
        lv_obj_remove_flag(caption, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(caption, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(caption, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(caption, 0, 0);
        lv_obj_set_style_pad_all(caption, 0, 0);
        lv_obj_set_flex_flow(caption, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(caption, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(caption, 6, 0);
        /* 88 matches pet_status_view's PET_LABEL_DY: the caption (dot+text)
         * is 20px tall (MiSans line_height=20), so its top is 88-10=78 below
         * center → y=226, clearing the 150px guide ring bottom (y=223) by 3px
         * instead of overlapping. */
        lv_obj_align(caption, LV_ALIGN_CENTER, 0, 88);

        s_pf.status_dot = lv_obj_create(caption);
        if (s_pf.status_dot)
          {
            lv_obj_remove_flag(s_pf.status_dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(s_pf.status_dot, 8, 8);
            lv_obj_set_style_radius(s_pf.status_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(s_pf.status_dot, lv_color_hex(UI_PAL_IDLE), 0);
            lv_obj_set_style_bg_opa(s_pf.status_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_pf.status_dot, 0, 0);
            lv_obj_set_style_pad_all(s_pf.status_dot, 0, 0);
          }

        s_pf.label = lv_label_create(caption);
        if (s_pf.label)
          {
            lv_label_set_text(s_pf.label, s_state_text[PARTICLE_STATE_IDLE]);
            lv_obj_set_style_text_color(s_pf.label, lv_color_hex(UI_PAL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(s_pf.label, &lv_font_misans_16_cjk, 0);
          }
      }
  }

  /* Physics timer — runs on the LVGL thread, so widget mutation is safe. */
  s_pf.timer = lv_timer_create(timer_cb, TICK_MS, NULL);
  if (s_pf.timer)
    {
      lv_timer_set_repeat_count(s_pf.timer, -1);
    }

  s_pf.initialized = true;
  syslog(LOG_INFO, "[%s] init OK (圆屏粒子流, %d 粒子, disp %dx%d R=%d)\n",
         TAG, N_PARTICLES, (int)h, (int)v, (int)s_pf.R);
  return 0;
}

#endif /* CONFIG_AI_AGENT_PARTICLE_FLOW */

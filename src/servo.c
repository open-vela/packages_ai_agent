/*
 * servo.c — AI-VOX3 舵机控制模块 (v102)
 *
 * 依赖 NuttX PWM 接口 (include/nuttx/timers/pwm.h):
 *   PWMIOC_SETCHARACTERISTICS _PWMIOC(1)  -> 设置 frequency + duty(ub16)
 *   PWMIOC_START            _PWMIOC(3)    -> 启动输出
 *   PWMIOC_STOP             _PWMIOC(4)    -> 停止
 * struct pwm_info_s { uint32_t frequency; ub16_t duty; }  (单通道)
 *   duty 为 ub16 小数: 65535 ≈ 100% 占空比
 *
 * 编译: 在 packages/ai_agent/Make.defs 的 CSRCS 中加入 servo.c
 */
#include <nuttx/config.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <syslog.h>

#include <fixedmath.h>           /* ub16_t (also pulled in by pwm.h) */
#include <nuttx/timers/pwm.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "servo.h"

/* ---- 内部状态 ---- */
struct servo_ch_s
{
  int         fd;
  const char *path;
  const char *name;
  int         invert;
  int         idx;               /* [V1036] index into g_limits[]   */
  int         cur;               /* 当前角度 (缓存，用于动画) */
};
static struct servo_ch_s g_pan  = { -1, SERVO_PAN_DEV,  "PAN",  SERVO_PAN_INVERT, SERVO_ID_PAN, 90 };
static struct servo_ch_s g_tilt = { -1, SERVO_TILT_DEV, "TILT", SERVO_TILT_INVERT, SERVO_ID_TILT, 90 };

/* [W962] Motion state per channel.
 *
 * target     where servo_move() wants the horn to end up
 * min/max    install-space mechanical limits
 * deadband   smallest target change worth pursuing
 * speed_dps  angular rate cap; the keep-alive thread turns this into a
 *            per-tick budget so a large jump becomes a sweep instead of
 *            a step the servo has to catch up with (it cannot).
 */
struct servo_limit_s
{
  int min;
  int max;
  int target;
  int deadband;
  int speed_dps;
};

#define SERVO_DEFAULT_DEADBAND  2      /* degrees                     */
#define SERVO_DEFAULT_SPEED_DPS 180    /* degrees per second          */

static struct servo_limit_s g_limits[2] =
{
  { 0, 180, 90, SERVO_DEFAULT_DEADBAND, SERVO_DEFAULT_SPEED_DPS },  /* PAN */
  { 0, 180, 90, SERVO_DEFAULT_DEADBAND, SERVO_DEFAULT_SPEED_DPS }   /* TILT */
};

/* Forward declarations: the helpers below are used from the keep-alive
 * loop, which sits earlier in this file than their definitions. */
static int servo_step_toward_target(int id);
static int lim_target_of(int id);
static int absi_int(int v);

/* [V502] keep-alive state.  Declared up here because servo_set_ch()
 * marks activity and it sits above the keep-alive section. */
static volatile int g_keepalive_run;
static volatile int g_keepalive_recent;

/* [V516-1] Every ioctl() on a /dev/pwm* fd is serialised through this.
 *
 * Why it is needed: the keep-alive thread, servo_self_test(),
 * companion_mood.c (nod/shake) and companion_face.c (tracking) all drive
 * the SAME two descriptors.  PWMIOC_SETCHARACTERISTICS + PWMIOC_START are
 * two separate ioctls, so an interleaving like
 *
 *     thread A: SETCHARACTERISTICS(duty=1500us)
 *     thread B: START
 *     thread A: (returns)
 *
 * is legal at the syscall level but leaves the channel with a duty that
 * nobody re-issued after START.  Symptom: the axis drifts or stops
 * responding under concurrent motion.
 */
static pthread_mutex_t g_pwm_lock = PTHREAD_MUTEX_INITIALIZER;

/* [V516-3] Remembered angle per channel, owned by the keep-alive thread. */
static int g_ka_sent_pan  = 90;
static int g_ka_sent_tilt = 90;

/* 角度 -> 脉宽(us): 0°=1000us, 90°=1500us, 180°=2000us */
static uint32_t angle_to_pulse_us(int angle)
{
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  return (uint32_t)1000 + (uint32_t)angle * 1000u / 180u;
}

/* 脉宽 -> ub16 占空比 (period=20000us, ub16: 65536=100%)
 *
 * [V516-2] The cast used to be able to produce 65536, which does not fit
 * in ub16_t (max 65535) and wraps to 0.  It only bites when pulse_us >=
 * SERVO_PERIOD_US, i.e. a 100%% duty request -- but a wrap to 0 means "no
 * pulse at all", and a servo with no pulses is exactly the failure we are
 * chasing.  Compute in 32-bit space and clamp instead.
 */
static ub16_t pulse_to_duty_ub16(uint32_t pulse_us)
{
  uint32_t duty;

  if (pulse_us >= SERVO_PERIOD_US)
    {
      return 65535;               /* 100%, clamped into ub16_t */
    }

  duty = (uint32_t)(((uint64_t)pulse_us * 65535u) /
                     (uint64_t)SERVO_PERIOD_US);
  if (duty > 65535u)
    {
      duty = 65535u;
    }

  return (ub16_t)duty;
}

/* 角度 -> 实际下发角度 (考虑 INVERT) */
static int apply_invert(struct servo_ch_s *ch, int angle)
{
  return ch->invert ? (180 - angle) : angle;
}

/* 设置单通道角度 */
static int servo_set_ch(struct servo_ch_s *ch, int angle)
{
  struct pwm_info_s info;
  int ret;
  /* [W962] Install-space limit, applied to the LOGICAL angle before the
   * invert mapping, so min/max mean the same thing to the caller no
   * matter which way the horn is mounted.  The physical 0..180 clamp is
   * still applied further down in angle_to_pulse_us(). */
  {
    struct servo_limit_s *lim = &g_limits[ch->idx];
    int clamped;

    clamped = (angle < lim->min) ? lim->min
                                 : ((angle > lim->max) ? lim->max : angle);

    if (clamped != angle)
      {
        syslog(LOG_WARNING,
               "servo: %s angle=%d clamped into limit %d..%d -> %d\n",
               ch->name, angle, lim->min, lim->max, clamped);
        angle = clamped;
      }
  }

  int real = apply_invert(ch, angle);

  if (ch->fd < 0)
    {
      syslog(LOG_ERR, "servo: %s not opened, skip angle=%d\n", ch->name, angle);
      return -ENODEV;
    }

  info.frequency = SERVO_FREQ_HZ;
  info.duty = pulse_to_duty_ub16(angle_to_pulse_us(real));

  /* [V1030] Proof a move reaches the PWM device.
   *
   * servo_move()/servo_move_pan() only record a TARGET; the write
   * happens on the keep-alive tick via servo_step_toward_target() ->
   * servo_set_ch().  This is therefore the only place that separates
   * "target accepted" from "duty written".  duty is the ub16 fraction
   * of the 20 ms period, so ~4915 is 1.5 ms, i.e. neutral.  1 in 8. */
  {
    static unsigned int s_v1030_writes;
    s_v1030_writes++;
    if ((s_v1030_writes & 7u) == 1u)
      {
        syslog(LOG_INFO, "[V1030] servo ch=%d angle=%d real=%d -> duty=%u\n",
               ch->idx, angle, real,
               (unsigned int)info.duty);
      }
  }

  /* [V516-1] one ioctl pair, one lock */
  pthread_mutex_lock(&g_pwm_lock);
  ret = ioctl(ch->fd, PWMIOC_SETCHARACTERISTICS,
              (unsigned long)((uintptr_t)&info));
  if (ret == 0)
    {
      /* Re-START after changing duty, so the new width takes effect even
       * if somebody stopped the channel in the meantime. */
      ioctl(ch->fd, PWMIOC_START, 0);
      ch->cur = angle;
      /* [V1036] Adopt the angle as the motion target.
       *
       * Two places used to disagree about where the horn belongs:
       * ch->cur (written here) and g_limits[].target (written only by
       * servo_move()).  Any direct write - servo_pan(), servo_center(),
       * servo_nod(), servo_snap(), and companion_face_hold() - moved
       * the horn but left the target stale, so the keep-alive tick
       * (200 ms, budget 36 deg) hauled it straight back.
       *
       * Writes that are only a step of a longer move, or a re-issue
       * of the current angle, go through
       * servo_set_ch_keep_target() so they cannot cancel it. */
      g_limits[ch->idx].target = angle;
    }
  pthread_mutex_unlock(&g_pwm_lock);

  if (ret < 0)
    {
      int e = errno;
      syslog(LOG_ERR, "servo: %s set angle=%d (real=%d) failed errno=%d\n",
             ch->name, angle, real, e);
      return -e;
    }

  g_keepalive_recent = 1;
  return OK;
}

/* [V1036] Write an angle WITHOUT adopting it as the motion target.
 *
 * For the keep-alive paths that only re-issue the current angle or
 * take one budgeted step of a longer move.  Adopting there would
 * truncate the move: a 90 -> 109 sweep with a 36 deg budget would
 * set target to the intermediate value and then decide it had
 * arrived. */
static int servo_set_ch_keep_target(struct servo_ch_s *ch, int angle)
{
  int idx = ch->idx;
  int saved = g_limits[idx].target;
  int r = servo_set_ch(ch, angle);

  g_limits[idx].target = saved;
  return r;
}

int servo_init(void)
{
  int opened = 0;

  if (g_pan.fd < 0)
    {
      g_pan.fd = open(SERVO_PAN_DEV, O_WRONLY);
      if (g_pan.fd < 0)
        syslog(LOG_ERR, "servo: open %s failed errno=%d\n", SERVO_PAN_DEV, errno);
      else
        opened++;
    }
  if (g_tilt.fd < 0)
    {
      g_tilt.fd = open(SERVO_TILT_DEV, O_WRONLY);
      if (g_tilt.fd < 0)
        syslog(LOG_ERR, "servo: open %s failed errno=%d\n", SERVO_TILT_DEV, errno);
      else
        opened++;
    }

  if (opened == 0)
    {
      syslog(LOG_ERR, "servo: BOTH channels failed to open -> 舵机不可用(不影响语音)\n");
      return -ENODEV;
    }

  /* START 两个通道 */
  pthread_mutex_lock(&g_pwm_lock);
  if (g_pan.fd >= 0)  ioctl(g_pan.fd, PWMIOC_START, 0);
  if (g_tilt.fd >= 0) ioctl(g_tilt.fd, PWMIOC_START, 0);
  pthread_mutex_unlock(&g_pwm_lock);

  /* 上电回中位，避免舵机猛甩 */

  /* [W962] Install the documented defaults.  Doing it here rather than
   * as static initialisers means servo_set_limits() is really linked in
   * and exercised at boot, and it gives one obvious place to change the
   * safe envelope for a given mechanical build. */
  servo_set_limits(SERVO_ID_PAN,  SERVO_DEFAULT_PAN_MIN,
                   SERVO_DEFAULT_PAN_MAX);
  servo_set_limits(SERVO_ID_TILT, SERVO_DEFAULT_TILT_MIN,
                   SERVO_DEFAULT_TILT_MAX);
  servo_set_deadband(SERVO_ID_PAN,  SERVO_DEFAULT_DEADBAND);
  servo_set_deadband(SERVO_ID_TILT, SERVO_DEFAULT_DEADBAND);
  servo_set_speed(SERVO_ID_PAN,  SERVO_DEFAULT_SPEED_DPS);
  servo_set_speed(SERVO_ID_TILT, SERVO_DEFAULT_SPEED_DPS);
  servo_center();

  syslog(LOG_INFO, "servo: init ok pan_fd=%d tilt_fd=%d (PAN=GPIO2 TILT=GPIO1)\n",
         g_pan.fd, g_tilt.fd);
  return OK;
}

void servo_deinit(void)
{
  pthread_mutex_lock(&g_pwm_lock);
  if (g_pan.fd >= 0)  { ioctl(g_pan.fd, PWMIOC_STOP, 0); close(g_pan.fd);  g_pan.fd = -1; }
  if (g_tilt.fd >= 0) { ioctl(g_tilt.fd, PWMIOC_STOP, 0); close(g_tilt.fd); g_tilt.fd = -1; }
  pthread_mutex_unlock(&g_pwm_lock);
}

int servo_pan(int angle)
{
  return servo_set_ch(&g_pan, angle);
}

int servo_tilt(int angle)
{
  return servo_set_ch(&g_tilt, angle);
}

int servo_center(void)
{
  int r1 = servo_set_ch(&g_pan, 90);
  int r2 = servo_set_ch(&g_tilt, 90);
  return (r1 < 0) ? r1 : r2;
}

/* 点头: TILT 90->115->65->90 (小幅度快速) */
int servo_nod(void)
{
  servo_set_ch(&g_tilt, 90);
  usleep(120000);
  servo_set_ch(&g_tilt, 115);
  usleep(180000);
  servo_set_ch(&g_tilt, 65);
  usleep(180000);
  servo_set_ch(&g_tilt, 90);
  return OK;
}

/* 摇头: PAN 90->120->60->90 (小幅度快速) */
int servo_shake(void)
{
  servo_set_ch(&g_pan, 90);
  usleep(120000);
  servo_set_ch(&g_pan, 120);
  usleep(180000);
  servo_set_ch(&g_pan, 60);
  usleep(180000);
  servo_set_ch(&g_pan, 90);
  return OK;
}

/* 上电自检: 两轴各扫 0->180->90，肉眼即可确认舵机在线 */
int servo_self_test(void)
{
  /* [V1263] 原来是 0->180 扫全行程：会撞机械限位（齿轮顶死、嗡嗡响），
   * 而且开机第一眼就是"猛低一下头再猛抬起来"——很容易被当成故障。
   * 改成中位附近 +/-20 度的小幅摆动，验证舵机在线足够了。 */
  syslog(LOG_INFO, "servo: self-test start (small sweep)\n");
  servo_set_ch(&g_pan, 70);   usleep(300000);
  servo_set_ch(&g_pan, 110);  usleep(300000);
  servo_set_ch(&g_pan, 90);   usleep(200000);
  servo_set_ch(&g_tilt, 70);  usleep(300000);
  servo_set_ch(&g_tilt, 110); usleep(300000);
  servo_set_ch(&g_tilt, 90);  usleep(200000);
  syslog(LOG_INFO, "servo: self-test done\n");
  return OK;
}

/* ------------------------------------------------------------------------
 * [V502] 舵机保活
 *
 * 模拟舵机靠连续的 50Hz 脉冲维持位置。servo_init() 只 START 一次，之后
 * 如果没人再下发角度，机械负载就能把轴推偏；某些情况下通道还会被别的
 * 代码 PWMIOC_STOP 掉。保活线程做两件事：
 *
 *   1) 周期性重新 PWMIOC_START，防止通道被别人停掉；
 *   2) 用当前已知的角度重发一遍 duty，保持脉冲流连续。
 *
 * 注意：**不会定时归中**。人脸跟随与 nod/shake 动画会随时改角度，定时
 * 归中会和它们打架。只有在"请求角度"与"实际最后写入角度"偏差超过
 * SERVO_KEEPALIVE_DRIFT_DEG 时才重新写一次，用于纠正偏差。
 */
#define SERVO_KEEPALIVE_PERIOD_MS   100   /* [V1275] 200ms 一拍 => 最高 200ms 才动一次，跟手迟滞明显；100ms 减半 */
#define SERVO_KEEPALIVE_DRIFT_DEG   2


/* 标记"刚有人动过舵机"，动画/跟随调用后由 servo_set_ch 自动置位。 */
void servo_mark_activity(void)
{
  g_keepalive_recent = 1;
}

static void *servo_keepalive_task(void *arg)
{
  (void)arg;

  g_ka_sent_pan  = g_pan.cur;
  g_ka_sent_tilt = g_tilt.cur;

  syslog(LOG_INFO, "servo: keep-alive task started (%d ms period)\n",
         SERVO_KEEPALIVE_PERIOD_MS);

  while (g_keepalive_run)
    {
      /* [V516-3] The old test was `g_pan.cur != last_pan` with last_pan
       * captured a cycle earlier.  servo_set_ch() writes ch->cur straight
       * away, so after ANY tracking move the test was true on the next
       * tick and we re-issued the PREVIOUS angle -- dragging the axis back
       * 200 ms behind the tracker.  Worse, servo_set_ch() then set
       * g_keepalive_recent, so the next tick did it again: a permanent 5 Hz
       * tug-of-war between keep-alive and face tracking.
       *
       * The activity flag is the right signal: servo_set_ch() sets it
       * whenever anybody moves an axis, and only then does keep-alive adopt
       * the new angle as its baseline. */
      if (g_keepalive_recent)
        {
          g_ka_sent_pan  = g_pan.cur;
          g_ka_sent_tilt = g_tilt.cur;
          g_keepalive_recent = 0;
        }

      /* Re-START both channels (single lock, both fds).  Doing it every
       * cycle is the whole point: a servo keeps position only while the
       * 50 Hz pulse train continues, and another subsystem may have
       * issued PWMIOC_STOP in the meantime. */
      pthread_mutex_lock(&g_pwm_lock);
      if (g_pan.fd >= 0)
        {
          ioctl(g_pan.fd, PWMIOC_START, 0);
        }
      if (g_tilt.fd >= 0)
        {
          ioctl(g_tilt.fd, PWMIOC_START, 0);
        }
      pthread_mutex_unlock(&g_pwm_lock);

      /* Drift correction only: if a channel's cached angle no longer
       * matches what we last sent (e.g. servo_center() ran behind our
       * back), re-issue the current angle -- never an older one. */
      if (g_pan.fd >= 0 && g_pan.cur != g_ka_sent_pan)
        {
          servo_set_ch_keep_target(&g_pan, g_pan.cur);
          g_ka_sent_pan = g_pan.cur;
        }

      if (g_tilt.fd >= 0 && g_tilt.cur != g_ka_sent_tilt)
        {
          servo_set_ch_keep_target(&g_tilt, g_tilt.cur);
          g_ka_sent_tilt = g_tilt.cur;
        }


      /* [W962] One budgeted step toward the move target, per channel.
       * Doing it here keeps servo_move() non-blocking and keeps ALL
       * motion funnelled through the single writer the PWM mutex exists
       * to protect. */
      servo_step_toward_target(SERVO_ID_PAN);
      servo_step_toward_target(SERVO_ID_TILT);
      usleep(SERVO_KEEPALIVE_PERIOD_MS * 1000);
    }

  syslog(LOG_INFO, "servo: keep-alive task exiting\n");
  return NULL;
}

/* ------------------------------------------------------------------------
 * [W962] Smooth, limited, debounced motion.
 *
 * One step per keep-alive tick.  The deadband compares against the
 * TARGET rather than the current angle: tracking keeps nudging the
 * target, and a sub-degree correction is invisible but costs a real PWM
 * rewrite every tick.
 *
 * Never returns an error: a missing channel is a configuration issue the
 * caller cannot fix, and failing here would abort the keep-alive loop
 * that keeps the servos powered at all.
 */
static int absi_int(int v)
{
  return (v < 0) ? -v : v;
}

static int servo_id_index(int id)
{
  if (id == SERVO_ID_PAN)
    {
      return 0;
    }

  if (id == SERVO_ID_TILT)
    {
      return 1;
    }

  return -1;
}

static struct servo_ch_s *channel_of(int id)
{
  if (id == SERVO_ID_PAN)
    {
      return &g_pan;
    }

  if (id == SERVO_ID_TILT)
    {
      return &g_tilt;
    }

  return NULL;
}

static int lim_target_of(int id)
{
  int idx = servo_id_index(id);

  return (idx < 0) ? 0 : g_limits[idx].target;
}

static int clamp_limits(const struct servo_limit_s *lim, int angle)
{
  if (angle < lim->min)
    {
      return lim->min;
    }

  if (angle > lim->max)
    {
      return lim->max;
    }

  return angle;
}

static int servo_step_toward_target(int id)
{
  struct servo_ch_s *ch;
  struct servo_limit_s *lim;
  int idx = servo_id_index(id);
  int budget;
  int delta;
  int step;

  if (idx < 0)
    {
      return OK;
    }

  ch  = channel_of(id);
  lim = &g_limits[idx];

  if (ch == NULL || ch->fd < 0)
    {
      return OK;
    }

  lim->target = clamp_limits(lim, lim->target);

  if (lim->target == ch->cur)
    {
      return OK;
    }

  if (absi_int(lim->target - ch->cur) < lim->deadband)
    {
      return OK;
    }

  /* deg/s * seconds-per-tick, floor of 1 so an axis can never stall part
   * way to its target. */
  budget = (lim->speed_dps * SERVO_KEEPALIVE_PERIOD_MS) / 1000;
  if (budget < 1)
    {
      budget = 1;
    }

  delta = lim->target - ch->cur;
  step  = (delta > 0) ? ((delta < budget) ? delta : budget)
                      : ((delta > -budget) ? delta : -budget);

  /* [V1036] one budgeted step of a longer move - keep the target */
  return servo_set_ch_keep_target(ch, ch->cur + step);
}

/* ---- public id-based API ---- */

int servo_set_limits(int id, int min_deg, int max_deg)
{
  int idx = servo_id_index(id);

  if (idx < 0)
    {
      return -EINVAL;
    }

  if (min_deg < 0)
    {
      min_deg = 0;
    }

  if (max_deg > 180)
    {
      max_deg = 180;
    }

  if (min_deg > max_deg)
    {
      syslog(LOG_ERR, "servo: bad limits %d..%d for id=%d\n",
             min_deg, max_deg, id);
      return -EINVAL;
    }

  g_limits[idx].min    = min_deg;
  g_limits[idx].max    = max_deg;
  g_limits[idx].target = clamp_limits(&g_limits[idx], g_limits[idx].target);

  return OK;
}

int servo_get_limits(int id, int *min_deg, int *max_deg)
{
  int idx = servo_id_index(id);

  if (idx < 0)
    {
      return -EINVAL;
    }

  if (min_deg != NULL)
    {
      *min_deg = g_limits[idx].min;
    }

  if (max_deg != NULL)
    {
      *max_deg = g_limits[idx].max;
    }

  return OK;
}

int servo_set_deadband(int id, int deg)
{
  int idx = servo_id_index(id);

  if (idx < 0)
    {
      return -EINVAL;
    }

  if (deg < 0)
    {
      deg = 0;
    }

  if (deg > 45)
    {
      deg = 45;
    }

  g_limits[idx].deadband = deg;
  return OK;
}

int servo_set_speed(int id, int deg_per_sec)
{
  int idx = servo_id_index(id);

  if (idx < 0)
    {
      return -EINVAL;
    }

  if (deg_per_sec < 1)
    {
      deg_per_sec = 1;
    }

  if (deg_per_sec > 600)
    {
      deg_per_sec = 600;
    }

  g_limits[idx].speed_dps = deg_per_sec;
  return OK;
}

int servo_move(int id, int angle)
{
  int idx = servo_id_index(id);

  if (idx < 0)
    {
      return -EINVAL;
    }

  g_limits[idx].target = clamp_limits(&g_limits[idx], angle);
  return OK;
}

int servo_move_pan(int angle)
{
  return servo_move(SERVO_ID_PAN, angle);
}

int servo_move_tilt(int angle)
{
  return servo_move(SERVO_ID_TILT, angle);
}

int servo_snap(int id, int angle)
{
  struct servo_ch_s *ch = channel_of(id);
  int idx = servo_id_index(id);

  if (ch == NULL || idx < 0)
    {
      return -EINVAL;
    }

  return servo_set_ch(ch, clamp_limits(&g_limits[idx], angle));
}

int servo_get_angle(int id)
{
  struct servo_ch_s *ch = channel_of(id);

  return (ch == NULL) ? -EINVAL : ch->cur;
}

int servo_get_target(int id)
{
  int idx = servo_id_index(id);

  return (idx < 0) ? -EINVAL : g_limits[idx].target;
}

int servo_is_moving(int id)
{
  struct servo_ch_s *ch = channel_of(id);

  if (ch == NULL || servo_id_index(id) < 0)
    {
      return 0;
    }

  return (lim_target_of(id) != ch->cur) ? 1 : 0;
}

int servo_keepalive_start(void)
{
  if (g_pan.fd < 0 && g_tilt.fd < 0)
    {
      syslog(LOG_ERR, "servo: keep-alive not started, no channel open\n");
      return -ENODEV;
    }

  pthread_attr_t attr;
  pthread_t th;

  g_keepalive_run = 1;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 2048);

  if (pthread_create(&th, &attr, servo_keepalive_task, NULL) != 0)
    {
      int e = errno;
      syslog(LOG_ERR, "servo: keep-alive pthread_create failed errno=%d\n",
             e);
      g_keepalive_run = 0;
      return -e;
    }

  pthread_attr_destroy(&attr);

  syslog(LOG_INFO, "servo: keep-alive thread started\n");
  return OK;
}

void servo_keepalive_stop(void)
{
  g_keepalive_run = 0;
}

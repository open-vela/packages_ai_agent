/*
 * servo.h — AI-VOX3 舵机控制模块 (v102)
 *
 * 板卡: emakefun AI-VOX3 / ESP32-S3-R8
 * 舵机: PAN = GPIO1, TILT = GPIO2  [V517 corrected]  (NuttX 板级 esp32s3_bringup.c 映射:
 *        /dev/pwm0 -> GPIO3 = PAN, /dev/pwm1 -> GPIO4 = TILT;
 *        Arduino 参考固件用 42/43，但本 NuttX 固件已映射到 3/4，以本文件为准)
 * 驱动: NuttX ESP32-S3 LEDC PWM -> /dev/pwm0 (PAN), /dev/pwm1 (TILT)
 *       defconfig 已含:
 *         CONFIG_ESP32S3_LEDC=y
 *         CONFIG_ESP32S3_LEDC_CHANNEL0_PIN=1    (pwm0 = PAN)
 *         CONFIG_ESP32S3_LEDC_CHANNEL1_PIN=2    (pwm1 = TILT)
 *         CONFIG_PWM=y
 *
 * 标准模拟舵机: 50Hz, 脉宽 1000us(0°) .. 2000us(180°), 中位 1500us(90°)
 */
#ifndef __PACKAGES_AI_AGENT_SRC_SERVO_H
#define __PACKAGES_AI_AGENT_SRC_SERVO_H

#ifdef __cplusplus
extern "C"
{
#endif

/* 设备节点: pwm0=PAN(左右), pwm1=TILT(上下) */
#define SERVO_PAN_DEV   "/dev/pwm0"
#define SERVO_TILT_DEV  "/dev/pwm1"

#define SERVO_FREQ_HZ     50        /* 标准模拟舵机频率 */
#define SERVO_PERIOD_US   20000     /* 50Hz 周期 = 20ms */

/* 角度范围 0..180，中位 90。
 * 若实际安装方向相反，把对应 INVERT 改为 1 即可（无需改硬件）。*/
#define SERVO_PAN_INVERT  0
#define SERVO_TILT_INVERT 0

/* ---- 对外 API ---- */
int  servo_init(void);            /* 打开设备、START、回中位。失败仅记日志不致命 */
void servo_deinit(void);

/* ---- [W962] Reusable angle API ----------------------------------
 * servo_pan()/servo_tilt() still work and still write immediately.
 * Everything new should use the id-based pair below, because the face
 * tracker, the mood animations and any future skill all need the same
 * three guarantees: never exceed the mechanical limits, never chase a
 * change too small to see, and never slew faster than the servo can
 * follow.
 */

#define SERVO_ID_PAN   0
#define SERVO_ID_TILT  1

/* Mechanical limits, clamped on every write.  Default 0..180 keeps the
 * behaviour identical to before; narrow them for a constrained mount. */
/* [W962] Default install limits.
 *
 * A 180 degree hobby servo is specified 0..180 but its gears bind at
 * the physical end stops; driving into them is what strips a servo and
 * what makes an axis buzz.  5..175 keeps essentially the whole usable
 * range while leaving a couple of degrees of margin.  Narrow these per
 * axis at runtime with servo_set_limits() once the mount is known. */
#ifndef SERVO_DEFAULT_PAN_MIN
#  define SERVO_DEFAULT_PAN_MIN  5
#endif
#ifndef SERVO_DEFAULT_PAN_MAX
#  define SERVO_DEFAULT_PAN_MAX  175
#endif
#ifndef SERVO_DEFAULT_TILT_MIN
#  define SERVO_DEFAULT_TILT_MIN 5
#endif
#ifndef SERVO_DEFAULT_TILT_MAX
#  define SERVO_DEFAULT_TILT_MAX 175
#endif

int  servo_set_limits(int id, int min_deg, int max_deg);
int  servo_get_limits(int id, int *min_deg, int *max_deg);

/* Debounce: smaller target changes than this (degrees) are ignored.
 * Tracking pushes a new target every 50 ms; without this the PWM is
 * rewritten 20 times a second to no visible effect. */
int  servo_set_deadband(int id, int deg);

/* Rate limit, degrees per second. */
int  servo_set_speed(int id, int deg_per_sec);

/* Preferred entry: non-blocking, honours limits + debounce + rate. */
int  servo_move(int id, int angle);
int  servo_move_pan(int angle);
int  servo_move_tilt(int angle);

/* Direct write, limits still applied (self test / emergency stop). */
int  servo_snap(int id, int angle);

int  servo_get_angle(int id);
int  servo_get_target(int id);
int  servo_is_moving(int id);
int  servo_pan(int angle);        /* 0=右 180=左 (INVERT 时相反) */
int  servo_tilt(int angle);       /* 0=下 180=上 (INVERT 时相反) */
int  servo_center(void);          /* pan=90 tilt=90 */
int  servo_nod(void);             /* 点一下头 (TILT 动画) */
int  servo_shake(void);           /* 摇一下头 (PAN 动画) */
int  servo_self_test(void);       /* 上电自检: 两轴 0->180->90 扫一遍，便于肉眼验证 */
int  servo_keepalive_start(void); /* [V502] 起保活线程（舵机初始化后调用） */
void servo_keepalive_stop(void);
void servo_mark_activity(void);  /* [V502] 动画/跟随改动角度后自动标记 */

#ifdef __cplusplus
}
#endif
#endif /* __PACKAGES_AI_AGENT_SRC_SERVO_H */

/*
 * companion_face.h — K230 人脸跟踪云台 (openvela / NuttX)
 */
#ifndef __PACKAGES_AI_AGENT_SRC_COMPANION_FACE_H
#define __PACKAGES_AI_AGENT_SRC_COMPANION_FACE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

/* UDP 端口，与小智固件一致。K230 侧脚本不用改。 */
#define COMPANION_FACE_PORT 3333

/*
 * ★ 摄像头装在哪，决定用哪套控制律：
 *   0 = 固定在桌面  -> 绝对角度映射（本项目默认，摄像头不动时唯一正确的做法）
 *   1 = 装在云台上  -> 比例累积，有负反馈会收敛
 * 设反会一路转到限位撞死。
 */
#ifndef COMPANION_FACE_ON_GIMBAL
#  define COMPANION_FACE_ON_GIMBAL 1   /* [V1264] camera IS on the gimbal: use the closed loop */
#endif

/* 镜头视场角，换镜头后按实际标定。小智侧现值 65/51。 */
#ifndef COMPANION_FACE_HFOV_DEG
#  define COMPANION_FACE_HFOV_DEG 65.0f
#endif
#ifndef COMPANION_FACE_VFOV_DEG
#  define COMPANION_FACE_VFOV_DEG 51.0f
#endif

/* 方向符号：装反了改这两个的符号，别去改算法。 */
#ifndef CAM_ABS_PAN_SIGN
#  define CAM_ABS_PAN_SIGN 1
#endif
#ifndef CAM_ABS_TILT_SIGN
#  define CAM_ABS_TILT_SIGN 1
#endif
#ifndef CAM_PAN_SIGN
/* [V1266] 实测：pan 增 -> cx 减（_v1265 live log 62 包全程验证），
 * 原值 1 会让闭环变正反馈、一路顶到限位，故改 -1。 */
#  define CAM_PAN_SIGN -1
#endif
#ifndef CAM_TILT_SIGN
/* [V1268] V1266 把两轴一起翻成 -1 是错的。v1267 实测：
 *   tilt=125 -> cy=603 ; tilt=64 -> cy=215  =>  tilt 增则 cy 增。
 * 闭环要负反馈，故 tilt 必须是 +1。pan 的 -1 是对的（cx 已收敛到 500）。 */
#  define CAM_TILT_SIGN -1   /* [V1273] 实测：tilt 增大 => cy 减小（低头脸往画面上方跑），所以取 -1 */
#endif

/* 启动/停止跟随线程。舵机必须先 servo_init() 成功。 */
int  companion_face_start(void);
void companion_face_stop(void);

/* 暂停/恢复自动跟随（手动调角度时用）。 */
void companion_face_set_track(int on);
int  companion_face_get_track(void);

/* 临时接管：把云台拨到 angle 度并保持 seconds 秒。 */
void companion_face_hold(int angle, int seconds);

/* 立刻回中并暂停跟随 3 秒。 */
void companion_face_center(void);

/* 健康度，用于诊断。任一参数可为 NULL。 */
void companion_face_stats(int *lines, int *faces, int *pan,
                          int *tilt, int *have_peer);

/* 取最近一个发包者（K230）的 IP，写入 buf。失败返回 -1。 */
int companion_face_peer(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* __PACKAGES_AI_AGENT_SRC_COMPANION_FACE_H */
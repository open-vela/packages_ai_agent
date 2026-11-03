/****************************************************************************
 * pedometer.h - 计步器（读 /dev/mpu0）
 *
 * 依赖：
 *   - 内核 MPU6050 驱动（nuttx/drivers/sensors/mpu60x0.c）
 *   - 板级把 I2C3 注册为 /dev/i2c2 并 register 出 /dev/mpu0
 *
 * 采样率 25 Hz。这个值是有依据的：
 *   - 步态基频约 1.5-3 Hz，25 Hz 采样有 8 倍以上过采样，够做峰值检测
 *   - 更快的采样对这个只做计步的应用没有收益，反而增加 I2C 占用
 *     （本板 I2C 跑 50kHz，读一次 14 字节约 7ms，25Hz 下占 17% 时间片）
 *
 * 所有值都是**原始 LSB**，不做单位换算：
 *   AFS_SEL=1 -> ±4g -> 1g = 8192 LSB
 * 静止时模长应接近 8192。
 *
 * 持久化：每天的步数 + 最近 7 天历史落在
 * /data/ai_agent/health/steps.json（littlefs），格式
 *   {"date":"2026-10-31","steps":1234,
 *    "history":[{"date":"2026-10-30","steps":4321}]}
 * 跨天自动清零并把前一天压进 history；写盘限频 60 秒一次。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/
#ifndef AI_AGENT_PEDOMETER_H
#define AI_AGENT_PEDOMETER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 1g 对应的原始 LSB（AFS_SEL=1，±4g） */
#define PEDOMETER_LSB_PER_G   8192

/** 启动采样线程（幂等）。返回 0 成功；-1 表示打不开 /dev/mpu0。 */
int  pedometer_start(void);

/** 停止采样线程。 */
void pedometer_stop(void);

/** 累计步数。没启动时返回 0。 */
uint32_t pedometer_get_steps(void);

/** 当前步频（步/分钟，滑动窗口估计）。 */
uint32_t pedometer_get_cadence(void);

/** 最近一次采样是否成功读到数据。 */
bool pedometer_is_alive(void);

/** 已经静止了多少秒（最后一次明显运动到现在）。
 *
 * 睡眠页用。坐着不动是"静止"，躺着睡也是"静止"，手表分不清，
 * 所以界面只如实显示"静止时长"，不声称"睡眠"。
 */
uint32_t pedometer_get_still_seconds(void);

/** 本次运行期间有运动的总秒数。 */
uint32_t pedometer_get_active_seconds(void);

/** 诊断：把最近一帧的原始三轴 + 模长打印出来（NSH 命令用）。 */
void pedometer_dump(void);

/** 步数历史 JSON（含今天），供工具层透传。
 *
 *   {"date":"2026-10-31","steps":1234,
 *    "history":[{"date":"2026-10-30","steps":4321}]}
 *
 * history 最多 7 条、新的在前；顶层的 steps 是今天的实时步数。
 * 没有文件 / 没启动 / 坏文件时也返回**合法 JSON**（steps=0、history 可能
 * 为空），调用方不用判空。
 *
 * 返回 0 成功；-EINVAL 参数错；-ENOMEM 内存不够；-ENOSPC cap 太小
 * （此时 out 里是 {"error":"..."}）。失败时 out 仍是合法 JSON。
 */
int pedometer_get_history_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_PEDOMETER_H */

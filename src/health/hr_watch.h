/****************************************************************************
 * hr_watch.h - 后台静默心率守护（真·心率告警的数据源）
 *
 * 场景③「心率告警」一直缺真数据：pet_care_hr_report() 这个出口在
 * v37 摘掉模拟随机游走（hr_monitor.c #if 0）后就一直空着。本模块
 * 把它接上——低频后台静默测量真实心率，超阈值才真正告警。
 *
 * 与既有模块的分工：
 *   - hr_measure.c  单次测量的全部实现（采集/质量判据/收尾）；
 *                   用户点按钮走 hr_measure_start()（会上屏会播报），
 *                   本模块走 hr_measure_start_silent()（同一套测量，
 *                   跳过全部 UI/语音——后台测量不能打扰用户）。
 *   - pet_care.c    告警出口 pet_care_hr_report()：内部自带 60s 冷却
 *                   与免打扰感知，本模块不重复造节流。
 *   - hr_watch.c    只做「定时起测 -> 等结束 -> 分诊」三件事。
 *
 * 调用约束：hr_watch_start() 任意线程可调（内部只创建线程），
 * 幂等——重复调用直接返回。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_HR_WATCH_H
#define AI_AGENT_HR_WATCH_H

#include <stdbool.h>

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 高心率告警阈值（bpm）。
 * 依据：成人静息心率正常范围约 60~100 bpm，AHA/梅奥等普遍把持续
 * 超过 ~120 bpm 的静息心率视为心动过速范畴、建议关注。手表单次
 * 30s 测量精度有限，取 120 而不是 100，宁可少报也不拿噪声吓用户
 * （与本项目「够用 + 诚实」原则一致：不报假警优先于多报）。 */
#define HR_WATCH_BPM_HIGH 120

/* 守护检查周期（秒）。10 分钟：静息心率不会分钟级剧变，更长周期
 * 会漏掉真实的持续升高；更短周期则传感器 LED 频繁点亮耗电、
 * 且每轮要独占 I2C 约 30s。 */
#define HR_WATCH_INTERVAL_S 600

/**
 * 幂等启动后台心率守护线程。
 *
 * 周期检查：若当前没有测量在进行（含用户手动测量），就静默起一次
 * （不上屏、不出声）；测量结束后读结果分诊：
 *   bpm == 0            -> 测量失败，静默跳过（连续 >3 次仅 syslog 警告）
 *   bpm >= HR_WATCH_BPM_HIGH -> 走 pet_care_hr_report() 告警（内部有
 *                          60s 冷却 + 免打扰感知，不重复节流）
 *   其余                -> 仅 syslog 记录
 *
 * @return 0    已启动（或此前已启动，幂等）
 *         负值 线程创建失败（-ENOMEM 等）
 */
int hr_watch_start(void);

/**
 * 守护线程是否已在运行（诊断用）。
 */
bool hr_watch_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_HR_WATCH_H */

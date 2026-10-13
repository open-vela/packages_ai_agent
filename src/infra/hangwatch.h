/****************************************************************************
 * hangwatch.h - 冻屏/死锁取证看门狗
 *
 * 背景: 真机上出现过"背光还亮但画面卡住 + 触摸无反应 + 串口输入无响应",
 * 且日志里没有任何断言/异常。这种现场无法用 printf 定位, 因为**不知道
 * 是整机死了还是只有 UI 线程死了**。
 *
 * 本模块提供一个跑在独立线程里的看门狗:
 *   - 每 5s 打一行 [alive](uptime / UI 心跳 / 各状态任务数) —— 只要它还在
 *     打, 就说明调度器还活着, 死的是 UI 那条链路; 它一起停 = 整机卡死。
 *   - UI 心跳停 >= 8s 时打 [stall] 并把**所有任务**的
 *     pid/state/prio/stack/name 全量 dump 出来 —— 直接指出是谁卡住。
 *
 * 判"UI 任务是否还在跑"只能用下面的 UI 心跳: lv_tick_get() 由 tick 中断
 * 推进, 即使 LVGL 任务卡死也照涨, 不能作为判据。
 ****************************************************************************/

#ifndef __AI_AGENT_INFRA_HANGWATCH_H
#define __AI_AGENT_INFRA_HANGWATCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* UI(LVGL 任务)心跳: 由 launcher 桌面的 1Hz lv_timer 回调自增。 */
void hangwatch_ui_tick(void);

/* 启动看门狗线程。返回 0 成功, 否则为 pthread_create 的错误码。 */
int hangwatch_start(void);

#ifdef __cplusplus
}
#endif

#endif /* __AI_AGENT_INFRA_HANGWATCH_H */

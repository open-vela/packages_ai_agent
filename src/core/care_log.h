/****************************************************************************
 * care_log.h - PROACTIVE-P1A/P1B：主动关怀决策/反馈 RAM 环形日志
 *
 * 依据 docs_ble/109 §4/§5.P1a + docs_ble/110 §1/§2（P1b 协议）：
 *   - 记录 event/type/features 摘要/state/score/decision/action/feedback/reason；
 *   - P1b：每条带单调递增 seq（App care_ack 用）与墙钟 ts（App 时间线用）；
 *   - P1b：add 返回 seq，mark_pending(seq) 显式绑定本条（修 P1a 用
 *     "最后写入下标"导致的并发错绑），ack(seq,feedback) 允许 App 覆盖隐式反馈。
 *
 * 约束（v142i，/data 只读）：
 *   - 纯 RAM：环形 16 条（约 832B），重启即丢；
 *   - 不新增线程、不写盘；1Hz 超时由既有 care_router_tick 驱动；
 *   - 字符串一律存**静态指针**（调用方保证传静态字符串/字面量），不拷贝。
 ****************************************************************************/

#ifndef AI_AGENT_CARE_LOG_H
#define AI_AGENT_CARE_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARE_LOG_MAX 16

/* features 摘要 flags（位掩码，dump/JSON 时展开） */
#define CARE_LOG_FLAG_DND   0x01
#define CARE_LOG_FLAG_QUIET 0x02
#define CARE_LOG_FLAG_BUSY  0x04

/* 隐式/显式反馈结论（也是 care_log_entry_t.feedback 的取值） */
#define CARE_LOG_FEEDBACK_NONE     ""
#define CARE_LOG_FEEDBACK_ACCEPTED "ACCEPTED"
#define CARE_LOG_FEEDBACK_IGNORED  "IGNORED"
#define CARE_LOG_FEEDBACK_REJECTED "REJECTED"

/* 一条日志（52B；16 条约 832B）
 * P1b 增补 seq / ts_wall_s：seq 供 App ack 定位；ts 是墙钟 epoch（秒，
 * 手机 App 时间线直接用；导出时按协议换成真 UTC，见 BLE handler）。 */
typedef struct
{
  uint32_t    seq;        /* 单调递增序号（1 起，0 保留） */
  uint32_t    ts_wall_s;  /* 记录时刻墙钟（epoch 秒，本地时基） */
  uint32_t    ts_s;       /* 记录时刻（MONOTONIC 秒，内部超时用） */
  const char *event;      /* action / silent / defer */
  const char *type;       /* idle / sedentary / hr_alert */
  const char *state;      /* ACTIVE / RESTING / INACTIVE / ELEVATED / UNKNOWN */
  const char *decision;   /* ACTION / SILENT / DEFER */
  const char *reason;     /* Router 的 reason（静态字符串） */
  const char *action;     /* fire / none / defer_ticket */
  const char *feedback;   /* "" / ACCEPTED / IGNORED / REJECTED */
  int8_t      score;
  int16_t     hr_bpm;
  int16_t     hr_dev;
  uint16_t    still_min;  /* 静止分钟数（饱和） */
  uint16_t    idle_s;     /* 空闲秒数（饱和） */
  uint8_t     hour;
  uint8_t     flags;      /* CARE_LOG_FLAG_* */
} care_log_entry_t;

/* 清空（诊断/测试/回滚用；正常运行无需调用） */
void care_log_reset(void);

/* 追加一条（环形覆盖最旧）。字符串参数必须指向静态存储。
 * 返回本条 seq（>0）；失败（参数/内部）返回 0。 */
uint32_t care_log_add(const char *event, const char *type, const char *state,
                      const char *decision, const char *reason,
                      const char *action, int score, int hr_bpm, int hr_dev,
                      uint32_t still_s, uint32_t idle_s, uint8_t hour,
                      uint8_t flags, uint32_t now_s);

/* 主动气泡真正发出后调用：把 seq 对应的日志标记为"等待隐式反馈"，
 * 窗口 window_s（P1a 默认 60s）。若上一条还在等待，判定为 IGNORED。
 * seq 已被环形覆盖/不存在时静默 no-op（不影响主流程）。 */
void care_log_mark_pending(uint32_t seq, uint32_t now_s, uint32_t window_s);

/* App 显式反馈（P1b care_ack，P1c 幂等化）：按 seq 覆盖 feedback。
 * feedback 必须是 ACCEPTED/IGNORED/REJECTED 之一；seq 不存在/非法返回 false。
 *
 * P1c 语义（与 App 双击/重试对齐）：
 *   - 同一 seq+feedback 重复 ack：返回 true，但**不重复累加** accepted/
 *     ignored/rejected（幂等）；
 *   - feedback 真正变化时才调整计数：隐式值 -> 显式值时先撤销旧桶再加新桶，
 *     保持统计分布一致；
 *   - 只有 action=="fire"（已开口的 ACTION）条目进入决策统计；
 *     SILENT/DEFER 条目允许 ack（写 feedback 供 App 对账）但不计入统计。 */
bool care_log_ack(uint32_t seq, const char *feedback);

/* 用户交互（触摸/按键/PTT/步数）到达：窗口内 -> feedback=ACCEPTED。 */
void care_log_note_activity(uint32_t now_s);

/* 1Hz：窗口超时 -> feedback=IGNORED（由 care_router_tick 驱动）。 */
void care_log_tick(uint32_t now_s);

/* 当前日志条数（0..CARE_LOG_MAX）。 */
int care_log_count(void);

/* P1b BLE care_list：把最近 limit 条（按 seq 倒序）拼成一条完整 JSON：
 *   {"evt":"care_list","count":N,"items":[{...},...]}
 * 自动按 cap 截断 items（至少能放下 1 条；放不下返回 -1）。
 * 调用方（BLE RX 线程）传现有 RX 缓冲即可复用，无需新增静态 RAM。
 * ts_offset_s：把手表本地墙钟换成 App 用的真 UTC epoch 的偏移量
 * （BLE handler 传 -AGENT_UTC_OFFSET_SEC；其它调用可传 0）。
 * 返回写入字节数（>0）或 -1。 */
int care_log_export_json(char *out, size_t cap, int limit,
                         int32_t ts_offset_s);

/* 串口诊断：汇总 + 每条明细（NSH care_test 汇总用）。 */
void care_log_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_CARE_LOG_H */

/****************************************************************************
 * care_ticket.h - PROACTIVE-P1A：主动关怀 Ticket（纯 RAM，不写 /data）
 *
 * 依据 docs_ble/109_proactive_companion_roadmap.md §4/§5.P1a：
 *   Router 的 ACTION -> 建 FIRED ticket 并执行；
 *   Router 的 DEFER  -> 建 DEFERRED ticket（只记录，到点后由既有 1Hz tick
 *                       的自然重评估接管，不新增线程/定时器）；
 *   SILENT           -> 不建 ticket（由 care_log 记录原因）。
 *
 * 约束（v142f，/data 只读）：
 *   - 只有 RAM：最大 8 条，重启即丢（P3 才谈持久化）；
 *   - 不新增线程、不写盘、不联网；
 *   - 时间由调用方传入（MONOTONIC 秒），本模块不读时钟（便于 host 测试）。
 ****************************************************************************/

#ifndef AI_AGENT_CARE_TICKET_H
#define AI_AGENT_CARE_TICKET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARE_TICKET_MAX        8   /* 静态上限（SRAM 紧张，固定分配） */
#define CARE_TICKET_ID_INVALID 0

/* ACTION ticket 的默认 TTL：保留 5min，覆盖 60s 隐式反馈窗口 + 诊断时间 */
#define CARE_TICKET_ACTION_TTL_S 300

/* 优先级（数值越大越优先；P1a 只用于 dump/后续排序，不参与执行顺序） */
#define CARE_TICKET_PRIO_LOW    0
#define CARE_TICKET_PRIO_NORMAL 1
#define CARE_TICKET_PRIO_HIGH   2

typedef enum
{
  CARE_TICKET_FREE = 0,
  CARE_TICKET_DEFERRED,   /* DEFER：到点后由下一次自然 eval 重新判定 */
  CARE_TICKET_FIRED,      /* ACTION 已执行（等待隐式反馈/诊断） */
  CARE_TICKET_EXPIRED,    /* TTL 到期 */
  CARE_TICKET_CANCELLED   /* 去重/手动取消 */
} care_ticket_status_t;

/* 一条 ticket（20B，8 条共 160B） */
typedef struct
{
  uint16_t    id;
  uint8_t     status;     /* care_ticket_status_t */
  uint8_t     priority;   /* CARE_TICKET_PRIO_* */
  uint32_t    created_s;  /* 创建时刻（MONOTONIC 秒） */
  uint32_t    expires_s;  /* TTL 到期时刻（MONOTONIC 秒） */
  const char *type;       /* idle / sedentary / hr_alert（静态字符串） */
  const char *action;     /* care.fire / defer.reeval（静态字符串） */
} care_ticket_t;

/* 清空所有 ticket（诊断/测试用；正常运行无需调用） */
void care_ticket_reset(void);

/* ACTION：建 FIRED ticket。返回 id；0 表示失败（无槽/参数错）。
 * P1b 去重规则：只取消同 type 的 **DEFERRED** 票；FIRED 票保留
 * （已开口的审计记录，App 时间线/care_log 需要能对上）。 */
uint16_t care_ticket_create(const char *type, uint8_t priority,
                            uint32_t ttl_s, uint32_t now_s);

/* DEFER：建 DEFERRED ticket，expires = now + delay_s。
 * 仅记录/诊断用；到期后由 care_router_tick 的 sweep 标记 EXPIRED，
 * 下一次 router_request 会自然重评估（无额外线程/定时器）。 */
uint16_t care_ticket_defer(const char *type, uint8_t priority,
                           uint32_t delay_s, uint32_t now_s);

/* 取消一条（去重/App 后续 ack 用）。返回 true 表示确实取消了。 */
bool care_ticket_cancel(uint16_t id);

/* 1Hz：TTL 到期 -> EXPIRED（由 care_router_tick 调用）。 */
void care_ticket_sweep(uint32_t now_s);

/* 当前活跃 ticket 数（DEFERRED + FIRED）。 */
int care_ticket_count(void);

/* 串口诊断：活跃/历史计数 + 每条明细（NSH care_test 汇总用）。 */
void care_ticket_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_CARE_TICKET_H */

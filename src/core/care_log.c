/*
 * care_log.c - PROACTIVE-P1A/P1B：主动关怀决策/反馈 RAM 环形日志
 *
 * 详见 care_log.h 与 docs_ble/109 §4/§5.P1a、docs_ble/110 §1/§2。
 * 纯 RAM、固定 16 条、无写盘、无新增线程；1Hz 超时判定由既有
 * care_router_tick 驱动。P1b 增补 seq/墙钟 ts、显式 ack、care_list JSON。
 */

#include "core/care_log.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define TAG "care_log"

#define PENDING_NONE 0xFF

static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;
static care_log_entry_t s_entries[CARE_LOG_MAX];
static uint8_t          s_head;         /* 下一条写入位置 */
static uint8_t          s_count;
static uint8_t          s_pending_idx;  /* 等待隐式反馈的条目下标 */
static uint32_t         s_pending_deadline;
static uint32_t         s_next_seq;     /* 下一条 seq；0 保留不用 */
static uint32_t         s_accepted;
static uint32_t         s_ignored;
static uint32_t         s_rejected;
static bool             s_inited;

static const char *safe_str(const char *s)
{
    return (s != NULL) ? s : "-";
}

static uint16_t sat_u16(uint32_t v)
{
    return (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;
}

static time_t wall_now_s(void)
{
    return time(NULL);
}

/* 调用方持锁 */
static void init_locked(void)
{
    if (s_inited) {
        return;
    }

    s_inited = true;
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
    s_pending_idx = PENDING_NONE;
    s_pending_deadline = 0;
    s_next_seq = 1;
    s_accepted = 0;
    s_ignored = 0;
    s_rejected = 0;
}

/* 调用方持锁：按 seq 找条目下标；找不到 -1（被环形覆盖/从未存在）。 */
static int find_idx_by_seq_locked(uint32_t seq)
{
    int i;

    if (seq == 0) {
        return -1;
    }

    for (i = 0; i < CARE_LOG_MAX; i++) {
        if (s_entries[i].seq == seq) {
            return i;
        }
    }

    return -1;
}

/* 调用方持锁：把 pending 条目结案（feedback 未写则按 IGNORED 收尾）。 */
static void close_pending_locked(uint32_t now_s, bool accepted)
{
    care_log_entry_t *e;

    if (s_pending_idx == PENDING_NONE) {
        return;
    }

    e = &s_entries[s_pending_idx];
    if (e->feedback != NULL && e->feedback[0] == '\0') {
        if (accepted && now_s <= s_pending_deadline) {
            e->feedback = CARE_LOG_FEEDBACK_ACCEPTED;
            s_accepted++;
        } else {
            e->feedback = CARE_LOG_FEEDBACK_IGNORED;
            s_ignored++;
        }
    }

    s_pending_idx = PENDING_NONE;
}

/* 调用方持锁：合法显式反馈字符串 -> 静态常量指针（条目只存静态指针）。 */
static const char *feedback_const(const char *feedback)
{
    if (feedback == NULL) {
        return NULL;
    }
    if (strcmp(feedback, CARE_LOG_FEEDBACK_ACCEPTED) == 0) {
        return CARE_LOG_FEEDBACK_ACCEPTED;
    }
    if (strcmp(feedback, CARE_LOG_FEEDBACK_IGNORED) == 0) {
        return CARE_LOG_FEEDBACK_IGNORED;
    }
    if (strcmp(feedback, CARE_LOG_FEEDBACK_REJECTED) == 0) {
        return CARE_LOG_FEEDBACK_REJECTED;
    }
    return NULL;
}

/* 调用方持锁：按 feedback 调整 accepted/ignored/rejected 计数。
 * 只有 ACTION/fire 且 feedback 真正变化时才调用（P1c：重复 ack 幂等）。 */
static void counter_adjust_locked(const char *fb, int delta)
{
    if (fb == NULL || fb[0] == '\0' || delta == 0) {
        return;
    }

    if (strcmp(fb, CARE_LOG_FEEDBACK_ACCEPTED) == 0) {
        if (delta > 0) {
            s_accepted++;
        } else if (s_accepted > 0) {
            s_accepted--;
        }
    } else if (strcmp(fb, CARE_LOG_FEEDBACK_IGNORED) == 0) {
        if (delta > 0) {
            s_ignored++;
        } else if (s_ignored > 0) {
            s_ignored--;
        }
    } else if (strcmp(fb, CARE_LOG_FEEDBACK_REJECTED) == 0) {
        if (delta > 0) {
            s_rejected++;
        } else if (s_rejected > 0) {
            s_rejected--;
        }
    }
}

void care_log_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_inited = true;
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
    s_pending_idx = PENDING_NONE;
    s_pending_deadline = 0;
    s_next_seq = 1;
    s_accepted = 0;
    s_ignored = 0;
    s_rejected = 0;
    pthread_mutex_unlock(&s_lock);
}

uint32_t care_log_add(const char *event, const char *type, const char *state,
                      const char *decision, const char *reason,
                      const char *action, int score, int hr_bpm, int hr_dev,
                      uint32_t still_s, uint32_t idle_s, uint8_t hour,
                      uint8_t flags, uint32_t now_s)
{
    uint8_t idx;
    uint32_t seq;
    care_log_entry_t *e;

    pthread_mutex_lock(&s_lock);
    init_locked();

    idx = s_head;
    e = &s_entries[idx];

    if (s_pending_idx == idx) {
        /* 极端情况：等待反馈的条目被环形覆盖 -> 放弃等待（不伪造结论） */
        s_pending_idx = PENDING_NONE;
    }

    seq = s_next_seq++;
    if (s_next_seq == 0) {
        s_next_seq = 1;     /* 0 保留为 invalid */
    }
    if (seq == 0) {
        seq = s_next_seq++;
        if (s_next_seq == 0) {
            s_next_seq = 1;
        }
    }

    e->seq = seq;
    e->ts_wall_s = (uint32_t)wall_now_s();
    e->ts_s = now_s;
    e->event = safe_str(event);
    e->type = safe_str(type);
    e->state = safe_str(state);
    e->decision = safe_str(decision);
    e->reason = safe_str(reason);
    e->action = safe_str(action);
    e->feedback = CARE_LOG_FEEDBACK_NONE;
    e->score = (int8_t)((score > 127) ? 127 : ((score < -128) ? -128 : score));
    e->hr_bpm = (int16_t)((hr_bpm > 32767) ? 32767
                          : ((hr_bpm < -32768) ? -32768 : hr_bpm));
    e->hr_dev = (int16_t)((hr_dev > 32767) ? 32767
                          : ((hr_dev < -32768) ? -32768 : hr_dev));
    e->still_min = sat_u16(still_s / 60u);
    e->idle_s = sat_u16(idle_s);
    e->hour = hour;
    e->flags = flags;

    s_head = (uint8_t)((idx + 1) % CARE_LOG_MAX);
    if (s_count < CARE_LOG_MAX) {
        s_count++;
    }

    pthread_mutex_unlock(&s_lock);
    return seq;
}

void care_log_mark_pending(uint32_t seq, uint32_t now_s, uint32_t window_s)
{
    int idx;

    pthread_mutex_lock(&s_lock);
    init_locked();

    if (seq != 0) {
        /* 上一条还没反馈就来了新的主动开口：旧条目判 IGNORED
         * （对应"下一次同类触发 -> IGNORED"）。 */
        close_pending_locked(now_s, false);

        idx = find_idx_by_seq_locked(seq);
        if (idx >= 0
            && (s_entries[idx].feedback == NULL
                || s_entries[idx].feedback[0] == '\0')) {
            if (window_s == 0) {
                window_s = 60;
            }
            s_pending_idx = (uint8_t)idx;
            s_pending_deadline = now_s + window_s;
        }
    }

    pthread_mutex_unlock(&s_lock);
}

bool care_log_ack(uint32_t seq, const char *feedback)
{
    int idx;
    const char *fb;
    bool ok = false;
    bool changed = false;

    fb = feedback_const(feedback);
    if (seq == 0 || fb == NULL) {
        return false;
    }

    pthread_mutex_lock(&s_lock);
    init_locked();

    idx = find_idx_by_seq_locked(seq);
    if (idx >= 0) {
        care_log_entry_t *e = &s_entries[idx];
        const char *old = (e->feedback != NULL) ? e->feedback : "";
        bool countable = (e->action != NULL
                          && strcmp(e->action, "fire") == 0);

        /* P1c：ack 幂等 —— 同一 seq+feedback 重复 ack 返回 ok=1，但
         * **不重复累加** accepted/ignored/rejected；只有 feedback 真的变化
         * 才调整计数（隐式值 -> 显式值时先撤销旧桶再加新桶，保持分布一致）。
         * 非 ACTION/fire 条目（SILENT/DEFER）允许 ack 以便 App 对账并写入
         * feedback，但**不进入决策统计**（避免静默记录污染接受率）。 */
        if (strcmp(old, fb) != 0) {
            if (countable) {
                counter_adjust_locked(old, -1);
                counter_adjust_locked(fb, +1);
            }
            e->feedback = fb;   /* 显式反馈覆盖隐式值（P1b 协议要求） */
            changed = true;
        }
        if (s_pending_idx == idx) {
            s_pending_idx = PENDING_NONE;
        }
        ok = true;
    }

    pthread_mutex_unlock(&s_lock);

    if (ok && !changed) {
        /* 幂等重试：保留诊断日志，便于 App 双击/重试排查 */
        syslog(LOG_INFO,
               "[%s] ack idempotent: seq=%u feedback=%s unchanged\n",
               TAG, (unsigned)seq, fb);
    }

    return ok;
}

void care_log_note_activity(uint32_t now_s)
{
    pthread_mutex_lock(&s_lock);
    init_locked();

    /* 窗口内 = ACCEPTED；已超时 = IGNORED（不晚到补记）。 */
    close_pending_locked(now_s, true);

    pthread_mutex_unlock(&s_lock);
}

void care_log_tick(uint32_t now_s)
{
    pthread_mutex_lock(&s_lock);
    init_locked();

    if (s_pending_idx != PENDING_NONE && now_s > s_pending_deadline) {
        close_pending_locked(now_s, false);
    }

    pthread_mutex_unlock(&s_lock);
}

int care_log_count(void)
{
    int n;

    pthread_mutex_lock(&s_lock);
    init_locked();
    n = (int)s_count;
    pthread_mutex_unlock(&s_lock);

    return n;
}

/* 单条 JSON（调用方保证类型/原因等均为静态无引号 token） */
static int format_item(char *dst, size_t cap, uint32_t ts, const care_log_entry_t *e)
{
    return snprintf(dst, cap,
        "{\"seq\":%u,\"ts\":%u,\"type\":\"%s\",\"state\":\"%s\","
        "\"decision\":\"%s\",\"reason\":\"%s\",\"action\":\"%s\","
        "\"feedback\":\"%s\",\"score\":%d,\"hr\":%d,\"dev\":%d,"
        "\"still_min\":%u,\"idle_s\":%u,\"hour\":%u,"
        "\"dnd\":%d,\"quiet\":%d}",
        (unsigned)e->seq, (unsigned)ts,
        safe_str(e->type), safe_str(e->state), safe_str(e->decision),
        safe_str(e->reason), safe_str(e->action),
        (e->feedback != NULL && e->feedback[0] != '\0') ? e->feedback : "",
        (int)e->score, (int)e->hr_bpm, (int)e->hr_dev,
        (unsigned)e->still_min, (unsigned)e->idle_s, (unsigned)e->hour,
        (e->flags & CARE_LOG_FLAG_DND) ? 1 : 0,
        (e->flags & CARE_LOG_FLAG_QUIET) ? 1 : 0);
}

/* P1c：care_list 时间戳统一门控（两遍共用，避免第一遍按 0 估长短、
 * 第二遍写长 ts 导致 count 与实际 items 不一致）。
 * 手表墙钟 + 偏移 -> App 真 UTC；2024-01-01..2027-12-31 之外输出 0
 * （App 显示"时间未知"），挡住未对时(1970)与 RTC 2036 假时间。 */
static uint32_t export_ts(uint32_t ts_wall_s, int32_t ts_offset_s)
{
    uint32_t ts = (uint32_t)((int64_t)ts_wall_s + ts_offset_s);

    if (ts < 1704067200U || ts > 1830297600U) {
        return 0;
    }
    return ts;
}

int care_log_export_json(char *out, size_t cap, int limit, int32_t ts_offset_s)
{
    char tmp[300];
    int  n = 0;
    int  i;
    int  written;
    int  total;
    size_t used;

    if (out == NULL || cap < 80) {
        return -1;
    }

    pthread_mutex_lock(&s_lock);
    init_locked();

    if (limit < 1) {
        limit = 1;
    }
    if (limit > CARE_LOG_MAX) {
        limit = CARE_LOG_MAX;
    }
    if (limit > (int)s_count) {
        limit = (int)s_count;
    }

    /* 第一遍：估算有多少条能放进 cap（预留 56B 给头部/尾部分隔） */
    used = 56;
    for (i = 0; i < limit; i++) {
        int idx = (int)((s_head + CARE_LOG_MAX - 1 - (uint8_t)i) % CARE_LOG_MAX);
        uint32_t ts = export_ts(s_entries[idx].ts_wall_s, ts_offset_s);
        int len = format_item(tmp, sizeof(tmp), ts, &s_entries[idx]);

        if (len <= 0) {
            break;
        }
        if (used + (size_t)(n > 0 ? 1 : 0) + (size_t)len > cap) {
            break;
        }
        used += (size_t)(n > 0 ? 1 : 0) + (size_t)len;
        n++;
    }

    written = snprintf(out, cap, "{\"evt\":\"care_list\",\"count\":%d,\"items\":[", n);
    if (written < 0 || (size_t)written >= cap) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    total = written;

    for (i = 0; i < n; i++) {
        int idx = (int)((s_head + CARE_LOG_MAX - 1 - (uint8_t)i) % CARE_LOG_MAX);
        uint32_t ts = export_ts(s_entries[idx].ts_wall_s, ts_offset_s);
        int len = format_item(tmp, sizeof(tmp), ts, &s_entries[idx]);

        if (len <= 0 || total + len + 4 >= (int)cap) {
            break;
        }
        if (i > 0) {
            out[total++] = ',';
        }
        memcpy(out + total, tmp, (size_t)len + 1);
        total += len;
    }

    written = snprintf(out + total, cap - (size_t)total, "]}");
    if (written < 0 || (size_t)written >= cap - (size_t)total) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    total += written;

    pthread_mutex_unlock(&s_lock);
    return total;
}

void care_log_dump(void)
{
    uint32_t accepted;
    uint32_t ignored;
    uint32_t rejected;
    uint8_t  head;
    uint8_t  count;
    uint8_t  pending;
    int      start;
    int      i;

    pthread_mutex_lock(&s_lock);
    init_locked();

    head = s_head;
    count = s_count;
    pending = s_pending_idx;
    accepted = s_accepted;
    ignored = s_ignored;
    rejected = s_rejected;

    printf("[care_log] count=%u/%d accepted=%u ignored=%u rejected=%u "
           "pending=%d next_seq=%u\n",
           (unsigned)count, CARE_LOG_MAX, (unsigned)accepted,
           (unsigned)ignored, (unsigned)rejected,
           (pending == PENDING_NONE) ? -1 : (int)pending,
           (unsigned)s_next_seq);

    start = (int)((head + CARE_LOG_MAX - count) % CARE_LOG_MAX);
    for (i = 0; i < (int)count; i++) {
        int idx = (start + i) % CARE_LOG_MAX;
        const care_log_entry_t *e = &s_entries[idx];

        printf("[care_log] seq=%u ts=%u wall=%u event=%s type=%s state=%s "
               "decision=%s reason=%s action=%s feedback=%s score=%d "
               "hr=%d dev=%d still=%umin idle=%us hour=%u dnd=%d quiet=%d "
               "busy=%d\n",
               (unsigned)e->seq, (unsigned)e->ts_s, (unsigned)e->ts_wall_s,
               safe_str(e->event), safe_str(e->type), safe_str(e->state),
               safe_str(e->decision), safe_str(e->reason),
               safe_str(e->action),
               (e->feedback != NULL && e->feedback[0] != '\0')
                   ? e->feedback : "-",
               (int)e->score, (int)e->hr_bpm, (int)e->hr_dev,
               (unsigned)e->still_min, (unsigned)e->idle_s,
               (unsigned)e->hour,
               (e->flags & CARE_LOG_FLAG_DND) ? 1 : 0,
               (e->flags & CARE_LOG_FLAG_QUIET) ? 1 : 0,
               (e->flags & CARE_LOG_FLAG_BUSY) ? 1 : 0);
    }

    pthread_mutex_unlock(&s_lock);
}

/*
 * care_ticket.c - PROACTIVE-P1A：主动关怀 Ticket（纯 RAM）
 *
 * 详见 care_ticket.h 与 docs_ble/109 §4/§5.P1a。这里只做 RAM 状态机：
 *   ACTION -> FIRED，DEFER -> DEFERRED（到点 EXPIRED，由自然重评估接管）。
 * 不写盘、不新增线程；逻辑时间全部由调用方传入，只有 dump 为了显示 age
 * 读一次 MONOTONIC（诊断路径，不在 1Hz 热路径里）。
 */

#include "core/care_ticket.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define TAG "care_ticket"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static care_ticket_t   s_tickets[CARE_TICKET_MAX];
static uint16_t        s_next_id = 1;
static uint32_t        s_created;
static uint32_t        s_deduped;
static uint32_t        s_expired;
static uint32_t        s_cancelled;
static bool            s_inited;

static const char *status_name(uint8_t st)
{
    switch (st) {
    case CARE_TICKET_DEFERRED:  return "DEFERRED";
    case CARE_TICKET_FIRED:     return "FIRED";
    case CARE_TICKET_EXPIRED:   return "EXPIRED";
    case CARE_TICKET_CANCELLED: return "CANCELLED";
    default:                    return "FREE";
    }
}

static time_t mono_now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* 调用方持锁 */
static void init_locked(void)
{
    if (s_inited) {
        return;
    }

    s_inited = true;
    memset(s_tickets, 0, sizeof(s_tickets));
    s_next_id = 1;
    s_created = 0;
    s_deduped = 0;
    s_expired = 0;
    s_cancelled = 0;
}

static bool status_active(uint8_t st)
{
    return st == CARE_TICKET_DEFERRED || st == CARE_TICKET_FIRED;
}

/* 调用方持锁 */
static care_ticket_t *find_by_id_locked(uint16_t id)
{
    int i;

    for (i = 0; i < CARE_TICKET_MAX; i++) {
        if (s_tickets[i].status != CARE_TICKET_FREE
            && s_tickets[i].id == id) {
            return &s_tickets[i];
        }
    }

    return NULL;
}

/* 调用方持锁：同 type 的**未执行**（DEFERRED）票去重。
 * ★ P1b（perf-fw 复核）：去重只取消 DEFERRED，**不取消 FIRED**——
 *   FIRED 是已经开口的审计记录，要保留（App 时间线/care_log 对账），
 *   否则同场景连续触发会把历史冲掉。 */
static care_ticket_t *find_deferred_by_type_locked(const char *type)
{
    int i;

    for (i = 0; i < CARE_TICKET_MAX; i++) {
        if (s_tickets[i].status == CARE_TICKET_DEFERRED
            && s_tickets[i].type != NULL
            && strcmp(s_tickets[i].type, type) == 0) {
            return &s_tickets[i];
        }
    }

    return NULL;
}

/* 调用方持锁：优先空槽 -> 最老 EXPIRED/CANCELLED -> 最老活跃（防御性）
 * 返回 NULL 仅当... 实际上永远能返回一个槽（最坏覆盖最老活跃）。 */
static care_ticket_t *alloc_slot_locked(void)
{
    care_ticket_t *best = NULL;
    int i;

    for (i = 0; i < CARE_TICKET_MAX; i++) {
        if (s_tickets[i].status == CARE_TICKET_FREE) {
            return &s_tickets[i];
        }
    }

    for (i = 0; i < CARE_TICKET_MAX; i++) {
        if (s_tickets[i].status == CARE_TICKET_EXPIRED
            || s_tickets[i].status == CARE_TICKET_CANCELLED) {
            if (best == NULL || s_tickets[i].created_s < best->created_s) {
                best = &s_tickets[i];
            }
        }
    }
    if (best != NULL) {
        return best;
    }

    for (i = 0; i < CARE_TICKET_MAX; i++) {
        if (best == NULL || s_tickets[i].created_s < best->created_s) {
            best = &s_tickets[i];
        }
    }

    if (best != NULL) {
        syslog(LOG_WARNING, "[%s] evict active id=%u type=%s\n",
               TAG, best->id, best->type != NULL ? best->type : "-");
    }

    return best;
}

/* 调用方持锁：填充 + 记账 */
static uint16_t fill_locked(const char *type, const char *action,
                            uint8_t status, uint8_t priority,
                            uint32_t ttl_s, uint32_t now_s)
{
    care_ticket_t *slot;
    care_ticket_t *old;
    uint16_t id;

    old = find_deferred_by_type_locked(type);
    if (old != NULL) {
        /* 只替换尚未执行的 DEFERRED 票；FIRED（已开口）保留审计链 */
        old->status = CARE_TICKET_CANCELLED;
        s_deduped++;
        syslog(LOG_INFO, "[%s] dedupe type=%s old_id=%u (deferred)\n",
               TAG, type, old->id);
    }

    slot = alloc_slot_locked();
    if (slot == NULL) {
        return CARE_TICKET_ID_INVALID;
    }

    id = s_next_id++;
    if (s_next_id == 0) {
        s_next_id = 1;      /* 0 保留为 invalid */
    }

    slot->id = id;
    slot->status = status;
    slot->priority = priority;
    slot->created_s = now_s;
    slot->expires_s = now_s + ttl_s;
    slot->type = type;
    slot->action = action;
    s_created++;

    return id;
}

void care_ticket_reset(void)
{
    pthread_mutex_lock(&s_lock);
    /* init_locked() 是幂等的；reset 要真的清空，不能只调它 */
    memset(s_tickets, 0, sizeof(s_tickets));
    s_next_id = 1;
    s_created = 0;
    s_deduped = 0;
    s_expired = 0;
    s_cancelled = 0;
    s_inited = true;
    pthread_mutex_unlock(&s_lock);
}

uint16_t care_ticket_create(const char *type, uint8_t priority,
                            uint32_t ttl_s, uint32_t now_s)
{
    uint16_t id;

    if (type == NULL) {
        return CARE_TICKET_ID_INVALID;
    }
    if (ttl_s == 0) {
        ttl_s = 1;
    }

    pthread_mutex_lock(&s_lock);
    init_locked();
    id = fill_locked(type, "care.fire", CARE_TICKET_FIRED, priority,
                     ttl_s, now_s);
    if (id != CARE_TICKET_ID_INVALID) {
        syslog(LOG_INFO,
               "[%s] create id=%u type=%s status=FIRED prio=%u ttl=%us\n",
               TAG, id, type, priority, (unsigned)ttl_s);
    }
    pthread_mutex_unlock(&s_lock);

    return id;
}

uint16_t care_ticket_defer(const char *type, uint8_t priority,
                           uint32_t delay_s, uint32_t now_s)
{
    uint16_t id;

    if (type == NULL) {
        return CARE_TICKET_ID_INVALID;
    }
    if (delay_s == 0) {
        delay_s = 1;
    }

    pthread_mutex_lock(&s_lock);
    init_locked();
    id = fill_locked(type, "defer.reeval", CARE_TICKET_DEFERRED, priority,
                     delay_s, now_s);
    if (id != CARE_TICKET_ID_INVALID) {
        syslog(LOG_INFO,
               "[%s] create id=%u type=%s status=DEFERRED prio=%u delay=%us\n",
               TAG, id, type, priority, (unsigned)delay_s);
    }
    pthread_mutex_unlock(&s_lock);

    return id;
}

bool care_ticket_cancel(uint16_t id)
{
    care_ticket_t *tk;
    bool ok = false;

    pthread_mutex_lock(&s_lock);
    init_locked();

    tk = find_by_id_locked(id);
    if (tk != NULL && status_active(tk->status)) {
        tk->status = CARE_TICKET_CANCELLED;
        s_cancelled++;
        ok = true;
        syslog(LOG_INFO, "[%s] cancel id=%u type=%s\n",
               TAG, id, tk->type != NULL ? tk->type : "-");
    }

    pthread_mutex_unlock(&s_lock);

    return ok;
}

void care_ticket_sweep(uint32_t now_s)
{
    int i;

    pthread_mutex_lock(&s_lock);
    init_locked();

    for (i = 0; i < CARE_TICKET_MAX; i++) {
        care_ticket_t *tk = &s_tickets[i];

        if (status_active(tk->status) && now_s >= tk->expires_s) {
            tk->status = CARE_TICKET_EXPIRED;
            s_expired++;
            syslog(LOG_INFO,
                   "[%s] expired id=%u type=%s age=%us action=%s\n",
                   TAG, tk->id, tk->type != NULL ? tk->type : "-",
                   (unsigned)(now_s - tk->created_s),
                   tk->action != NULL ? tk->action : "-");
        }
    }

    pthread_mutex_unlock(&s_lock);
}

int care_ticket_count(void)
{
    int i;
    int n = 0;

    pthread_mutex_lock(&s_lock);
    init_locked();
    for (i = 0; i < CARE_TICKET_MAX; i++) {
        if (status_active(s_tickets[i].status)) {
            n++;
        }
    }
    pthread_mutex_unlock(&s_lock);

    return n;
}

void care_ticket_dump(void)
{
    uint32_t now = (uint32_t)mono_now_s();
    uint32_t created;
    uint32_t deduped;
    uint32_t expired;
    uint32_t cancelled;
    uint32_t next_id;
    int active;
    int i;

    pthread_mutex_lock(&s_lock);
    init_locked();

    active = 0;
    for (i = 0; i < CARE_TICKET_MAX; i++) {
        if (status_active(s_tickets[i].status)) {
            active++;
        }
    }
    created = s_created;
    deduped = s_deduped;
    expired = s_expired;
    cancelled = s_cancelled;
    next_id = s_next_id;

    printf("[care_ticket] active=%d created=%u deduped=%u cancelled=%u "
           "expired=%u next_id=%u\n",
           active, (unsigned)created, (unsigned)deduped,
           (unsigned)cancelled, (unsigned)expired, (unsigned)next_id);

    for (i = 0; i < CARE_TICKET_MAX; i++) {
        const care_ticket_t *tk = &s_tickets[i];

        if (tk->status == CARE_TICKET_FREE) {
            continue;
        }

        printf("[care_ticket] id=%u type=%s status=%s prio=%u "
               "age=%us ttl=%us action=%s\n",
               (unsigned)tk->id, tk->type != NULL ? tk->type : "-",
               status_name(tk->status), (unsigned)tk->priority,
               (unsigned)(now >= tk->created_s ? now - tk->created_s : 0),
               (unsigned)(tk->expires_s - tk->created_s),
               tk->action != NULL ? tk->action : "-");
    }

    pthread_mutex_unlock(&s_lock);
}

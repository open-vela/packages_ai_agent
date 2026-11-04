/****************************************************************************
 * alarm_page.c - 闹钟页（定时任务列表 + 新增）
 *
 * 数据源只有一处：infra/cron_service.c 里那张真实持久化的任务表
 * （/data/cron.json）。页面不自己存任何东西，也不补假数据 —— 表里没有就是
 * 空的。用户对手表说"明天早上7点叫我"走 tool_cron -> cron_add_job()，
 * 这里下一轮刷新就能看到。
 *
 * ★ 时间显示的前提（很容易看错，写清楚）：
 *   本构建 CONFIG_LIBC_LOCALTIME 没开，localtime_r() 实际上等于 gmtime()。
 *   但 infra/time_sync.c 在 clock_settime() 之前已经加了
 *   AGENT_UTC_OFFSET_SEC(+8h)，也就是系统时钟里存的本来就是**本地时间**。
 *   所以这里 localtime_r() 得到的 tm_hour/tm_min 就是墙上时间，和表盘一致，
 *   不要再减 8 小时。
 *
 * ★ 「+ 新增闹钟」就地展开编辑区，不新开页面：
 *   本构建没有 roller / dropdown / textarea / keyboard，时间只能靠
 *   LV_BUTTON + LV_LABEL 拼出来的 [-] HH [+] 调。
 *   点"确定"才调 cron_add_job()；id / enabled / last_run / next_run 全部由
 *   它内部生成（cron_generate_id / compute_initial_next_run / 落盘），
 *   调用方填了也会被覆盖，所以这里一个都不填。
 *
 * ★ 新增的时间换算（和上面显示用的假设是同一个）：
 *   time(NULL) 已经是本地墙钟，at_epoch = 今天 00:00 + HH:MM；
 *   若这个时刻已经过去、或离现在不到 60 秒，就顺延到明天。
 *
 * ★ 字体：正文一律用 UI_FONT_SMALL(cjk_18，全集)，不用 UI_FONT_BODY。
 *   cjk_24 是按 UI 用字烘焙的子集（347 字），"闹/定/任/以/表/明/叫/条/显/示"
 *   都不在里面，用 BODY 会画成缺字方块（CONFIG_LV_USE_FONT_PLACEHOLDER=y）。
 *   只有纯数字的时间用 BODY —— 数字落在子集里固定的 ASCII 32..126 区间。
 *
 * ★ 每行是"点一下就删"，没有二次确认：手表上没有放弹窗的地方，任务丢了
 *   用语音重新建即可。所以防误触必须做（ui_gesture_was_drag），否则想下滑
 *   返回时手指划过的行会被顺手删掉。新增区的按钮同理。
 *
 * ★ 一屏容量按 5 行排（原来 6 行）：底部多了一行「+ 新增闹钟」按钮。
 *   5×48 + 4×4 = 256，状态栏 32，提示行 8+21 = 29，
 *   按钮 8+44+24（底边留白）= 76，合计 393 < 410（450 - 上下各 20）；
 *   还按 6 行排会到 421，页面不能滚动，按钮底部会被裁掉。
 *   剩下的 17px 由一段 flex_grow 的空档吃掉。
 *
 *   编辑区展开时列表缩到 **2 行**（100），编辑区 218（多了标题两行），
 *   状态栏 32，列表与编辑区之间 32：32+100+32+218 = 382 < 410。
 *   ★ 原来列表留 3 行（152）+ 编辑区 176 是 372，但编辑区加到 218 后
 *     3 行会到 402、只剩 8px，还是贴底；收到 2 行后编辑区底边离屏幕
 *     底约 50px，才真的"不贴底"。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "alarm_page.h"
#include "expression.h"          /* 空态卡片里的当前表情图 */
#include "ui_gesture.h"          /* ui_gesture_was_drag: 点击防误触 */
#include "ui_nav.h"              /* ui_nav_attach_back_swipe: 下滑返回 */
#include "ui_theme.h"
#include "infra/cron_service.h"  /* cron_list_jobs / cron_remove_job / cron_add_job */

/****************************************************************************
 * 常量
 ****************************************************************************/

/* cron 服务上限 16 条（AGENT_CRON_MAX_JOBS）。一屏最多放 5 行，
 * 读 12 条是为了把"还有几条没显示"算准（超过 12 的极端情况按 12 计）。 */
#define ALARM_MAX_ROWS    12
#define ALARM_SHOW_ROWS   5

#define ALARM_ROW_H       48        /* 5×48 + 4×4 = 256，见文件头的版面账 */
#define ALARM_ROW_GAP     4
#define ALARM_TIME_W      76        /* "HH:MM" 固定宽度，各行的右边缘才能对齐 */
#define ALARM_NAME_BYTES  27        /* 约 9 个汉字，再长就截断加省略号 */
#define ALARM_ID_LEN      9         /* 同 cron_job_t.id：8 位十六进制 + NUL */

#define ALARM_ADD_H       44        /* 底部「+ 新增闹钟」按钮高 */
#define ALARM_ADD_GAP     24        /* 按钮距屏幕底边的呼吸位（原来贴底） */
#define ALARM_EDIT_ROWS   2         /* 编辑区展开时列表只留 2 行，见文件头 */
#define ALARM_EDIT_H      218       /* 标题21+21 + 48+48+44+21 + 5×2 = 213，留 5 */
#define ALARM_EDIT_ROW_H  48
#define ALARM_STEP_W      52        /* [-] / [+] 按钮宽 */
#define ALARM_STEP_H      44        /* 手指按得准，别做 40 以下 */
#define ALARM_EDIT_TIME_W 60        /* HH / MM 标签宽 */
#define ALARM_EDIT_ACT_W  112       /* 取消 / 确定 按钮宽 */
#define ALARM_EDIT_ACT_H  44        /* 取消 / 确定 按钮高 */

/* 编辑区按钮的 user_data 编码：一个回调管四个加减键 */
#define ALARM_ADJ_HOUR_DOWN  1
#define ALARM_ADJ_HOUR_UP    2
#define ALARM_ADJ_MIN_DOWN   3
#define ALARM_ADJ_MIN_UP     4

#define ALARM_REFRESH_MS  10000     /* 10 秒：next_run 只精确到分钟，1 秒刷没意义 */

/****************************************************************************
 * 私有数据
 ****************************************************************************/

static lv_obj_t   *s_page;
static lv_obj_t   *s_list;
static lv_obj_t   *s_empty;
static lv_obj_t   *s_hint;
/* 顺延提示的暂存：refresh() 每 10s 会重写 s_hint，若不暂存，
 * "已顺延到明天 HH:MM"活不过一帧就被"轻点一行即可删除"盖掉。
 * 非空时 refresh 显示它；显示满 8s（一次刷新周期）后自动让位。 */
static char        s_defer_note[48];
static uint32_t    s_defer_until;
static lv_obj_t   *s_spacer;    /* 弹性空档：把底部按钮顶到屏幕底边 */
static lv_obj_t   *s_row[ALARM_SHOW_ROWS];
static lv_obj_t   *s_row_name[ALARM_SHOW_ROWS];
static lv_obj_t   *s_row_time[ALARM_SHOW_ROWS];
static char        s_row_id[ALARM_SHOW_ROWS][ALARM_ID_LEN];
static lv_timer_t *s_tick;

/* 内联新增区。对象在 create() 里一次建好，之后只改文字/显隐，不在回调里
 * 删对象 —— 和行是同一套做法，避免在"自己的 CLICKED 回调"里释放自己。 */
static lv_obj_t   *s_add_btn;
static lv_obj_t   *s_editor;
static lv_obj_t   *s_edit_hh;
static lv_obj_t   *s_edit_mm;
static lv_obj_t   *s_edit_err;
static int         s_edit_hour;
static int         s_edit_min;
static bool        s_edit_open;

/* 行是"建好 5 行，刷新时只改文字和显隐"，不在事件回调里删对象。
 * 因为点一行要删任务并**立刻**刷新，如果刷新走 lv_obj_clean()，就会在
 * "这一行自己的 CLICKED 回调"里把这一行释放掉 —— 没必要冒这个险。
 *
 * 这个数组必须放静态区：cron_job_t 一个约 700 字节，12 个就是 8.4KB。
 * refresh() 跑在渲染线程（RENDER_STACK=32KB）的定时器里，放栈上会把
 * 渲染线程的余量吃掉一大半。 */
static cron_job_t  s_jobs[ALARM_MAX_ROWS];

/****************************************************************************
 * 刷新
 ****************************************************************************/

static void refresh(void);
static void edit_reset_time(void);
static void edit_update_labels(void);
static void edit_open(void);
static void edit_close(void);

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh();
}

/* 名字：优先 name；name 为空时退回 message 的前若干字。
 * 按 UTF-8 边界截断 —— 从字节中间切开，屏上会画成半个方块。 */
static void make_label_text(const cron_job_t *job, char *out, size_t out_size)
{
    const char *src;
    size_t n = 0;

    src = (job->name[0] != '\0') ? job->name : job->message;

    if (src[0] == '\0')
      {
        /* 名字和正文都是空：留个占位文字，别让这一行看起来像坏了 */
        snprintf(out, out_size, "定时任务");
        return;
      }

    while (src[n] != '\0' && n < ALARM_NAME_BYTES && n + 1 < out_size)
      {
        n++;
      }

    /* 越界位置若是续字节(10xxxxxx)，说明最后一个汉字被切开了，退到字符起点 */
    while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80)
      {
        n--;
      }

    memcpy(out, src, n);
    out[n] = '\0';

    /* 后面还有内容：加省略号，让用户知道名字没显示全 */
    if (src[n] != '\0' && n + 3 < out_size)
      {
        strcpy(out + n, "…");
      }
}

/* 下次触发时间。
 * next_run <= 0 表示"没有安排"（一次性任务响过以后 cron 会把它置 0），
 * next_run <= now 表示已经过期但还没来得及处理。两种都显示 "--"，
 * 而不是显示一个已经过去的时刻骗人。 */
static void make_time_text(int64_t next_run, time_t now_init, char *out,
                           size_t out_size)
{
    struct tm tmv;
    time_t t;

    if (next_run <= 0 || next_run <= (int64_t)now_init)
      {
        snprintf(out, out_size, "--");
        return;
      }

    t = (time_t)next_run;

    if (localtime_r(&t, &tmv) == NULL)
      {
        snprintf(out, out_size, "--");
        return;
      }

    snprintf(out, out_size, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* 列表高度按"实际显示几行"定死，不用 LV_SIZE_CONTENT。
 * 这个仓踩过：flex 容器里再套一个 LV_SIZE_CONTENT 的容器，尺寸会被
 * 算成 0（见 ui_theme.c 的 statusbar 注释），列表整块消失。 */
static void apply_list_height(int shown)
{
    if (shown <= 0)
      {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        return;
      }

    lv_obj_set_height(s_list, shown * ALARM_ROW_H +
                              (shown - 1) * ALARM_ROW_GAP);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
}

/* 弹性空档只在"列表有内容"或"编辑区展开"时出现：
 *   - 列表为空：让位给 s_empty（它自己 flex_grow，居中提示，按钮照样在底部）
 *   - 编辑区展开：吃掉列表和编辑区之间的余量，把编辑区顶到屏幕底部 */
static void set_spacer_visible(bool on)
{
    if (s_spacer == NULL)
      {
        return;
      }

    if (on)
      {
        lv_obj_clear_flag(s_spacer, LV_OBJ_FLAG_HIDDEN);
      }
    else
      {
        lv_obj_add_flag(s_spacer, LV_OBJ_FLAG_HIDDEN);
      }
}

static void refresh(void)
{
    char name[32];
    char timebuf[8];
    char hint[48];
    time_t now_init;
    int max_rows;
    int count;
    int shown;
    int i;

    if (s_page == NULL || s_list == NULL)
      {
        return;
      }

    count = cron_list_jobs(s_jobs, ALARM_MAX_ROWS);

    if (count < 0)
      {
        count = 0;
      }

    /* 编辑区展开时列表让位：只留前 2 行，保证编辑区完整落在屏内 */
    max_rows = s_edit_open ? ALARM_EDIT_ROWS : ALARM_SHOW_ROWS;
    shown = (count > max_rows) ? max_rows : count;
    now_init = time(NULL);

    for (i = 0; i < ALARM_SHOW_ROWS; i++)
      {
        if (i >= shown)
          {
            /* 空位藏起来：flex 布局会跳过 hidden 的子对象，不占高度 */
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            s_row_id[i][0] = '\0';
            continue;
          }

        make_label_text(&s_jobs[i], name, sizeof(name));
        make_time_text(s_jobs[i].next_run, now_init, timebuf, sizeof(timebuf));

        lv_label_set_text(s_row_name[i], name);
        lv_label_set_text(s_row_time[i], timebuf);

        /* 点这一行时要用 id，拷到静态槽里；行对象只记槽位指针，不记 id 本身 */
        snprintf(s_row_id[i], sizeof(s_row_id[i]), "%s", s_jobs[i].id);

        /* enabled=false 的任务不是"没有"，只是被关了：用第三级色标出来，
         * 但这一行仍然可点（点一下就是删掉它）。 */
        lv_obj_set_style_text_color(
            s_row_name[i],
            lv_color_hex(s_jobs[i].enabled ? UI_TEXT_PRIMARY
                                           : UI_TEXT_TERTIARY), 0);
        lv_obj_set_style_text_color(
            s_row_time[i],
            lv_color_hex(s_jobs[i].enabled ? UI_TEXT_SECONDARY
                                           : UI_TEXT_TERTIARY), 0);

        lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
      }

    /* 编辑区展开时列表高度固定按 2 行预留（即使实际只有 0~1 条）：
     * 编辑区的纵向位置不会随条目多少跳动，也保证它落在屏幕下半部，
     * 而不是在 0 条时贴着状态栏。 */
    apply_list_height(s_edit_open ? ALARM_EDIT_ROWS : shown);

    if (s_edit_open)
      {
        /* 编辑区占了下半屏：空列表提示和底部说明都收起来。
         * ★ 编辑区展开时**不再**让空档 flex_grow：那会把编辑区顶到
         *   页面底边（正是这次要修的"贴底"）。空档藏起来，编辑区靠
         *   make_editor() 里固定的 margin_top 32 与列表拉开距离，
         *   剩余高度留在编辑区下方。 */
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        set_spacer_visible(false);
        return;
      }

    /* 空列表：整块居中的提示顶上来，其它东西让位 */
    if (shown == 0)
      {
        lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hint, "");
        set_spacer_visible(false);
        return;
      }

    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    set_spacer_visible(true);

    /* 顺延提示优先：未过期就显示它，过期后清空走常规提示 */
    if (s_defer_note[0] != '\0')
      {
        if (lv_tick_get() < s_defer_until)
          {
            lv_label_set_text(s_hint, s_defer_note);
            lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
            return;
          }

        s_defer_note[0] = '\0';
      }

    if (count > max_rows)
      {
        /* 只显示了前几行，剩下的必须说出来，不然用户以为任务丢了 */
        snprintf(hint, sizeof(hint), "还有 %d 条未显示（最多 %d 条）",
                 count - max_rows, max_rows);
      }
    else
      {
        snprintf(hint, sizeof(hint), "轻点一行即可删除");
      }

    lv_label_set_text(s_hint, hint);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

/****************************************************************************
 * 点击行：删除任务 + 立刻刷新
 ****************************************************************************/

static void on_row_clicked(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    int err;

    /* 防误触：从这一行上开始上下滑（多半是想下滑返回）不该删任务。
     * 标记由 ui_gesture 在 PRESSING 阶段就置上，一定早于 CLICKED。 */
    if (ui_gesture_was_drag())
      {
        return;
      }

    if (id == NULL || id[0] == '\0')
      {
        return;
      }

    /* 注意：这里会拿 cron 的互斥锁并落盘（/data/cron.json），UI 线程可能
     * 被阻塞几十毫秒。删除是低频操作，接受这个代价，换来的是"点了就真存"。 */
    err = cron_remove_job(id);

    printf("[Alarm] remove %s -> %d\n", id, err);

    /* 不等到 10 秒定时器：点完必须马上看到结果。
     * 这里只改文字/显隐，不删对象，所以可以在自己的回调里安全刷新。 */
    refresh();
}

/****************************************************************************
 * 内联新增区：状态
 ****************************************************************************/

/* 默认值：当前时间的下一个整 5 分钟。
 * 用 (sod / 300 + 1) * 300：现在正好踩在 5 的整数倍上时也取下一个，
 * 免得用户一展开就看到一个"已经过去 0 秒"的时间点。 */
static void edit_reset_time(void)
{
    time_t now = time(NULL);
    int sod;
    int t5;

    /* time(NULL) 已经是本地墙钟（time_sync 提前加了 +8h），不要再减 8 小时 */
    sod = (int)(now % 86400);
    if (sod < 0)
      {
        sod += 86400;   /* 防御：1970 以前不会出现，但不让下标变负 */
      }

    t5 = ((sod / 300) + 1) * 300;
    if (t5 >= 86400)
      {
        t5 -= 86400;    /* 23:56 -> 00:00 */
      }

    s_edit_hour = t5 / 3600;
    s_edit_min  = (t5 % 3600) / 60;
}

static void edit_update_labels(void)
{
    char buf[8];

    if (s_edit_hh == NULL || s_edit_mm == NULL)
      {
        return;
      }

    snprintf(buf, sizeof(buf), "%02d", s_edit_hour);
    lv_label_set_text(s_edit_hh, buf);

    snprintf(buf, sizeof(buf), "%02d", s_edit_min);
    lv_label_set_text(s_edit_mm, buf);
}

static void edit_open(void)
{
    if (s_editor == NULL)
      {
        return;
      }

    /* 每次展开都重取默认时间：上次取消时改到一半的值不该留下 */
    edit_reset_time();
    edit_update_labels();

    s_edit_open = true;

    lv_label_set_text(s_edit_err, "");
    lv_obj_clear_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_add_btn, LV_OBJ_FLAG_HIDDEN);

    refresh();   /* 列表缩到 2 行，给编辑区让高度 */
}

static void edit_close(void)
{
    s_edit_open = false;

    if (s_editor != NULL)
      {
        lv_label_set_text(s_edit_err, "");
        lv_obj_add_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
      }

    if (s_add_btn != NULL)
      {
        lv_obj_clear_flag(s_add_btn, LV_OBJ_FLAG_HIDDEN);
      }

    refresh();   /* 列表恢复 5 行，提示行回来 */
}

/****************************************************************************
 * 内联新增区：事件
 ****************************************************************************/

static void on_add_clicked(lv_event_t *e)
{
    (void)e;

    if (ui_gesture_was_drag())
      {
        return;
      }

    edit_open();
}

static void on_edit_cancel(lv_event_t *e)
{
    (void)e;

    if (ui_gesture_was_drag())
      {
        return;
      }

    edit_close();
}

/* 四个加减键共用一个回调，具体动作由 user_data 编码 */
static void on_edit_adjust(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);

    if (ui_gesture_was_drag())
      {
        return;
      }

    switch (code)
      {
        case ALARM_ADJ_HOUR_DOWN:
          s_edit_hour = (s_edit_hour + 23) % 24;   /* 0 -> 23 */
          break;

        case ALARM_ADJ_HOUR_UP:
          s_edit_hour = (s_edit_hour + 1) % 24;    /* 23 -> 0 */
          break;

        case ALARM_ADJ_MIN_DOWN:
          s_edit_min = (s_edit_min + 55) % 60;     /* 按 5 分钟步进，0 -> 55 */
          break;

        case ALARM_ADJ_MIN_UP:
          s_edit_min = (s_edit_min + 5) % 60;      /* 55 -> 0 */
          break;

        default:
          return;
      }

    edit_update_labels();
}

/* 点"确定"：构造一条一次性任务交给 cron。
 *
 * 只填调用方该填的字段；id / enabled / last_run / next_run 由 cron_add_job()
 * 内部生成（cron_generate_id / compute_initial_next_run / cron_save_jobs），
 * 这里填了也会被覆盖，所以一个都不填。 */
static void on_edit_confirm(lv_event_t *e)
{
    cron_job_t job;
    time_t now;
    int sod_now;
    int sod_tgt;
    int err;
    bool deferred;

    (void)e;

    if (ui_gesture_was_drag())
      {
        return;
      }

    memset(&job, 0, sizeof(job));

    snprintf(job.name, sizeof(job.name), "%s", "闹钟");
    job.kind = CRON_KIND_AT;
    job.delete_after_run = true;

    /* at_epoch：今天 HH:MM；已过（或离现在不到 20 秒）就顺延到明天。
     * time(NULL) 的值本身就是本地墙钟，不要再减 8 小时。
     *
     * ★ 2026-10-25 真机踩坑：余量原为 60 秒 —— 用户 11:59:39 设 12:00
     *   （21 秒后）被顺延到明天，观感就是"闹钟没响"。60→20：20 秒
     *   足够覆盖提交耗时，又不会吞掉"一分钟内"的合理设置。
     *   顺延发生时下面会把"明天"明确提示出来，不再无声无息。 */
    now = time(NULL);
    sod_now = (int)(now % 86400);
    sod_tgt = s_edit_hour * 3600 + s_edit_min * 60;
    deferred = false;
    if (sod_tgt <= sod_now + 20)
      {
        sod_tgt += 86400;
        deferred = true;
      }

    job.at_epoch = (int64_t)(now - sod_now) + (int64_t)sod_tgt;

    snprintf(job.message, sizeof(job.message), "%s", "闹钟");
    snprintf(job.channel, sizeof(job.channel), "system");
    snprintf(job.chat_id, sizeof(job.chat_id), "cron");

    /* 会拿 cron 互斥锁并写 /data/cron.json；低频操作，接受这点阻塞 */
    err = cron_add_job(&job);

    if (err != OK)
      {
        /* 不能静默：最多条数已满时 cron_add_job 只返回 ERROR，不提示用户 */
        syslog(LOG_WARNING, "[Alarm] add %02d:%02d failed (%d)\n",
               s_edit_hour, s_edit_min, err);
        lv_label_set_text(s_edit_err, "添加失败");
        return;
      }

    /* 验收用：一行能看到"加了哪个时刻、拿到什么 id、at_epoch 算成多少"。
     * deferred 时多打一个 tomorrow，和屏幕提示一致。 */
    syslog(LOG_INFO, "[Alarm] add %02d:%02d%s at=%lld id=%s\n",
           s_edit_hour, s_edit_min, deferred ? " (tomorrow)" : "",
           (long long)job.at_epoch, job.id);

    /* 顺延必须让用户知道，否则就是"我设了闹钟但它没响"。
     * 写进暂存：edit_close() 会立刻 refresh()，直接写 s_hint 会被
     * 常规提示盖掉；暂存能在 refresh 的输出里活到下一个刷新周期。 */
    if (deferred)
      {
        snprintf(s_defer_note, sizeof(s_defer_note),
                 "已顺延到明天 %02d:%02d", s_edit_hour, s_edit_min);
        s_defer_until = lv_tick_get() + 8000;
      }

    edit_close();
}

/****************************************************************************
 * 布局
 ****************************************************************************/

/* 胶囊按钮：自己拼 LV_BUTTON + LV_LABEL。
 * 不用 ui_button_create() 的原因和天气页的"刷新"一样：它把文字定成
 * UI_FONT_BODY(24px 子集)，"取消/确定/新增/闹钟"这些字在子集里没有。 */
static lv_obj_t *make_pill_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                  const char *text, const lv_font_t *font,
                                  uint32_t color, lv_event_cb_t cb, int code)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* 按下时压暗一点，抬手前先让用户看见"这一下被接住了" */
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_SURFACE_HI),
                              LV_STATE_PRESSED);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)code);

    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    /* 底色既有浅色（UI_SURFACE_HI 的 -/+ 与"取消"）又有深色（UI_ACCENT
     * 的"确定"与"+ 新增闹钟"）。写死 UI_TEXT_PRIMARY 会让深色按钮上的
     * 文字掉到 2.55:1，所以按底色自动选（2026-11-02）。 */
    lv_obj_set_style_text_color(lbl, lv_color_hex(ui_text_on_bg(color)), 0);
    lv_obj_center(lbl);

    return btn;
}

/* 一行时间：  时： [-] HH [+] */
static void make_time_row(lv_obj_t *parent, const char *name,
                          lv_obj_t **value_out, int down_code, int up_code)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *lbl;

    lv_obj_set_size(row, lv_pct(100), ALARM_EDIT_ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, UI_GAP_S, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_SECONDARY), 0);

    /* +/- 是 ASCII，cjk_24 里 ASCII 32..126 是完整的一段，可以用 BODY */
    make_pill_button(row, ALARM_STEP_W, ALARM_STEP_H, "-", UI_FONT_BODY,
                     UI_SURFACE_HI, on_edit_adjust, down_code);

    /* HH / MM：纯数字，可以用 24px 的 BODY 当视觉锚点 */
    *value_out = lv_label_create(row);
    lv_obj_set_width(*value_out, ALARM_EDIT_TIME_W);
    lv_obj_set_style_text_align(*value_out, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(*value_out, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(*value_out, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_label_set_text(*value_out, "00");

    make_pill_button(row, ALARM_STEP_W, ALARM_STEP_H, "+", UI_FONT_BODY,
                     UI_SURFACE_HI, on_edit_adjust, up_code);
}

/* 编辑区整体：标题两行 + 两行时间 + 取消/确定 + 一行错误提示。
 * 高度写死（ALARM_EDIT_H），不用 LV_SIZE_CONTENT：本仓踩过 flex 里套
 * LV_SIZE_CONTENT 被算成 0 的坑。 */
static void make_editor(lv_obj_t *parent)
{
    lv_obj_t *title;
    lv_obj_t *sub;
    lv_obj_t *actions;

    s_editor = lv_obj_create(parent);
    lv_obj_set_width(s_editor, lv_pct(100));
    lv_obj_set_height(s_editor, ALARM_EDIT_H);
    lv_obj_set_style_bg_opa(s_editor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_editor, 0, 0);
    lv_obj_set_style_pad_all(s_editor, 0, 0);
    /* 2 而不是 ALARM_ROW_GAP(4)：218 里要装 6 个块，4×5 会多出 10px */
    lv_obj_set_style_pad_row(s_editor, 2, 0);
    /* 只在展开时有意义（HIDDEN 的对象不参与 flex 布局）：32px 让编辑区
     * 不贴着列表，也不贴页面底边。 */
    lv_obj_set_style_margin_top(s_editor, 32, 0);
    lv_obj_clear_flag(s_editor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_editor, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_editor, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 标题：「新增闹钟」+ 说清楚这是一次性的。
     * 用户点"确定"后 cron 建的是 CRON_KIND_AT + delete_after_run，
     * 到点响一次就从表里删掉，和"重复闹钟"不是一回事，必须写在脸上。 */
    title = lv_label_create(s_editor);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_label_set_text(title, "新增闹钟");

    sub = lv_label_create(s_editor);
    lv_obj_set_width(sub, lv_pct(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sub, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_label_set_text(sub, "仅一次 · 到点响一次");

    make_time_row(s_editor, "时：", &s_edit_hh,
                  ALARM_ADJ_HOUR_DOWN, ALARM_ADJ_HOUR_UP);
    make_time_row(s_editor, "分：", &s_edit_mm,
                  ALARM_ADJ_MIN_DOWN, ALARM_ADJ_MIN_UP);

    actions = lv_obj_create(s_editor);
    lv_obj_set_size(actions, lv_pct(100), ALARM_EDIT_ACT_H);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_pill_button(actions, ALARM_EDIT_ACT_W, ALARM_EDIT_ACT_H, "取消",
                     UI_FONT_SMALL, UI_SURFACE_HI, on_edit_cancel, 0);
    make_pill_button(actions, ALARM_EDIT_ACT_W, ALARM_EDIT_ACT_H, "确定",
                     UI_FONT_SMALL, UI_ACCENT, on_edit_confirm, 0);

    /* 失败提示：平时是空串，占着 21px 的行高，失败时不用重排布局 */
    s_edit_err = lv_label_create(s_editor);
    lv_obj_set_width(s_edit_err, lv_pct(100));
    lv_obj_set_style_text_align(s_edit_err, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_edit_err, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_edit_err, lv_color_hex(UI_COLOR_WARNING), 0);
    lv_label_set_text(s_edit_err, "");

    lv_obj_add_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
}

static void make_row(lv_obj_t *parent, int idx)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *name;
    lv_obj_t *tm;

    lv_obj_set_size(row, lv_pct(100), ALARM_ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, UI_RADIUS_ROW, 0);
    lv_obj_set_style_pad_hor(row, UI_GAP_S, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 按下时给一层浅底：抬手才真删，先让用户看见"这一行被按住了" */
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_SURFACE_HI),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, s_row_id[idx]);

    /* 左：任务名。用 cjk_18 全集，任务名是任意中文，子集字体画不出来 */
    name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_style_text_font(name, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_label_set_text(name, "");

    /* 右：下次触发时间。纯数字，可以用 24px 的 BODY 当视觉锚点 */
    tm = lv_label_create(row);
    lv_obj_set_width(tm, ALARM_TIME_W);
    lv_obj_set_style_text_align(tm, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(tm, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(tm, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_label_set_text(tm, "--");

    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

    s_row[idx]      = row;
    s_row_name[idx] = name;
    s_row_time[idx] = tm;
}

/* 空列表：一张 350x160 的卡（当前表情 + 两行字），卡外再一行语音说明。
 * 撑满剩余高度再居中，否则会挤在状态栏下面。
 *
 * 表情图是编译期烘焙的常量（expression_get 只在 LVGL 线程读），
 * 和表盘/小云页共用同一份资产，不新增 flash。 */
static void make_empty(lv_obj_t *parent)
{
    const expression_t *ex;
    lv_obj_t *card;
    lv_obj_t *box;
    lv_obj_t *img;
    lv_obj_t *t1;
    lv_obj_t *t2;
    lv_obj_t *t3;

    s_empty = lv_obj_create(parent);
    lv_obj_set_width(s_empty, lv_pct(100));
    lv_obj_set_flex_grow(s_empty, 1);
    lv_obj_set_style_bg_opa(s_empty, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_empty, 0, 0);
    lv_obj_set_style_pad_all(s_empty, 0, 0);
    lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_empty, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 卡片高度 160：表情最大 94 + 两行 21+21 + 间距 8 + 内边距 8 = 152 */
    card = ui_card_create(s_empty, 350, 160);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_set_style_pad_row(card, 4, 0);

    /* ★ lv_image_set_scale() 只改绘制变换，**不改对象的布局尺寸**
     *   （GET_SELF_SIZE 仍返回 120x103）。直接塞进 flex 列会把 160 的卡
     *   撑破，所以套一个"缩放后尺寸"的透明盒、图在盒里居中——
     *   和 pet_page.c 里云朵图的做法一致。 */
    ex  = expression_get(expression_current());
    box = lv_obj_create(card);
    lv_obj_set_size(box, ex->img->header.w * 200 / 256,
                    ex->img->header.h * 200 / 256);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    img = lv_image_create(box);
    lv_image_set_src(img, ex->img);
    lv_image_set_scale(img, 200);   /* 120px 长边 → 约 94px（256 = 1x） */
    lv_obj_center(img);

    t1 = lv_label_create(card);
    lv_label_set_text(t1, "还没有闹钟");
    lv_obj_set_style_text_font(t1, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(t1, lv_color_hex(UI_TEXT_PRIMARY), 0);

    t2 = lv_label_create(card);
    lv_label_set_text(t2, "点下方按钮新增");
    lv_obj_set_style_text_font(t2, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(t2, lv_color_hex(UI_TEXT_TERTIARY), 0);

    /* ★ 原来写"可以对手表说：明天早上7点叫我"是错的：语音提醒走
     *   pet_care 的倒计时，不写 cron 表，本页永远看不到。改成说明事实，
     *   免得用户说完回来发现列表还是空的。
     *   卡片 160 高塞不进第三行，所以放在卡下面。 */
    t3 = lv_label_create(s_empty);
    lv_obj_set_width(t3, lv_pct(100));
    lv_label_set_long_mode(t3, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(t3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(t3, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(t3, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(t3, UI_GAP_S, 0);
    lv_label_set_text(t3, "语音说的提醒不会显示在这里");
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *alarm_page_create(void)
{
    int i;

    if (s_page != NULL)
      {
        return s_page;
      }

    s_page = lv_obj_create(lv_screen_active());
    ui_page_init(s_page);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 同 sleep_page：默认主题的 10px flex 行距会让 5 行时总高 434 超出
     * 内容区 410（按钮底边距被压到 20px）。显式清零，行距由子项 margin 管。 */
    lv_obj_set_style_pad_row(s_page, 0, 0);

    (void)ui_statusbar_create(s_page, "闹钟");

    /* 列表容器：最多 5 行固定建好，刷新只改文字/显隐。
     * 一屏 5 行是算出来的，不是拍的：可用高 410 - 状态栏 32 = 378；
     * 5 行 × 48 + 4 × 4 = 256，提示行 8 + 21 = 29，
     * 「+ 新增闹钟」8 + 44 + 24（底边留白）= 76，合计 393，
     * 剩下 17 交给下面的空档。 */
    s_list = lv_obj_create(s_page);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_height(s_list, 0);   /* 真实高度由 refresh() 按行数算 */
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, ALARM_ROW_GAP, 0);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (i = 0; i < ALARM_SHOW_ROWS; i++)
      {
        make_row(s_list, i);
      }

    s_hint = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(s_hint, UI_GAP_S, 0);
    lv_label_set_text(s_hint, "");
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    /* 顺序有讲究：空列表提示 → 弹性空档 → 按钮 → 编辑区。
     * 空档把按钮顶到屏幕底部，而不是让它顶在状态栏下面。 */
    make_empty(s_page);

    /* 弹性空档：列表有内容时由它吃掉余量，把按钮顶到屏幕底边。
     * 固定 1px 高 + flex_grow，不用 LV_SIZE_CONTENT（本仓踩过算成 0 的坑），
     * 和 weather_page.c 把"刷新"按钮顶到底部是同一个做法。 */
    s_spacer = lv_obj_create(s_page);
    lv_obj_set_width(s_spacer, lv_pct(100));
    lv_obj_set_height(s_spacer, 1);
    lv_obj_set_flex_grow(s_spacer, 1);
    lv_obj_set_style_bg_opa(s_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_spacer, 0, 0);
    lv_obj_set_style_pad_all(s_spacer, 0, 0);
    lv_obj_clear_flag(s_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_spacer, LV_OBJ_FLAG_HIDDEN);

    s_add_btn = make_pill_button(s_page, 180, ALARM_ADD_H, "+ 新增闹钟",
                                 UI_FONT_SMALL, UI_ACCENT, on_add_clicked, 0);
    lv_obj_set_style_margin_top(s_add_btn, UI_GAP_S, 0);
    /* 原来按钮压着屏幕底边（只靠页面 20px padding），手指在圆角处很别扭 */
    lv_obj_set_style_margin_bottom(s_add_btn, ALARM_ADD_GAP, 0);

    make_editor(s_page);

    /* 必须挂手势：不调它页面就滑不出去（见 sport_page.c 里同一条注释） */
    ui_nav_attach_back_swipe(s_page);

    /* 编辑区初始收起，时间先填"下一个整 5 分钟" */
    edit_reset_time();
    edit_update_labels();

    refresh();

    s_tick = lv_timer_create(refresh_cb, ALARM_REFRESH_MS, NULL);

    printf("[Alarm] page created\n");
    return s_page;
}

void alarm_page_delete(void)
{
    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    if (s_tick != NULL)
      {
        lv_timer_delete(s_tick);
        s_tick = NULL;
      }

    /* 这些指针都指向 s_page 的子对象，必须和页面一起作废 */
    s_list     = NULL;
    s_empty    = NULL;
    s_hint     = NULL;
    s_defer_note[0] = '\0';
    s_defer_until   = 0;
    s_spacer   = NULL;
    s_add_btn  = NULL;
    s_editor   = NULL;
    s_edit_hh  = NULL;
    s_edit_mm  = NULL;
    s_edit_err = NULL;

    /* 编辑区状态复位：下次进来是收起状态，时间重新取"下一个 5 分钟" */
    s_edit_open = false;
    s_edit_hour = 0;
    s_edit_min  = 0;

    memset(s_row,      0, sizeof(s_row));
    memset(s_row_name, 0, sizeof(s_row_name));
    memset(s_row_time, 0, sizeof(s_row_time));
    memset(s_row_id,   0, sizeof(s_row_id));
}

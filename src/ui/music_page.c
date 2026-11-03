/****************************************************************************
 * music_page.c - 音乐页
 *
 * ★ 最重要的约束（2026-10-21 真机冻死一次之后定的）：
 *
 *   **这个文件的每一个回调都跑在 LVGL 渲染线程上，绝不允许调用
 *   tool_music_*_execute() 或任何会阻塞的播放器接口。**
 *
 *   事故经过：早期版本在 on_local_clicked 里直接调 tool_music_play_execute()，
 *   而它内部要 media_player_open -> prepare(等 PREPARED) -> start(等 STARTED)，
 *   每一步都有超时。一旦卡在某一步，冻住的不是音乐，是**整个 UI 线程**：
 *   画面不再刷新，最后被看门狗复位。1 秒一次的 music_refresh 也在 UI 线程
 *   上，而 status 要拿 g_media 锁 —— 播放占着锁时同样会把它堵死。
 *
 *   现在的结构：
 *     UI 线程  -> music_worker_post()  只入队，立刻返回
 *     工作线程 -> 真正执行 tool_music_*_execute()
 *     工作线程 -> 每秒刷新一份状态快照
 *     UI 线程  -> music_worker_snapshot() 只读内存拷贝
 *
 *   所以播放器再卡，界面也照常刷新。
 *
 * 另外两个实现约束：
 *   1) 本构建把 LV_USE_BAR/SLIDER/ARC 全关了，进度条只能用两个普通 lv_obj 拼。
 *   2) 不在这个页面里发起播放以外的音频逻辑，一切复用 tools/tool_media.c。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>

#include "music_page.h"
#include "music_local.h"
#include "music_worker.h"
#include "voice/pcm_file_player.h"   /* music_page_delete 里要停播 */
#include "ui_gesture.h"      /* 挂手势：零清防误触状态 */
#include "ui_nav.h"          /* ui_nav_attach_back_swipe：下滑返回 */
#include "ui_theme.h"

/****************************************************************************
 * 私有数据
 ****************************************************************************/

#define BAR_W        300
#define BAR_H        6
#define SEEK_STEP_MS 10000
#define VOL_STEP     10

static lv_obj_t *s_page;
static lv_obj_t *s_state_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_bar_bg;
static lv_obj_t *s_bar_fill;
static lv_obj_t *s_toggle_btn;
static lv_obj_t *s_vol_label;
static lv_obj_t *s_hint_label;
static lv_timer_t *s_timer;

static int s_volume;          /* 最近一次从快照读到的音量 */
static int s_pos_ms;          /* 最近一次快照的位置，供 seek 用 */

/* 本地音乐列表：每行一个文件名，点一下直接播。
 * 在线搜索受版权限制经常放不出来，本地文件是唯一能保证出声的路径。 */
static char s_local_names[MUSIC_LOCAL_MAX][MUSIC_LOCAL_NAMELEN];
static int  s_local_count;

/****************************************************************************
 * 刷新（只读快照，绝不阻塞）
 ****************************************************************************/

static void fmt_ms(char *out, size_t sz, int ms)
{
    int sec;

    if (ms < 0)
      {
        ms = 0;
      }

    sec = ms / 1000;
    snprintf(out, sz, "%02d:%02d", sec / 60, sec % 60);
}

static void music_refresh(void)
{
    music_snapshot_t snap;
    char t_pos[8];
    char t_dur[8];
    char line[40];
    int dur;
    int pct;

    music_worker_snapshot(&snap);

    if (!snap.valid)
      {
        return;          /* 还没取到过状态：保持上次显示，不要闪成 -- */
      }

    s_volume = snap.volume;
    s_pos_ms = snap.position_ms;
    dur      = snap.duration_ms;

    if (strcmp(snap.state, "PLAYING") == 0 ||
        strcmp(snap.state, "PREPARED") == 0)
      {
        lv_label_set_text(s_state_label, "播放中");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(UI_ACCENT), 0);
        ui_button_set_text(s_toggle_btn, "停止");
      }
    else if (strcmp(snap.state, "PAUSED") == 0)
      {
        /* 暂停/续播已临时下线（真机音频设备在续播路径上不稳定），
         * 万一还有残留状态，统一按"未播放"处理，按钮变回播放。 */
        lv_label_set_text(s_state_label, "未播放");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
        ui_button_set_text(s_toggle_btn, "播放");
      }
    else if (strcmp(snap.state, "ERROR") == 0)
      {
        lv_label_set_text(s_state_label, "播放出错");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(UI_COLOR_WARNING), 0);
        ui_button_set_text(s_toggle_btn, "播放");
      }
    else
      {
        lv_label_set_text(s_state_label, "未播放");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
        ui_button_set_text(s_toggle_btn, "播放");
      }

    if (dur > 0)
      {
        fmt_ms(t_pos, sizeof(t_pos), snap.position_ms);
        fmt_ms(t_dur, sizeof(t_dur), dur);
        snprintf(line, sizeof(line), "%s / %s", t_pos, t_dur);
        lv_label_set_text(s_time_label, line);

        pct = (int)((long)snap.position_ms * 100 / dur);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_obj_set_width(s_bar_fill, BAR_W * pct / 100);
      }
    else
      {
        lv_label_set_text(s_time_label, "--:--");
        lv_obj_set_width(s_bar_fill, 0);
      }

    snprintf(line, sizeof(line), "音量 %d%%", s_volume);
    lv_label_set_text(s_vol_label, line);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    music_refresh();
}

/****************************************************************************
 * 本地音乐列表
 ****************************************************************************/

static void on_local_clicked(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    char path[MUSIC_LOCAL_NAMELEN + 32];
    music_snapshot_t snap;

    if (name == NULL)
      {
        return;
      }

    /* ★ 防连点。
     *
     * 以前每一次点击都无条件 music_worker_post(PLAY, ...)，快速连点几下
     * 就会有多个播放请求排队（队列深度 8），这是 2026-10-21 那次死机的
     * 直接触发条件。播放器层已经加了单例闸门，这里再从 UI 侧挡一道：
     * 已经在放同一首时重复点击直接忽略。 */
    music_worker_snapshot(&snap);
    if (snap.valid && strcmp(snap.state, "PLAYING") == 0)
      {
        lv_label_set_text(s_hint_label, "已经在播放了");
        return;
      }

    snprintf(path, sizeof(path), MUSIC_LOCAL_DIR "/%s", name);

    /* 只入队。真正的 media_player_open/prepare/start 在工作线程里跑，
     * 它卡多久都不会影响这里的界面刷新。 */
    (void)music_worker_post(MUSIC_CMD_PLAY, path);

    lv_label_set_text(s_state_label, "载入中…");
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
}

static void local_list_build(lv_obj_t *parent)
{
    int i;

    s_local_count = music_local_scan(s_local_names, MUSIC_LOCAL_MAX);

    if (s_local_count == 0)
      {
        lv_obj_t *empty = lv_label_create(parent);

        lv_label_set_text(empty, "本地没有歌");
        lv_obj_set_style_text_font(empty, UI_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(UI_TEXT_TERTIARY), 0);
        return;
      }

    for (i = 0; i < s_local_count; i++)
      {
        lv_obj_t *row = ui_button_create(parent, 300, 52, UI_SURFACE,
                                         s_local_names[i],
                                         on_local_clicked, s_local_names[i]);

        lv_obj_set_style_margin_bottom(row, UI_GAP_S, 0);
      }
}

/****************************************************************************
 * 控制按钮（全部只入队，绝不阻塞）
 ****************************************************************************/

static void on_prev(lv_event_t *e)
{
    char arg[24];
    int target = s_pos_ms - SEEK_STEP_MS;

    (void)e;

    if (target < 0)
      {
        target = 0;
      }

    snprintf(arg, sizeof(arg), "%d", target);
    (void)music_worker_post(MUSIC_CMD_SEEK, arg);
}

static void on_next(lv_event_t *e)
{
    char arg[24];
    int target = s_pos_ms + SEEK_STEP_MS;

    (void)e;

    snprintf(arg, sizeof(arg), "%d", target);
    (void)music_worker_post(MUSIC_CMD_SEEK, arg);
}

static void on_toggle(lv_event_t *e)
{
    music_snapshot_t snap;

    (void)e;

    music_worker_snapshot(&snap);

    if (snap.valid &&
        (strcmp(snap.state, "PLAYING") == 0 ||
         strcmp(snap.state, "PREPARED") == 0))
      {
        /* ★ 2026-11-01：暂停/续播临时下线，按钮改为"停止"。
         * 原因：真机音频设备在"暂停后再续播"的路径上会偶发卡死并触发
         * 看门狗（黑屏重启）。演示稳定性优先，先只保留"播放→停止/
         * 自然放完"两条已验证安全的路径；暂停功能留待后续版本。 */
        (void)music_worker_post(MUSIC_CMD_STOP, NULL);
        lv_label_set_text(s_hint_label, "已停止（暂停功能暂不可用）");
      }
    else
      {
        /* 没在播放：这个页面不知道要放哪首，如实提示而不是点了没反应 */
        lv_label_set_text(s_hint_label, "先点上方的歌 或 说「播放 歌名」");
      }
}

static void on_vol_up(lv_event_t *e)
{
    char arg[16];
    int v = s_volume + VOL_STEP;

    (void)e;

    if (v > 100)
      {
        v = 100;
      }

    snprintf(arg, sizeof(arg), "%d", v);
    (void)music_worker_post(MUSIC_CMD_VOLUME, arg);
}

static void on_vol_down(lv_event_t *e)
{
    char arg[16];
    int v = s_volume - VOL_STEP;

    (void)e;

    if (v < 0)
      {
        v = 0;
      }

    snprintf(arg, sizeof(arg), "%d", v);
    (void)music_worker_post(MUSIC_CMD_VOLUME, arg);
}
/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *music_page_create(void)
{
    lv_obj_t *statusbar;
    lv_obj_t *row;

    if (s_page != NULL)
      {
        return s_page;
      }

    /* 工作线程只启动一次，跨页面复用 */
    (void)music_worker_init();

    s_page = lv_obj_create(lv_screen_active());
    ui_page_init(s_page);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    statusbar = ui_statusbar_create(s_page, "音乐");
    (void)statusbar;

    /* 1. 本地音乐列表 —— 放最上面，因为这是唯一一定放得出声的入口 */

    local_list_build(s_page);

    /* 2. 状态大字 */

    s_state_label = lv_label_create(s_page);
    lv_label_set_text(s_state_label, "未播放");
    lv_obj_set_style_text_font(s_state_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_state_label, UI_GAP_M, 0);

    /* 3. 进度条：两个普通 obj 拼（本构建没有 lv_bar） */

    s_bar_bg = lv_obj_create(s_page);
    lv_obj_set_size(s_bar_bg, BAR_W, BAR_H);
    lv_obj_set_style_radius(s_bar_bg, BAR_H / 2, 0);
    lv_obj_set_style_bg_color(s_bar_bg, lv_color_hex(UI_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(s_bar_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar_bg, 0, 0);
    lv_obj_set_style_pad_all(s_bar_bg, 0, 0);
    lv_obj_clear_flag(s_bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_top(s_bar_bg, UI_GAP_M, 0);

    s_bar_fill = lv_obj_create(s_bar_bg);
    lv_obj_set_size(s_bar_fill, 0, BAR_H);
    lv_obj_set_pos(s_bar_fill, 0, 0);
    lv_obj_set_style_radius(s_bar_fill, BAR_H / 2, 0);
    lv_obj_set_style_bg_color(s_bar_fill, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar_fill, 0, 0);
    lv_obj_set_style_pad_all(s_bar_fill, 0, 0);
    lv_obj_clear_flag(s_bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* 4. 时间 */

    s_time_label = lv_label_create(s_page);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_set_style_text_font(s_time_label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_time_label, UI_GAP_S, 0);

    /* 5. 控制按钮 */

    row = lv_obj_create(s_page);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(row, UI_GAP_M, 0);

    ui_button_create(row, 84, 56, UI_SURFACE, "-10s", on_prev, NULL);
    s_toggle_btn = ui_button_create(row, 100, 56, UI_ACCENT, "播放",
                                    on_toggle, NULL);
    ui_button_create(row, 84, 56, UI_SURFACE, "+10s", on_next, NULL);

    /* 6. 音量 */

    row = lv_obj_create(s_page);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(row, UI_GAP_S, 0);

    ui_button_create(row, 66, 52, UI_SURFACE, "音量-", on_vol_down, NULL);

    s_vol_label = lv_label_create(row);
    lv_label_set_text(s_vol_label, "音量 --");
    lv_obj_set_style_text_font(s_vol_label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_left(s_vol_label, UI_GAP_M, 0);
    lv_obj_set_style_pad_right(s_vol_label, UI_GAP_M, 0);

    ui_button_create(row, 66, 52, UI_SURFACE, "音量+", on_vol_up, NULL);

    /* 7. 提示 */

    s_hint_label = lv_label_create(s_page);
    lv_label_set_text(s_hint_label, "点上面的歌开始播放");
    lv_obj_set_style_text_font(s_hint_label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(s_hint_label, UI_GAP_S, 0);

    s_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
    music_refresh();

    /* ★ 2026-10-25 修正：原写 ui_gesture_attach(s_page, NULL)——状态维护
     * 有了，但本页没有返回按钮，等于**滑不出去**（用户实测反馈，
     * 团队审计 A 也确认：页内无任何退出手段，只能靠 KEY1 绕道）。
     *
     * 改用 ui_nav_attach_back_swipe()：内部就是
     * ui_gesture_attach(page, on_back_gesture)，s_drag 的清零机制不变
     * （按钮防误触照常工作），同时把「下滑返回」接上。 */
    ui_nav_attach_back_swipe(s_page);

    printf("[MusicPage] created\n");
    return s_page;
}

void music_page_delete(void)
{
    /* 定时器必须先删：它每秒回调一次，页面对象删掉之后再碰
     * s_state_label 这些指针就是 use-after-free。 */
    if (s_timer != NULL)
      {
        lv_timer_delete(s_timer);
        s_timer = NULL;
      }

    /* 离开音乐页就停播。否则退出页面后歌还在响（功放一直开着），
     * 而且播放器占着 /dev/audio/pcm0p，后面的语音播报也用不上设备。 */
    pcm_file_stop();

    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_state_label = NULL;
    s_time_label  = NULL;
    s_bar_bg      = NULL;
    s_bar_fill    = NULL;
    s_toggle_btn  = NULL;
    s_vol_label   = NULL;
    s_hint_label  = NULL;
}
/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LVGL UI 通道。
 * 初始化 LVGL 显示驱动、创建 UI 控件（Chat View、PTT Button、Recording Indicator）
 * 并初始化聊天历史环形缓冲区。在 Phase 3 调用。
 */
int lvgl_ui_channel_init(void);

/**
 * 启动 LVGL UI 通道。
 * 创建 LVGL 事件循环线程（lv_timer_handler 周期调用）。
 * 在 Phase 5 调用，不依赖网络。
 */
int lvgl_ui_channel_start(void);

/**
 * 停止 LVGL UI 通道。
 * 停止事件循环线程，释放 UI 资源和聊天历史。
 */
void lvgl_ui_channel_stop(void);

/**
 * 显示聊天界面。
 * 调用 lv_screen_load() 将 AI Agent 聊天屏幕切换到前台。
 * 可通过 CLI "show_chat" 命令或收到 Agent 回复时自动触发。
 */
void lvgl_ui_channel_show(void);

/**
 * 发送 Agent 回复到 UI。
 * 在 Chat View 中添加 Agent 消息气泡，并调用 voice_channel_speak() 进行 TTS 播报。
 * 首次调用时自动显示聊天界面。由 outbound_dispatch_task 调用。
 *
 * @param text  UTF-8 文本，Agent 回复内容
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send(const char* text);

/**
 * 发送 Agent 回复到 UI（扩展版）：显式指定卡片左侧色条颜色。
 * bar_color=0 → 沿用 chat_view_classify_reply 启发式自动分类（与 send 等价）。
 * 非 0 → 用指定颜色覆盖分类（绿=结论/蓝=结构化/橙=jira/灰=普通，见
 * LVGL_UI_CARD_COLOR_* 宏）。其余行为（自动显屏、async 调度、TTS）同 send。
 *
 * @param text      UTF-8 文本，Agent 回复内容
 * @param bar_color 色条颜色，0=自动分类
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send_ex(const char* text, uint32_t bar_color);

/**
 * 显示用户消息气泡（右对齐蓝色气泡，无色条卡片）。
 * 用于回显 PTT 识别到的用户语音文本。不触发 TTS（用户自己的话不播报）。
 * 首次调用时自动显示聊天界面。线程安全：通过 lv_async_call 调度到 LVGL 线程。
 *
 * @param text  UTF-8 文本，用户语音识别内容
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send_user(const char* text);

/* PTT 按钮状态机，可由外部（AP 经 RPMsg）驱动，反映语音管线阶段。 */
enum lvgl_ui_ptt_state {
    LVGL_UI_PTT_IDLE       = 0,  /* 空闲：恢复默认按钮样式，隐藏指示器 */
    LVGL_UI_PTT_RECORDING = 1,  /* 录音中：按钮红+脉冲，指示器"录音中..." */
    LVGL_UI_PTT_PROCESSING = 2, /* 识别中：停止脉冲，指示器"识别中..." */
};

/**
 * 设置 PTT 按钮状态（录音中/识别中/空闲）。线程安全：通过 lv_async_call
 * 调度到 LVGL 线程执行，避免跨核/跨线程动 widget。
 * @param state  enum lvgl_ui_ptt_state
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_set_ptt(int state);

/* 宠物状态环的状态语义（M7 差异化页）。值与 pet_status_view.h 的 enum
 * pet_state 一一对应；此处用 int 以免把 pet_status_view.h 拖进 AP/agent 的
 * 公共包含路径。0=idle,1=listening,2=thinking,3=executing,4=waiting,
 * 5=error。 */
int lvgl_ui_channel_set_pet_state(int state);

/* 置 ERROR 态并附带具体原因（短句、可含换行）。宠物屏会把通用「出错了」
 * 换成该原因，例如未联网时显示「未联网 / 请先连接 Wi-Fi」，避免聊天卡
 * 让位后用户只看到一个看不出原因的红色错误态。reason 为空时退化为
 * set_pet_state(5)。线程安全（异步调度到 LVGL 线程）。 */
int lvgl_ui_channel_set_pet_error(const char *reason);

/* 切换圆屏差异化页前台（M7 ① pet / ② particle）。page:
 *   0 = 宠物状态环首屏 (pet_status_view)
 *   1 = 粒子流差异化页 (particle_flow_view)
 * 聊天气泡屏由 send/send_user 自动抢占、空闲自动让位回宠物屏，故不在此列。
 * 经 lv_async_call 在 LVGL 线程执行；返回 0 成功，-ENOSYS 表示该页未编入。 */
int lvgl_ui_channel_set_page(int page);

/* 审批页（远程权限请求 pending 时跳转，让用户明确当前短按是审批语义）。
 * 在 CP/qemu 上切到独立审批 screen 并填充 tool + hint；在 AP UI_RPMSG shim
 * 上序列化为 UI_RPMSG_SET_APPROVAL。tool/hint 均为 UTF-8，可空。 */
int lvgl_ui_channel_set_approval(const char* tool, const char* hint);

/* 退出审批页，恢复之前的页面（pet/particle/chat）。审批被决定、被取消或
 * 超时后由 AP 驱动。返回 0 成功，负值错误。 */
int lvgl_ui_channel_clear_approval(void);

/* 审批页是否仍处于 pending（set 已调用、尚未 clear）。供同步执行工具的路径
 * 判断宠物状态能否回落到 IDLE —— pending 期间审批页持有 WAITING，不能被
 * 工具返回值覆盖。返回 1 pending，0 无。 */
int lvgl_ui_channel_approval_pending(void);

/* 滚动聊天记录：dir 0 = 上翻（更早），1 = 下翻（更新）。首次调用会自动把
 * 聊天屏切到前台。经 lv_async_call 调度到 LVGL 线程。 */
int lvgl_ui_channel_scroll(int dir);

/* 唤醒待机时钟:若待机屏当前可见,隐藏它并重启 60s 空闲倒计时,返回 1(表示
 * 本次调用"吃掉"了这记按键,只做了解锁);若待机屏未可见返回 0(按键应照常派发)。
 * 键适配器据此在待机可见时吞掉第一下按键,避免"唤醒"顺带答掉挂在审批页上的
 * 请求。线程安全:ui_activity_reset 异步调度 hide。 */
int lvgl_ui_channel_wake_from_standby(void);

/* ── LVGL 线程安全锁 ───────────────────────────────────────────
 *
 * 本构建 CONFIG_LV_USE_OS=0，LVGL 自身不做任何加锁（timer 链表、对象树、
 * lv_malloc 堆都是无保护的全局量）。lv_loop_thread() 在一个独立线程里跑
 * lv_timer_handler()，而 agent 循环/outbound/PTT/命令通道等线程通过
 * lv_async_call() / lv_timer_create() 进入 LVGL。无锁时二者竞争：
 * lv_timer_create() 先把 timer 链进全局链表、后填字段，handler 可能在
 * 字段填好前就执行它，读到垃圾 user_data。真机实测：lv_async_timer_cb
 * 对未对齐的 user_data 做 ldmia → UsageFault（CFSR UNALIGNED，ai_agent
 * 进程）→ RPTUN escalate 成 CP panic／复位 AP。
 *
 * 任何非 LVGL 线程调用 LVGL 前必须持有 lvgl_ui_lock()；LVGL 循环线程
 * 在 lv_timer_handler() 外围持锁。锁可重入（owner+depth 模拟）。
 */
void lvgl_ui_lock(void);
void lvgl_ui_unlock(void);

/* 加锁版 lv_async_call：成功返回 0，失败返回 -1。
 * 非 LVGL 线程一律用这个，不要直接调 lv_async_call()。 */
int lvgl_ui_async_call(void (*cb)(void*), void* data);

#ifdef __cplusplus
}
#endif

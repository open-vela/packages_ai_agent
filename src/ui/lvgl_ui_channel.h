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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pet_display.h"

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
 * Bring the pet stage (pet + bubble + menu) to the foreground.
 * Used by the launcher desktop when entering the pet app.
 */
void lvgl_ui_enter_pet_stage(void);

/**
 * DEMO-PHONE（2026-11-03）：把界面切回小云页（AI 页）。
 *
 * 手机 App 通过 BLE {"cmd":"demo"} 触发主动关怀时，用户可能正停留在表盘/
 * 应用列表/心率等任意页面；先调本函数保证小云页出现，随后投递的气泡/表情
 * 才看得见。
 *
 * 线程安全：只做 lvgl_ui_post() 入队，真正的 ui_nav_goto() 在 render 线程
 * 执行；当前已经是 AI 页时不重复切页。可从任意线程调用，非阻塞。
 *
 * @return 0 已入队；-ENODEV UI 通道还没起来；-ENOMEM 入队失败
 *         （失败时调用方照常发气泡，只是不保证切页）。
 */
int lvgl_ui_channel_navigate_pet(void);

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
 * 同 lvgl_ui_channel_send()，但由调用方给出消息类别 (AGENT_MSG_KIND_*)。
 * 只有 AGENT_MSG_KIND_FINAL 才触发 TTS：状态语/错误语仍然渲染气泡与表情
 * （显示与播报解耦），但不会被念出来 (P2a)。
 *
 * @param text  UTF-8 文本，Agent 回复内容
 * @param kind  AGENT_MSG_KIND_FINAL / STATUS / ERROR
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send_kind(const char* text, uint8_t kind);

/**
 * 发送 Agent 气泡但不触发 TTS。供 render 线程内的主动关怀使用，
 * 避免在 LVGL render loop 中同步等待语音播放。
 */
int lvgl_ui_channel_send_silent(const char* text, pet_emotion_t emotion);

/**
 * P2A-感知增强（task-62）：主动关怀跨页角标。
 *
 * 在 lv_layer_top() 顶层显示一条小角标：任何页面都可见（不切页）、5s 后
 * 自动消失；连续触发会刷新文本并重新计时。线程安全：内部只 malloc +
 * lvgl_ui_post()，LVGL 对象操作全部在 render 线程执行。
 * 不写历史、不切页、不触发 TTS（历史/播报仍由 care_fire 各自路径处理）。
 *
 * @param text UTF-8 文本（非空）
 * @return 0 已投递；-EINVAL 参数/UI 未就绪；-ENOMEM。
 */
int lvgl_ui_care_badge(const char* text);

/**
 * 线程安全地向 LVGL 投递一次异步回调。
 * LVGL 以 LV_OS_NONE 构建（无内部锁），任何非 render 线程直接调
 * lv_async_call / lv_timer_* 都会与 lv_timer_handler 竞态并损坏定时器
 * 链表（Round 27：ask 回复后 render 线程睡死）。此函数只做加锁入队，
 * 由 render 线程每圈统一落地。回调在 LVGL 线程执行，语义与
 * lv_async_call 相同。
 */
int lvgl_ui_post(lv_async_cb_t cb, void* data);

/**
 * 发送用户消息到 UI（ASR 识别结果）。
 * 在头顶气泡显示用户气泡并记录到历史。由语音通道（阶段 B）调用。
 *
 * @param text  UTF-8 文本，用户说话内容
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send_user(const char* text);

/**
 * 只把一条消息记入聊天历史环，不改气泡、不动桌宠表情。
 * 给那些不以气泡形式呈现的流量用（NSH `ask` 的提问、以 "cli" 通道回来的回复），
 * 这样历史窗口看到的是完整对话，而不只是 LVGL 通道那一部分。
 * 线程安全：内部经 lv_async_call 转到 LVGL 线程。
 *
 * @param text     UTF-8 文本
 * @param is_user  true 表示用户说的，false 表示 Agent 回的
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_log(const char* text, bool is_user);

/**
 * P2A-感知增强-FIX（task-64）：主动关怀专用历史记录，**保证不切页**。
 *
 * 与 lvgl_ui_channel_log 的区别：只写聊天历史环 + 刷新历史浮层（若可见），
 * 不碰气泡、不更新桌宠页文本、**不调用任何 ui_nav_goto**。表盘/闹钟/音乐等
 * 任意页面触发主动关怀时当前页面保持不变（配合 lvgl_ui_care_badge 的顶层
 * 角标一起用）。
 * 线程安全：内部只 malloc + lvgl_ui_post()，LVGL 操作在 render 线程执行。
 *
 * @param text UTF-8 文本（非空）
 * @return 0 已投递；-EINVAL 参数/UI 未就绪；-ENOMEM。
 */
int lvgl_ui_care_history_log(const char* text);

/****************************************************************************
 * 流式回答显示（SPEED-B / v125）
 *
 * LLM 边生成边把文字显示到手表：头顶气泡 + 桌宠页 AI 文本层逐段追加，
 * 不再等整句返回才出现。4 个函数都**线程安全、非阻塞**（内部只做
 * malloc + lvgl_ui_post() 入队），可从任意线程调用；LVGL 对象操作全部
 * 在 render 线程执行（LVGL 以 LV_OS_NONE 构建，调用线程绝不能碰 lv_obj）。
 *
 * 典型时序（LLM 流式层）：
 *   首个非空 delta 到达： stream_begin(user_text)
 *   每个 delta：          stream_delta(delta_utf8)   // best-effort，可丢
 *   最终回答：            stream_end(final_text, from_local)
 *   工具轮/失败/取消：    stream_abort()
 *
 * 语义要点：
 *  - begin：把用户问题写进历史环/气泡（若 ASR/ask/BLE 已经显示过同一句则
 *    复用，不重复写），并在桌宠页 AI 文本层放 “小云思考中…” 占位。
 *  - delta：追加到**同一**气泡与桌宠页文本层（只 set_text，不重建页面、
 *    不写历史）。超过气泡容量时显示尾部（前缀 …），长回答也能持续滚动。
 *  - end：用 final_text 覆盖气泡、写历史环/桌宠页文本层，表情与
 *    “（本地）”来源标记与既有 ui_mirror_answer() 一致；随后 agent_main 的
 *    dispatch 镜像会被一次性去重，**同一句不会落两遍历史**。
 *  - abort：清掉占位/未完成文本，表情回 IDLE；历史只留随后 dispatch 的
 *    错误语。
 *
 * 健壮性：未收到 begin 的 delta 直接忽略；无 begin 的 end 仍会安全收尾；
 * 重复 begin 重置上一条流；abort 之后再 end 也安全。delta 在积压
 * （>16 条未显示）或内存不足时丢弃（只记一次日志），**end 不丢**。
 ****************************************************************************/

/**
 * 开始一轮流式回答显示。非阻塞。
 * @param user_text 用户问题（UTF-8，可为 NULL/空）；已在历史里则复用不重复。
 * @return 0 已入队；-ENODEV UI 通道未就绪；-ENOMEM 内存不足/入队失败。
 */
int lvgl_ui_channel_stream_begin(const char *user_text);

/**
 * 追加一段增量文本到当前流式气泡。非阻塞、best-effort（压力大时丢增量）。
 * delta_utf8 应是完整的 UTF-8 字符串；实现内部也做了字符边界防御。
 * @return 0（包括“被丢弃”和“没有 begin 被忽略”的情况）。
 */
int lvgl_ui_channel_stream_delta(const char *delta_utf8);

/**
 * 结束流式：用 final_text 落历史/桌宠页文本层/最终气泡，并清流式状态。
 * 不触发 TTS（保持原有 dispatch/TTS 路径）。
 * @param final_text 最终完整回答（UTF-8）
 * @param from_local true 表示端侧模型回答：历史/气泡带 “（本地）”，
 *                   与 ui_mirror_answer() 的标记规则一致
 * @return 0 已入队；-ENOMEM 内存不足/入队失败。
 */
int lvgl_ui_channel_stream_end(const char *final_text, bool from_local);

/**
 * 中止流式（工具轮/失败/取消）：清理占位与未完成文本、表情回 IDLE。
 * 可重复调用；没有进行中的流时是安全 no-op。
 */
void lvgl_ui_channel_stream_abort(void);

/**
 * 聊天历史条数（上限 20 条）。只能在 LVGL 线程上调用。
 */
int lvgl_ui_history_count(void);

/**
 * 取第 newest_first 条历史，0 是最新的一条。只能在 LVGL 线程上调用。
 *
 * @param newest_first  0 = 最新
 * @param buf / len     输出缓冲
 * @param is_user       可为 NULL；输出这条是谁说的
 * @return true 取到了，false 表示下标越界
 */
bool lvgl_ui_history_get(int newest_first, char* buf, size_t len,
    bool* is_user);

/**
 * 把聊天历史环导出成 `chat_history` 事件的 items JSON 数组文本（旧 → 新）。
 *
 * 线程安全：内部 lvgl_ui_post() 把读环的动作投到 LVGL 线程，调用线程用
 * 信号量最多等 200ms。可从任意线程调用（BLE RX 回调线程就是这么用的）。
 * ★ 不要在 LVGL 线程内部调它：投递出去的回调要等本线程返回才跑，只会白
 *   等 200ms 然后 -ETIMEDOUT。读环请直接用 lvgl_ui_history_count/get。
 *
 * 输出裁剪：历史比一帧 BLE 数据（1600B）长时，**保留最新的**若干条
 * （最旧的先丢），单条过长则按 UTF-8 字符边界截断，保证输出始终是合法
 * JSON 数组文本（不含外层对象，调用方自己拼 {"evt":...}）。
 *
 * @param out        输出缓冲；成功时是 NUL 结尾的 items 文本 {"u":1,"t":".."},{"u":0,..}
 *                   —— **不含**外层方括号，调用方自己拼 "items":[<out>]
 * @param cap        输出缓冲容量（>=32）
 * @param max_items  最多导出多少条；<=0 表示不额外限制（上限仍是 20 条）
 * @return 导出的条数（>=0，0 表示历史为空）；负值为错误
 *         （-ENODEV LVGL 通道没起来，-ETIMEDOUT 200ms 内没等到 LVGL 线程，
 *          -EINVAL 参数错，-ENOMEM 投递/初始化失败）
 */
int lvgl_ui_history_export(char* out, size_t cap, int max_items);

#ifdef __cplusplus
}
#endif

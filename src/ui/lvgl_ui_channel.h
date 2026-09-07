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
 * 发送 Agent 回复到 UI。
 * 在 Chat View 中添加 Agent 消息气泡，并调用 voice_channel_speak() 进行 TTS 播报。
 * 首次调用时自动显示聊天界面。由 outbound_dispatch_task 调用。
 *
 * @param text  UTF-8 文本，Agent 回复内容
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send(const char* text);

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

#ifdef __cplusplus
}
#endif

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
 * 发送用户消息到 UI。
 * 对话的另一侧：在 Chat View 中添加用户气泡，用来显示识别出来的命令原文。
 * 不触发 TTS——把用户自己的话念回给他只是噪音，何况本板没有播放设备。
 * 首次调用同样会自动显示聊天界面。
 *
 * @param text  UTF-8 文本，用户说的话
 */
void lvgl_ui_channel_send_user(const char* text);

/**
 * 把一条消息显示到屏幕上，不触发 TTS。
 * 用于镜像那些本来发给别的通道的流量——播报会让原通道重复一次，而且
 * voice_channel_speak() 是阻塞的，会让 outbound 分发线程卡在每条消息上。
 *
 * @param text  UTF-8 文本
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_post(const char* text);

#ifdef __cplusplus
}
#endif

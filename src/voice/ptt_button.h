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

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AI_AGENT_PTT

/**
 * 启动 PTT 物理按键轮询线程（hold-to-talk）。
 * 在 agent_main 的 voice_channel_init 之后调用。线程 detached，
 * 打开 /dev/btn0，poll 按键边沿，press→录音、release→ASR+派发。
 * 依赖 CONFIG_AI_AGENT_LVGL_UI 或 CONFIG_AI_AGENT_UI_RPMSG（UI 通道）。
 * @return 0 成功，负值错误
 */
int ptt_button_start(void);

#endif /* CONFIG_AI_AGENT_PTT */

#ifdef __cplusplus
}
#endif

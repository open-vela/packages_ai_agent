/*
 * Copyright (C) 2025 Xiaomi Corporation
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

/**
 * xiaozhi_channel.h — XiaoZhi WebSocket protocol channel.
 *
 * Connects to XiaoZhi Server via WSS for JSON control messages
 * and binary Opus audio frames.
 */

int xiaozhi_channel_init(void);
int xiaozhi_channel_start(void);
void xiaozhi_channel_stop(void);

/**
 * Send a text message to XiaoZhi Server via the listen protocol.
 * Simulates voice input: sends listen start + STT text + listen stop.
 * @return 0 on success, -1 if not connected
 */
int xiaozhi_channel_send_text(const char* text);

/**
 * Send an Agent reply to XiaoZhi Server as TTS output.
 * Used by outbound dispatch to deliver Agent responses back to the user.
 * Sends: {"type":"tts","state":"start","text":"..."} + {"type":"tts","state":"stop"}
 * @return 0 on success, -1 if not connected
 */
int xiaozhi_channel_send_reply(const char* text);

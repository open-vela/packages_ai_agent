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

/* Build-time Xiaomi MiMo Token Plan credentials for THIS working tree only.
 * Generated from the developer key handed over on 2026-10-26; git-ignored,
 * never commit.  Everything (LLM / TTS / ASR) goes through the token-plan
 * gateway, which is OpenAI compatible:
 *   https://token-plan-cn.xiaomimimo.com/v1/chat/completions
 */
#pragma once

#define MIMO_API_KEY_LOCAL "tp-cfyriyzx371cd9syv05n0lbzwtqaurfpuqt5ont9rj5yhkuo"

#define MIMO_API_HOST      "token-plan-cn.xiaomimimo.com"
#define MIMO_API_PORT      "443"
#define MIMO_API_PATH      "/v1/chat/completions"

#define MIMO_LLM_MODEL     "mimo-v2.6-flash"
#define MIMO_TTS_MODEL     "mimo-v2.5-tts"
#define MIMO_TTS_VOICE     "mimo_default"
#define MIMO_ASR_MODEL     "mimo-v2.5-asr"

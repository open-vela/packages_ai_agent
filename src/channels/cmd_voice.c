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

#include "channels/cmd_voice.h"
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"
#include "voice/voice_channel.h"
#include "voice/voice_tts.h"

#include <stdio.h>
#include <string.h>

void cmd_set_volc_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_volc_key <api_key>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_API_KEY, argv[1]);
    printf("Doubao voice API key saved.\n");
}

void cmd_set_volc_speaker(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_volc_speaker <speaker_id>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_SPEAKER, argv[1]);
    printf("TTS speaker set to: %s\n", argv[1]);
}

void cmd_set_volc_asr(int argc, char** argv)
{
    if (argc < 4) {
        printf("Usage: set_volc_asr <app_id> <token> <cluster>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_APPKEY, argv[1]);
    claw_config_set(AGENT_CFG_KEY_VOLC_TOKEN, argv[2]);
    claw_config_set(AGENT_CFG_KEY_VOLC_ASR_CLUSTER, argv[3]);
    printf("ASR credentials saved (app_id=%s, cluster=%s).\n",
        argv[1], argv[3]);
}

void cmd_voice_start(void)
{
    voice_channel_start();
}

void cmd_voice_stop(void)
{
    voice_channel_stop();
}

void cmd_voice_test_tts(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: voice_test_tts <text> [output.pcm]\n");
        return;
    }

    const char* out = (argc >= 3)
        ? argv[2]
        : AGENT_DATA_DIR "/tts_out.pcm";

    voice_channel_test_tts(argv[1], out);
}

void cmd_voice_test_asr(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: voice_test_asr <pcm_file>\n");
        return;
    }
    voice_channel_test_asr(argv[1]);
}

void cmd_set_voice_tts(int argc, char** argv)
{
    if (argc < 2) {
        const char* cur = voice_tts_get_backend();

        printf("Current TTS backend: %s\n",
            cur ? cur : "(none)");
        printf("Usage: set_voice_tts <backend_name>\n");
        return;
    }

    int ret = voice_tts_set_backend(argv[1]);

    if (ret == 0) {
        printf("TTS backend set to: %s\n", argv[1]);
    } else {
        printf("TTS backend '%s' not found.\n", argv[1]);
    }
}

void cmd_set_voice_asr(int argc, char** argv)
{
    if (argc < 2) {
        const char* cur = voice_asr_get_backend();

        printf("Current ASR backend: %s\n",
            cur ? cur : "(none)");
        printf("Usage: set_voice_asr <backend_name>\n");
        return;
    }

    int ret = voice_asr_set_backend(argv[1]);

    if (ret == 0) {
        printf("ASR backend set to: %s\n", argv[1]);
    } else {
        printf("ASR backend '%s' not found.\n", argv[1]);
    }
}

void cmd_speak(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: speak <text>\n");
        return;
    }

    /* Concatenate all arguments as the text to speak. */
    char content[512] = { 0 };

    for (int i = 1; i < argc; i++) {
        strncat(content, argv[i], sizeof(content) - strlen(content) - 1);
        if (i < argc - 1)
            strncat(content, " ", sizeof(content) - strlen(content) - 1);
    }

    printf("Speaking: %s\n", content);
    int ret = voice_channel_speak(content);
    printf("speak done: %d\n", ret);
}

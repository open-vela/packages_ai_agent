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

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Stubs to fix link errors in a broken defconfig */

#ifdef CONFIG_EXAMPLES_OSTEST
/* These are normally in the kernel, but if missing we stub them to link */
void enter_critical_section(void) { }
void leave_critical_section(void) { }
#endif

#ifdef CONFIG_GRAPHICS_LVGL
__attribute__((weak)) pid_t gettid(void) { return 0; }
#endif

__attribute__((weak)) void arm_netinitialize(void) { }

/* Voice channel stubs for sim build */
#ifndef CONFIG_MEDIA
__attribute__((weak)) int voice_channel_init(void) { return 0; }
__attribute__((weak)) int voice_channel_start(void) { return -1; }
__attribute__((weak)) int voice_channel_stop(void) { return 0; }
__attribute__((weak)) int voice_channel_speak(const char* text)
{
    (void)text;
    return -1;
}
__attribute__((weak)) int voice_channel_test_tts(const char* text)
{
    (void)text;
    return -1;
}
__attribute__((weak)) int voice_channel_test_asr(void) { return -1; }
__attribute__((weak)) const char* voice_tts_get_backend(void) { return "none"; }
__attribute__((weak)) int voice_tts_set_backend(const char* name)
{
    (void)name;
    return -1;
}
__attribute__((weak)) const char* voice_asr_get_backend(void) { return "none"; }
__attribute__((weak)) int voice_asr_set_backend(const char* name)
{
    (void)name;
    return -1;
}
#endif

/* MQTT stubs for sim build */
#ifndef CONFIG_NETUTILS_MQTTC
__attribute__((weak)) int mqtt_reinit(void* client, void* sockfd,
    void* sendbuf, size_t sendbufsz,
    void* recvbuf, size_t recvbufsz)
{
    (void)client;
    (void)sockfd;
    (void)sendbuf;
    (void)sendbufsz;
    (void)recvbuf;
    (void)recvbufsz;
    return -1;
}
__attribute__((weak)) int mqtt_connect(void* client, const char* client_id,
    const char* will_topic, const void* will_message,
    size_t will_message_size, const char* user_name,
    const char* password, uint8_t connect_flags,
    uint16_t keep_alive)
{
    (void)client;
    (void)client_id;
    (void)will_topic;
    (void)will_message;
    (void)will_message_size;
    (void)user_name;
    (void)password;
    (void)connect_flags;
    (void)keep_alive;
    return -1;
}
__attribute__((weak)) int mqtt_subscribe(void* client, const char* topic, int max_qos)
{
    (void)client;
    (void)topic;
    (void)max_qos;
    return -1;
}
__attribute__((weak)) int mqtt_sync(void* client)
{
    (void)client;
    return -1;
}
__attribute__((weak)) int mqtt_init_reconnect(void* client,
    void* reconnect_callback, void* reconnect_state,
    void* publish_response_callback)
{
    (void)client;
    (void)reconnect_callback;
    (void)reconnect_state;
    (void)publish_response_callback;
    return -1;
}
__attribute__((weak)) int mqtt_publish(void* client, const char* topic,
    const void* msg, size_t msg_sz, uint8_t flags)
{
    (void)client;
    (void)topic;
    (void)msg;
    (void)msg_sz;
    (void)flags;
    return -1;
}
#endif

/* media_player_*  的实现在 src/voice/pcm_media_player.c（强符号）。
 * media_recorder_* 的实现在 src/voice/pcm_media_recorder.c（强符号）。
 *
 * ★ 2026-10-14: 这里原本还有一组**永远失败**的 media_recorder_* 弱桩, 结果是
 *   链接时弱符号先被拉进来、归档里那个强实现再也没机会被选中 —— 真机上
 *   media_recorder_open() 恒返回 NULL, audio_capture_open() 的采集分支必然
 *   失败("[audio_cap] media_recorder_open failed"), 也就是**实时语音采集从来
 *   没工作过**。既然已经提供了真实现, 这组桩就必须删掉, 与 media_player 的
 *   处理方式保持一致。 */
#if 0
#endif

/* WeChat channel stubs for sim build */
__attribute__((weak)) int weixin_channel_init(void) { return 0; }
__attribute__((weak)) int weixin_channel_start(void) { return -1; }
__attribute__((weak)) int weixin_channel_send(const char* to_user_id,
    const char* context_token, const char* text)
{
    (void)to_user_id;
    (void)context_token;
    (void)text;
    return -1;
}
__attribute__((weak)) void weixin_channel_stop(void) { }
__attribute__((weak)) int weixin_channel_login(char* qr_url, size_t qr_cap,
    char* qrcode_id, size_t qrc_cap)
{
    (void)qr_url;
    (void)qr_cap;
    (void)qrcode_id;
    (void)qrc_cap;
    return -1;
}
__attribute__((weak)) int weixin_channel_poll_login(const char* qrcode_id)
{
    (void)qrcode_id;
    return -1;
}

/* ── Shutdown stubs (real impl in agent_main.c / MAINSRC) ── */

static volatile bool _stub_shutdown;
__attribute__((weak)) void agent_request_shutdown(void)
{
    _stub_shutdown = true;
}
__attribute__((weak)) bool agent_shutdown_requested(void)
{
    return _stub_shutdown;
}

/* media_player_write_data() 由上面的 pcm0p 实现提供 (强符号)。 */

/* ── QR code display stub ─────────────────────────────────── */

__attribute__((weak)) int claw_show_qrcode(const char* url)
{
    (void)url;
    return -1;
}

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

/* NOTE (task #32): the voice_channel/voice_tts/voice_asr weak
 * stubs that used to live here were removed.  The real strong
 * definitions ship inside the same package (src/voice/), and
 * these weak duplicates made GNU ld bind references to stubs.o
 * (already extracted from the archive) instead of extracting
 * voice_channel.o -- so the whole voice -> audio -> native
 * chain silently vanished from the final ELF. */

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

/* Media player stubs for sim build */
#ifndef CONFIG_MEDIA
__attribute__((weak)) void* media_player_open(const char* url)
{
    (void)url;
    return NULL;
}
__attribute__((weak)) int media_player_close(void* player, int dummy)
{
    (void)player;
    (void)dummy;
    return -1;
}
__attribute__((weak)) int media_player_start(void* player)
{
    (void)player;
    return -1;
}
__attribute__((weak)) int media_player_stop(void* player)
{
    (void)player;
    return -1;
}
__attribute__((weak)) int media_player_pause(void* player)
{
    (void)player;
    return -1;
}
__attribute__((weak)) int media_player_seek(void* player, unsigned int msec)
{
    (void)player;
    (void)msec;
    return -1;
}
__attribute__((weak)) int media_player_prepare(void* player, const char* url, const char* opts)
{
    (void)player;
    (void)url;
    (void)opts;
    return -1;
}
__attribute__((weak)) int media_player_set_event_callback(void* player, void* cookie, void* cb)
{
    (void)player;
    (void)cookie;
    (void)cb;
    return -1;
}
__attribute__((weak)) int media_player_get_position(void* player, unsigned int* msec)
{
    (void)player;
    (void)msec;
    return -1;
}
__attribute__((weak)) int media_player_get_duration(void* player, unsigned int* msec)
{
    (void)player;
    (void)msec;
    return -1;
}
__attribute__((weak)) int media_policy_set_stream_volume(const char* stream, int volume)
{
    (void)stream;
    (void)volume;
    return -1;
}

/* Media recorder stubs for sim build */
__attribute__((weak)) void* media_recorder_open(const char* src)
{
    (void)src;
    return NULL;
}
__attribute__((weak)) int media_recorder_close(void* rec)
{
    (void)rec;
    return -1;
}
__attribute__((weak)) int media_recorder_prepare(void* rec, const char* url, const char* opts)
{
    (void)rec;
    (void)url;
    (void)opts;
    return -1;
}
__attribute__((weak)) int media_recorder_start(void* rec)
{
    (void)rec;
    return -1;
}
__attribute__((weak)) int media_recorder_stop(void* rec)
{
    (void)rec;
    return -1;
}
__attribute__((weak)) ssize_t media_recorder_read_data(void* rec, void* buf, size_t len)
{
    (void)rec;
    (void)buf;
    (void)len;
    return -1;
}
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

/* ── Media player write_data stub ─────────────────────────── */

__attribute__((weak)) int media_player_write_data(void* player,
    const void* data, size_t len)
{
    (void)player;
    (void)data;
    (void)len;
    return -1;
}

/* ── QR code display stub ─────────────────────────────────── */

__attribute__((weak)) int claw_show_qrcode(const char* url)
{
    (void)url;
    return -1;
}

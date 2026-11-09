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

/**
 * ui_rpmsg_proto.h — Wire protocol for the AP→CP UI RPMsg channel (路 B).
 *
 * The AP core runs ai_agent (LLM/voice/TLS) but has no LCD/LVGL/FPU.  The CP
 * core owns the round LCD and renders the LVGL chat UI.  This header defines
 * the shared message format used by:
 *   - AP side:  src/ui/ui_rpmsg_client.c  (implements the lvgl_ui_channel_*
 *                + lvgl_toast_* API as RPMsg senders — a drop-in shim so the
 *                agent's call sites stay byte-identical, only the link-time
 *                implementation swaps to RPMsg).
 *   - CP side:  src/ui_cp/ui_rpmsg_server.c (deserializes each message and
 *                calls the REAL lvgl_ui_channel_* / lvgl_toast_* on this core,
 *                where LVGL actually runs).
 *
 * Design notes:
 *   - One struct, opcode-driven.  RPMsg MTUs on bk7258 are small (~512B
 *     including openamp header), so the payload cap is conservative.  Agent
 *     chat replies are truncated to UI_RPMSG_MAX_TEXT before sending; the CP
 *     chat view scrolls anyway and the full text stays in the AP chat log.
 *   - TTS is NOT carried here.  lvgl_ui_channel_send() on CP calls
 *     voice_channel_speak() which is a no-op stub on CP (TTS hardware/driver
 *     lives on AP).  The AP client shim calls voice_channel_speak() locally
 *     AND sends UI_RPMSG_SEND for the bubble — so audio plays on AP, the
 *     bubble renders on CP, with no protocol round-trip for audio.
 *   - Opcodes are extensible: future send_ex (sender metadata) and set_ptt
 *     (push-to-talk state) add new opcodes without breaking older peers.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RPMsg endpoint name — both cores open an endpoint with this name. */
#define UI_RPMSG_EPT_NAME    "ui_rpmsg"
/* RPMsg remote CPU name on the AP side (CP owns the LCD). */
#define UI_RPMSG_REMOTE_CPU  "cp"

/* Conservative payload cap: fits a 512-byte RPMsg buffer with room for the
 * openamp header.  Must stay in sync with the link-time MTU. */
#define UI_RPMSG_MAX_TEXT    480
#define UI_RPMSG_MAX_PAYLOAD (UI_RPMSG_MAX_TEXT)

/** AP→CP UI command opcodes. */
enum ui_rpmsg_op {
    UI_RPMSG_NOP    = 0,
    UI_RPMSG_INIT   = 1,  /* (re)assert UI up / ack endpoint open */
    UI_RPMSG_START  = 2,  /* start LVGL loop (CP already self-starts; AP ack) */
    UI_RPMSG_STOP   = 3,  /* stop LVGL loop */
    UI_RPMSG_SHOW   = 4,  /* bring chat screen to foreground */
    UI_RPMSG_SEND   = 5,  /* add a chat bubble: payload = UTF-8 agent reply text */
    UI_RPMSG_TOAST  = 6,  /* transient toast: payload = title\0body + color + dur */
    UI_RPMSG_SEND_EX = 7, /* bubble w/ explicit card bar color:
                           * [bar_color:u32le][text..] (text not NUL-term).
                           * bar_color=0 → CP auto-classifies (same as SEND). */
    UI_RPMSG_SET_PTT = 8, /* drive PTT button state from AP:
                           * payload[0] = enum lvgl_ui_ptt_state
                           * (0=idle,1=recording,2=processing). */
    UI_RPMSG_SEND_USER = 9, /* show a USER (right-aligned) bubble with the
                             * recognized utterance: payload = UTF-8 text
                             * (len, not NUL-terminated), like SEND but
                             * is_user=true and no TTS. */
    UI_RPMSG_SET_PET_STATE = 10, /* drive the CP-side pet status ring (M7 差异化
                                  * 页) from the AP agent's lifecycle state.
                                  * payload[0] = enum pet_state
                                  * (0=idle,1=listening,2=thinking,
                                  *  3=executing,4=waiting,5=error);
                                  * payload_len = 1. */
    UI_RPMSG_SET_PAGE = 11,      /* switch the CP round-screen page:
                                  * payload[0] = page index
                                  * (0 = pet home, 1 = particle flow);
                                  * payload_len = 1. */
    UI_RPMSG_SET_APPROVAL = 12, /* show the approval screen (a remote permission
                                 * request is pending): payload = [tool\0hint\0]
                                 * (both NUL-terminated, truncated to the cap).
                                 * tool ≤ 24, hint ≤ 96 bytes from the bridge. */
    UI_RPMSG_CLEAR_APPROVAL = 13, /* dismiss the approval screen and restore the
                                   * previous foreground page; payload_len = 0. */
    UI_RPMSG_SCROLL = 14,       /* scroll the chat transcript: payload[0] =
                                 * direction (0 = up/older, 1 = down/newer);
                                 * payload_len = 1. */
    UI_RPMSG_CLEAR_HISTORY = 15, /* drop the on-screen conversation and bring the
                                  * (now empty) chat screen up — a new gateway
                                  * session started; payload_len = 0. */
};

/** Fixed wire format (packed, little-endian on both M-class cores). */
struct ui_rpmsg_msg {
    uint8_t  op;            /* enum ui_rpmsg_op */
    uint8_t  reserved;      /* zero on send, reserved for future flags */
    uint16_t payload_len;   /* valid bytes in payload[] (0 for INIT/START/STOP/SHOW) */
    /** Op-specific payload.
     *   SEND    : payload[0..payload_len-1] = UTF-8 text (len, not NUL-terminated).
     *   TOAST   : payload = [bar_color:u32le][duration_ms:u32le][title\0body\0].
     *             payload_len covers all of the above.
     *   SEND_EX : payload = [bar_color:u32le][text..] (text len, not NUL-term);
     *             payload_len covers the u32 + text.
     *   SET_PTT : payload[0] = PTT state (0 idle,1 recording,2 processing);
     *             payload_len = 1.
     *   SEND_USER : payload = UTF-8 user utterance text (len, not NUL-term);
     *               like SEND but rendered as a user (right) bubble, no TTS.
     *   SET_PET_STATE : payload[0] = pet state (0..5, see enum pet_state);
 *                  payload_len = 1.
 *   SET_PAGE : payload[0] = page index (0=pet home,1=particle flow);
 *              payload_len = 1.
 *   INIT/START/STOP/SHOW: payload_len = 0.
     */
    uint8_t  payload[UI_RPMSG_MAX_PAYLOAD];
} __attribute__((packed));

/* Compile-time sanity: the message must fit a typical 512-byte RPMsg buffer. */
#define UI_RPMSG_MSG_SIZE (sizeof(struct ui_rpmsg_msg))

#ifdef __cplusplus
}
#endif

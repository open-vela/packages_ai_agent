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

#include "channels/cmd_channel.h"
#include "infra/config_store.h"
#include "channels/feishu_bot.h"
#include "channels/mqtt_channel.h"
#ifdef CONFIG_AI_AGENT_REMOTE_CTRL
#include "channels/remote_ctrl_channel.h"
#endif
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_client.h"
#include "node/node_manager.h"
#endif
#include "ui/qrcode_display.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "channels/weixin_channel.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void cmd_set_feishu_app(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: set_feishu_app <app_id> <app_secret>\n");
        return;
    }
    feishu_set_app(argv[1], argv[2]);
    printf("Feishu app credentials saved (app_id=%s).\n", argv[1]);
}

void cmd_set_feishu_user_token(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_feishu_user_token <token>\n");
        return;
    }
    feishu_set_user_token(argv[1]);
    printf("Feishu user_access_token saved (%.8s...).\n", argv[1]);
}

void cmd_set_mqtt(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_mqtt <host:port> [client_id]\n"
               "  e.g. set_mqtt <broker-host>:1883\n"
               "       set_mqtt <broker-host>:1883 my-device\n");
        return;
    }

    claw_config_set(AGENT_CFG_KEY_MQTT_BROKER, argv[1]);
    if (argc >= 3) {
        claw_config_set(AGENT_CFG_KEY_MQTT_CLIENT_ID, argv[2]);
    }

    /* Re-init and start immediately so no restart is needed */
    mqtt_channel_stop();
    mqtt_channel_init();
    mqtt_channel_start();
    printf("MQTT broker set to %s and started.\n", argv[1]);
}

#ifdef CONFIG_AI_AGENT_NODE
void cmd_node_list(void)
{
    char buf[1024];
    printf("=== Connected Nodes ===\n");
    node_manager_list(buf, sizeof(buf));
    printf("%s", buf);
    printf("=======================\n");
}
#endif

void cmd_set_gateway(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_gateway <host> [port] [token]\n");
        printf("  e.g. set_gateway <host> 8080 my-token\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_GATEWAY_HOST, argv[1]);
    if (argc >= 3)
        claw_config_set(AGENT_CFG_KEY_GATEWAY_PORT, argv[2]);
    if (argc >= 4)
        claw_config_set(AGENT_CFG_KEY_GATEWAY_TOKEN, argv[3]);
    printf("Gateway set to %s:%s%s. Use 'node_start' to connect.\n",
        argv[1], argc >= 3 ? argv[2] : "8080",
        argc >= 4 ? " (token saved)" : "");
}

#ifdef CONFIG_AI_AGENT_NODE
void cmd_node_start(void)
{
    int ret = node_client_start();
    if (ret == OK) {
        printf("Node client started (check logs for connection status).\n");
    } else {
        printf("Node client start failed.\n");
    }
}

void cmd_node_stop(void)
{
    node_client_stop();
    printf("Node client stopped.\n");
}
#endif

void cmd_set_weixin_token(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_weixin_token <bot_token>\n");
        return;
    }
    weixin_channel_set_token(argv[1], 0);
    printf("WeChat bot token saved.\n");
}

void cmd_weixin_login(void)
{
    char qr_url[512] = "";
    char qrcode_id[128] = "";

    printf("Requesting QR code from iLink Bot API...\n");
    int rc = weixin_channel_login(qr_url, sizeof(qr_url),
        qrcode_id, sizeof(qrcode_id));
    if (rc != 0) {
        printf("Failed to get QR code (rc=%d). Check network.\n", rc);
        return;
    }

    printf("QR URL: %s\n", qr_url);
    claw_show_qrcode(qr_url);
    printf("Scan the QR code with WeChat, then waiting...\n");

    for (int i = 0; i < 60; i++) {
        sleep(2);
        int status = weixin_channel_poll_login(qrcode_id);
        if (status == 1) {
            printf("Login confirmed! Token saved.\n");
            return;
        } else if (status == 2) {
            printf("Scanned, waiting for confirm...\n");
        } else if (status == -3) {
            printf("QR code expired. Run weixin_login again.\n");
            return;
        } else if (status < 0) {
            printf("Poll error (rc=%d).\n", status);
            return;
        }
    }
    printf("Timeout waiting for scan.\n");
}

#ifdef CONFIG_AI_AGENT_REMOTE_CTRL

/* The transcript is up to 8 KiB and the console thread has a 16 KiB stack, so
 * it is staged here rather than on the stack. Only the console reads it. */
static char g_remote_text[REMOTE_TRANSCRIPT_MAX + 1];

void cmd_set_remote(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_remote <host:port> [device_id]\n"
               "  Points this device at the PC bridge that fronts the remote\n"
               "  agent. Use set_remote_auth for broker credentials.\n");
        return;
    }

    claw_config_set(AGENT_CFG_KEY_REMOTE_BROKER, argv[1]);
    if (argc >= 3) {
        claw_config_set(AGENT_CFG_KEY_REMOTE_DEVICE_ID, argv[2]);
    }

    /* Re-init and start immediately so no restart is needed. */
    remote_ctrl_channel_stop();
    remote_ctrl_channel_init();
    if (remote_ctrl_channel_start() != OK) {
        printf("Remote broker saved, but the channel did not start. "
               "Check the log.\n");
        return;
    }

    printf("Remote broker set to %s and starting.\n", argv[1]);
}

void cmd_set_remote_auth(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: set_remote_auth <username> <password>\n");
        return;
    }

    claw_config_set(AGENT_CFG_KEY_REMOTE_USERNAME, argv[1]);
    claw_config_set(AGENT_CFG_KEY_REMOTE_PASSWORD, argv[2]);

    remote_ctrl_channel_stop();
    remote_ctrl_channel_init();
    remote_ctrl_channel_start();

    /* The password is never echoed. */
    printf("Remote credentials saved for %s.\n", argv[1]);
}

void cmd_remote_status(void)
{
    struct remote_status_s status;

    if (remote_ctrl_channel_status(&status) != OK) {
        printf("Remote control is not configured. Use set_remote first.\n");
        return;
    }

    printf("=== Remote agent ===\n");
    printf("  State     : %s\n", remote_state_name(status.state));
    if (status.summary[0] != '\0') {
        printf("  Activity  : %s\n", status.summary);
    }
    /* Which model answered matters for reading the reply and the bill, and it
     * can be changed from the PC side, so it is shown rather than assumed. */
    if (status.model[0] != '\0') {
        printf("  Model     : %s\n", status.model);
    }
    if (status.billing[0] != '\0') {
        printf("  %s\n", status.billing);
    }
    printf("  Chat turns: %s\n", status.chat_supported ? "yes" : "no");
    if (status.turn_active) {
        printf("  A turn is running.\n");
    }

    if (status.has_prompt) {
        printf("\n  Approval needed for: %s\n", status.prompt.tool);
        if (status.prompt.hint[0] != '\0') {
            printf("  Details            : %s\n", status.prompt.hint);
        }
        if (status.decision_in_flight) {
            printf("  A decision was already sent; waiting for the bridge.\n");
        } else {
            printf("  Answer with%s%s.\n",
                status.prompt.can_once ? " 'ronce'" : "",
                status.prompt.can_deny
                    ? (status.prompt.can_once ? " or 'rdeny'" : " 'rdeny'")
                    : "");
        }
    }
    printf("====================\n");
}

void cmd_remote_ask(int argc, char** argv)
{
    char text[REMOTE_PROMPT_TEXT_MAX + 1] = { 0 };
    char reply[256];

    if (argc < 2) {
        printf("Usage: rask <instruction>\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
        if (i < argc - 1) {
            strncat(text, " ", sizeof(text) - strlen(text) - 1);
        }
    }

    /* Non-blocking on purpose: if this waited for the turn, a remote permission
     * request could never be answered, because answering it needs this very
     * console. */
    if (remote_ctrl_channel_submit(text, reply, sizeof(reply)) != OK) {
        printf("%s\n", reply);
        return;
    }

    printf("%s\n", reply);
}

void cmd_remote_output(void)
{
    if (remote_ctrl_channel_transcript(g_remote_text, sizeof(g_remote_text)) != OK) {
        printf("Remote control is not configured.\n");
        return;
    }

    if (g_remote_text[0] == '\0') {
        printf("(no remote agent output yet)\n");
        return;
    }

    printf("%s", g_remote_text);
    if (g_remote_text[strlen(g_remote_text) - 1] != '\n') {
        printf("\n");
    }
}

/* Resolves a console argument to a model name. All digits means the number the
 * listing printed; anything else is taken as a name, so a model the device could
 * not remember — the list is bounded and may be truncated — is still reachable
 * by typing it in full. Returns NULL only for a number outside the list. */
static const char* resolve_model(const struct remote_model_view_s* view,
    const char* argument)
{
    size_t index = 0;
    size_t i;

    for (i = 0; argument[i] != '\0'; i++) {
        if (argument[i] < '0' || argument[i] > '9') {
            return argument;
        }

        /* Digits only: the user meant an index, so an out-of-range one is an
         * error rather than a name to forward. Bounded here so a long run of
         * digits cannot overflow on the way to that check. */
        if (index > REMOTE_MODEL_OPTIONS_MAX) {
            return NULL;
        }
        index = index * 10 + (size_t)(argument[i] - '0');
    }

    if (index == 0 || index > view->count) {
        return NULL;
    }

    return view->options[index - 1];
}

void cmd_remote_model(int argc, char** argv)
{
    /* Static because the view carries the whole option table: too large for this
     * thread's stack, and copying it out beats holding the session lock across a
     * screenful of printf calls to a slow console. */
    static struct remote_model_view_s view;
    const char* target;
    uint8_t i;

    if (remote_ctrl_channel_model_view(&view) != OK) {
        printf("Remote control is not configured.\n");
        return;
    }

    if (argc < 2) {
        if (view.current[0] == '\0') {
            printf("Model: unknown — the bridge has not reported one yet.\n");
        } else {
            printf("Model: %s\n", view.current);
        }

        if (view.count == 0) {
            printf("No selectable models reported yet.\n");
            return;
        }

        printf("Switch with 'rmodel <number>' or 'rmodel <name>':\n");
        for (i = 0; i < view.count; i++) {
            printf("  %2u  %s%s\n", (unsigned)(i + 1), view.options[i],
                strcmp(view.options[i], view.current) == 0 ? "   (current)" : "");
        }

        if (view.truncated) {
            printf("List is partial; a name missing from it may still be "
                   "accepted.\n");
        }
        return;
    }

    target = resolve_model(&view, argv[1]);
    if (target == NULL) {
        printf("No model %s in the list. Run 'rmodel' to see it.\n", argv[1]);
        return;
    }

    /* Checked before sending only to give a clear reason; the link refuses this
     * on its own, and the bridge refuses it again. */
    if (view.turn_active) {
        printf("A turn is running. Wait for it to finish, then switch.\n");
        return;
    }

    if (remote_ctrl_channel_select_model(target) != OK) {
        printf("Could not request '%s': the link is offline, a turn is "
               "running, or the name is unusable.\n", target);
        return;
    }

    printf("Requested '%s'. The bridge decides whether it applies; 'rstat' "
           "shows what took effect.\n", target);
}

void cmd_remote_usage(void)
{
    if (remote_ctrl_channel_query_usage() != OK) {
        printf("Could not query usage: the link is offline or unconfigured.\n");
        return;
    }

    printf("Usage query sent; the refreshed value appears in the log and in "
           "'rstat'.\n");
}

void cmd_remote_once(void)
{
    if (remote_ctrl_channel_decide(REMOTE_DECISION_ONCE) != OK) {
        printf("No approvable request is pending (or one was already "
               "answered).\n");
        return;
    }

    printf("Approved once.\n");
}

void cmd_remote_deny(void)
{
    if (remote_ctrl_channel_decide(REMOTE_DECISION_DENY) != OK) {
        printf("No deniable request is pending (or one was already "
               "answered).\n");
        return;
    }

    printf("Denied.\n");
}

#endif /* CONFIG_AI_AGENT_REMOTE_CTRL */

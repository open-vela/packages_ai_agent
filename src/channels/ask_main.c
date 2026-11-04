/*
 * ask_main.c - NSH builtin that hands a question to the running agent.
 *
 * The agent already has an "ask" command in its own CLI (nsh_commands.c), but
 * that CLI thread runs at priority 30 and competes with NSH at 100 for the
 * same console fd, so NSH always wins the read and the agent CLI never sees
 * input.  This is a plain NSH builtin instead: the build is flat, so it shares
 * the address space with the daemon and can push straight onto the bus.
 *
 * The reply comes back asynchronously through outbound_dispatch_task, which
 * prints "[Agent]: ..." on the console.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "core/message_bus.h"
#ifdef CONFIG_AI_AGENT_LVGL_UI
#  include "ui/lvgl_ui_channel.h"
#endif

int ask_main(int argc, char *argv[])
{
    char content[256] = { 0 };
    agent_msg_t msg = { 0 };
    int i;

    if (argc < 2) {
        printf("Usage: ask <message>\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        strncat(content, argv[i], sizeof(content) - strlen(content) - 1);
        if (i < argc - 1) {
            strncat(content, " ", sizeof(content) - strlen(content) - 1);
        }
    }

    strncpy(msg.channel, "cli", sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "console", sizeof(msg.chat_id) - 1);
    msg.content = strdup(content);
    if (!msg.content) {
        printf("ask: out of memory\n");
        return 1;
    }

    if (message_bus_push_inbound(&msg) != 0) {
        printf("ask: agent not running?\n");
        free(msg.content);
        return 1;
    }

    printf("Sent to agent: %s\n", content);
    syslog(LOG_INFO, "[ask] %s\n", content);
#ifdef CONFIG_AI_AGENT_LVGL_UI
    /* Record the question so the pet page history window shows both sides. */
    lvgl_ui_channel_log(content, true);
#endif
    return 0;
}

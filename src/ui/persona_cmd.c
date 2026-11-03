/****************************************************************************
 * persona_cmd.c - NSH builtin: 查看/切换桌宠人格。
 *
 *   persona                 打印当前人格和用法
 *   persona gentle          切换（也接受 sassy/cheer 和 温柔/毒舌/元气）
 *
 * 为什么单独做成 NSH builtin：演示时不用先开 agent CLI，也不用让模型
 * 转一手，串口敲一行就能验证「同一个人格状态是否被 system prompt 读到」。
 * 写盘只发生在 persona_set() 里，且只在人格真的变化时写 1 字节，符合
 * 「不在录音/周期路径写 flash」的约束。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <stdio.h>
#include <sys/types.h> /* OK / ERROR */

#include "ui/persona.h"

int persona_main(int argc, char *argv[])
{
    persona_id_t id;

    if (argc < 2) {
        id = persona_get();
        printf("persona: %s (%d)\n", persona_name(id), (int)id);
        printf("usage: persona gentle|sassy|cheer|温柔|毒舌|元气\n");
        return 0;
    }

    if (persona_parse_name(argv[1], &id) != OK) {
        printf("persona: unknown '%s'\n", argv[1]);
        printf("usage: persona gentle|sassy|cheer|温柔|毒舌|元气\n");
        return 1;
    }

    if (persona_set(id) != OK) {
        printf("persona: switch failed\n");
        return 1;
    }

    id = persona_get();
    printf("persona: %s (%d)\n", persona_name(id), (int)id);
    return 0;
}

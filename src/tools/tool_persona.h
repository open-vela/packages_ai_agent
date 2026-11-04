/****************************************************************************
 * tool_persona.h - set_persona 工具（切换桌宠回答风格）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_TOOL_PERSONA_H
#define AI_AGENT_TOOL_PERSONA_H

#include "agent_compat.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Execute the set_persona tool.
 *
 * input_json: {"persona":"gentle"}；persona 也接受 sassy / cheer，
 *             以及中文 温柔 / 毒舌 / 元气。
 * 输出当前人格名（中文），失败返回 ERROR。
 */
int tool_set_persona_execute(const char *input_json, char *output,
                             size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_TOOL_PERSONA_H */

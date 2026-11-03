/****************************************************************************
 * persona.h - 桌宠人格（温柔 / 毒舌 / 元气）的对外接口。
 *
 * 人格只改变「怎么说」，不改变「说什么」：三种人格共用同一套工具、事实
 * 和安全规则，所以同一句话在三种人格下事实一致、语气不同。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_PERSONA_H
#define AI_AGENT_PERSONA_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PERSONA_GENTLE = 0,   /* 温柔 */
    PERSONA_SASSY  = 1,   /* 毒舌 */
    PERSONA_CHEER  = 2,   /* 元气 */
    PERSONA_COUNT
} persona_id_t;

/* 当前人格。首次调用会从 /data/persona 读取；文件不存在/内容非法时
 * 用默认值 PERSONA_GENTLE（温柔）。 */
persona_id_t persona_get(void);

/* 切换人格。人格真的变化时才写一次 /data/persona；重复设置同一人格
 * 不碰 flash。/data 不可写时静默降级为内存态，仍返回 OK。 */
int persona_set(persona_id_t id);

/* 当前人格的 system prompt（静态字符串，调用方不要 free）。 */
const char *persona_get_system_prompt(void);

/* 人格中文名（"温柔"/"毒舌"/"元气"）；非法 id 返回 "未知"。 */
const char *persona_name(persona_id_t id);

/* 别名解析：gentle/sassy/cheer 或 温柔/毒舌/元气。
 * 成功写 *id 返回 OK，无法识别返回 ERROR。供 set_persona 工具和
 * persona NSH 命令共用，避免两处各维护一份别名表。 */
int persona_parse_name(const char *name, persona_id_t *id);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_PERSONA_H */

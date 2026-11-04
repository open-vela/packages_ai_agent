/****************************************************************************
 * persona.c - 桌宠人格状态与持久化。
 *
 * 为什么单独存 /data/persona 而不是写 config.json：
 *   config.json 由 config_store 维护，任何一次 set 都会整份重写；人格切换
 *   只是用户偶尔做一次的动作，单独一个 1 字节文件能把 NOR flash 的写入
 *   面缩到最小。
 *
 * 为什么只在切换时写：
 *   /data 与 XIP 代码在同一片 NOR flash 上，录音期间写 flash 会因总线争用
 *   导致静默复位（见 docs_ble/25_flash_reliability_and_audio_defects.md）。
 *   所以本模块只在 persona_set() 发现人格真的变化时写一次，启动读取、
 *   system prompt 构建、心跳/渲染等周期路径一律不写。
 *
 * 降级策略：
 *   /data 不存在或只读时，读失败用默认人格，写失败只保留内存态，都不向
 *   调用方报错、不崩溃 —— 人格是体验项，不能因为存储故障影响对话。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include "ui/persona.h"
#include "agent_compat.h"

#include <fcntl.h>
#include <strings.h>

#define TAG "persona"

/* 纯文本，内容就是单个数字 0/1/2，方便用 cat/echo 在串口里排查。 */
#define PERSONA_FILE "/data/persona"

/* 三种人格的 system prompt。要求：简短中文、都包含「回答简短」。
 * 只描述语气，不授予任何额外能力/工具权限。 */
static const char *const g_prompts[PERSONA_COUNT] = {
    /* 温柔 */
    "你是温柔体贴的小云，说话轻声细语，先照顾用户的身体和情绪，"
    "再回答问题。多用关心的语气，回答控制在 1~2 句话。",
    /* 毒舌 */
    "你是毒舌又俏皮的小云，说话带点调侃和玩笑，但不会真正冒犯用户，"
    "关键信息仍然准确，回答控制在 1~2 句话。",
    /* 元气 */
    "你是元气满满的小云，说话热情，多用感叹号和积极的鼓励，"
    "像给用户打气一样，回答控制在 1~2 句话。",
};

static const char *const g_names[PERSONA_COUNT] = {
    "温柔",
    "毒舌",
    "元气",
};

/* s_loaded 区分「还没读过盘」和「读过但文件不存在」：启动读一次之后，
 * 后续 get 不再反复 open 文件。 */
static persona_id_t s_persona = PERSONA_GENTLE;
static bool s_loaded;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* 读 /data/persona。只在 s_loaded == false 时调用一次；失败一律保留默认
 * 人格，不写回、不纠正文件，避免在启动路径上产生 flash 写。 */
static void persona_load_locked(void)
{
    char buf[8];
    char *end = NULL;
    ssize_t n;
    long value;
    int fd;

    if (s_loaded) {
        return;
    }

    /* 先置位：即使 open/read 失败也只尝试一次，避免每次构建 system
     * prompt 都去查一个不存在的文件。 */
    s_loaded = true;

    fd = open(PERSONA_FILE, O_RDONLY);
    if (fd < 0) {
        syslog(LOG_INFO, "[%s] no saved persona at %s (%d), using %s\n",
               TAG, PERSONA_FILE, errno, g_names[s_persona]);
        return;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        syslog(LOG_WARNING, "[%s] %s is empty, using %s\n",
               TAG, PERSONA_FILE, g_names[s_persona]);
        return;
    }
    buf[n] = '\0';

    value = strtol(buf, &end, 10);
    if (end == buf || value < 0 || value >= PERSONA_COUNT) {
        syslog(LOG_WARNING, "[%s] invalid persona in %s, using %s\n",
               TAG, PERSONA_FILE, g_names[s_persona]);
        return;
    }

    /* 数字之后只允许空白（兼容手工 echo 带进来的换行）。 */
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }
    if (*end != '\0') {
        syslog(LOG_WARNING, "[%s] trailing junk in %s, using %s\n",
               TAG, PERSONA_FILE, g_names[s_persona]);
        return;
    }

    s_persona = (persona_id_t)value;
    syslog(LOG_INFO, "[%s] loaded persona: %s (%ld)\n",
           TAG, g_names[s_persona], value);
}

/* 写 /data/persona。调用方必须已经确认人格发生变化（节流在 set 里），
 * 所以这里不判断内容是否相同。失败只记日志，调用方继续用内存态。 */
static int persona_save_locked(persona_id_t id)
{
    char value = (char)('0' + (int)id);
    int fd;

    fd = open(PERSONA_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        syslog(LOG_WARNING,
               "[%s] cannot write %s (%d), persona stays in RAM only\n",
               TAG, PERSONA_FILE, errno);
        return ERROR;
    }

    /* 1 字节一次 write：没有临时文件、没有 rename、没有 fsync，把 NOR
     * flash 的写次数和写窗口都压到最小。 */
    if (write(fd, &value, 1) != 1) {
        syslog(LOG_WARNING,
               "[%s] short write to %s (%d), persona stays in RAM only\n",
               TAG, PERSONA_FILE, errno);
        close(fd);
        return ERROR;
    }

    if (close(fd) != 0) {
        syslog(LOG_WARNING, "[%s] close %s failed (%d)\n",
               TAG, PERSONA_FILE, errno);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] saved persona: %s (%d)\n",
           TAG, g_names[id], (int)id);
    return OK;
}

persona_id_t persona_get(void)
{
    persona_id_t id;

    pthread_mutex_lock(&s_lock);
    persona_load_locked();
    id = s_persona;
    pthread_mutex_unlock(&s_lock);

    return id;
}

int persona_set(persona_id_t id)
{
    if ((int)id < 0 || id >= PERSONA_COUNT) {
        return ERROR;
    }

    pthread_mutex_lock(&s_lock);
    persona_load_locked();

    /* 节流：人格没变就完全不动 flash。连续两次 persona gentle / 模型
     * 重复调用 set_persona 都不会产生写。 */
    if (s_persona == id) {
        pthread_mutex_unlock(&s_lock);
        return OK;
    }

    s_persona = id;

    /* 写盘期间持锁：避免两个线程同时切换时写序颠倒（最后落盘的不是
     * 最后生效的）。写的是 1 字节小文件，持锁时间可以接受。 */
    (void)persona_save_locked(id);
    pthread_mutex_unlock(&s_lock);

    /* 持久化失败已经降级为内存态，对调用方仍算切换成功。 */
    return OK;
}

const char *persona_get_system_prompt(void)
{
    const char *prompt;

    pthread_mutex_lock(&s_lock);
    persona_load_locked();
    prompt = g_prompts[s_persona];
    pthread_mutex_unlock(&s_lock);

    return prompt;
}

const char *persona_name(persona_id_t id)
{
    if ((int)id < 0 || id >= PERSONA_COUNT) {
        return "未知";
    }

    return g_names[id];
}

int persona_parse_name(const char *name, persona_id_t *id)
{
    static const struct {
        const char *alias;
        persona_id_t id;
    } aliases[] = {
        { "gentle", PERSONA_GENTLE },
        { "温柔",   PERSONA_GENTLE },
        { "sassy",  PERSONA_SASSY  },
        { "毒舌",   PERSONA_SASSY  },
        { "cheer",  PERSONA_CHEER  },
        { "元气",   PERSONA_CHEER  },
    };
    size_t len;
    size_t i;

    if (name == NULL || id == NULL) {
        return ERROR;
    }

    /* 容忍 JSON / 串口输入两端的空白。 */
    while (*name == ' ' || *name == '\t') {
        name++;
    }
    len = strlen(name);
    while (len > 0
           && (name[len - 1] == ' ' || name[len - 1] == '\t'
               || name[len - 1] == '\r' || name[len - 1] == '\n')) {
        len--;
    }

    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strlen(aliases[i].alias) == len
            && strncasecmp(name, aliases[i].alias, len) == 0) {
            *id = aliases[i].id;
            return OK;
        }
    }

    return ERROR;
}

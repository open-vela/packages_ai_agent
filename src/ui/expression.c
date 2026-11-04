/****************************************************************************
 * expression.c - 小云表情状态与持久化。
 *
 * 只做三件事：
 *   1. 一张 8 款的索引表（顺序 = LuminaWatch CloudAvatars.kt 的前 8 个）
 *   2. 当前选中索引的读写（内存 + /data/expression 单字节）
 *   3. 位图通过 assets/ui_expressions.c 里的 lv_image_dsc_t 暴露
 *
 * 为什么单独存 /data/expression 而不是写 config.json：
 *   和 persona.c 一样的理由 —— config.json 任何一次 set 都会整份重写，
 *   而切表情是偶尔发生的用户动作。单独 1 字节文件把 NOR flash 的写入面
 *   缩到最小（/data 与 XIP 代码同片 NOR，写 flash 会争总线）。
 *
 * 降级：/data 不存在或只读时，读失败用默认（微笑），写失败只保留内存态，
 * 不向调用方报错 —— 表情是体验项，不能因为存储故障影响上屏。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include "ui/expression.h"

#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define TAG "expression"

#define EXPRESSION_FILE "/data/expression"

/* 位图符号由 tools/convert_expr.py 生成到 assets/ui_expressions.c。
 * 命名规则 ui_expr_<CloudAvatars id>。 */
extern const lv_image_dsc_t ui_expr_wei_xiao;
extern const lv_image_dsc_t ui_expr_ping_jing;
extern const lv_image_dsc_t ui_expr_xi_xi;
extern const lv_image_dsc_t ui_expr_ou_ye;
extern const lv_image_dsc_t ui_expr_ha_ha_ha;
extern const lv_image_dsc_t ui_expr_ai;
extern const lv_image_dsc_t ui_expr_ao_li_gei;
extern const lv_image_dsc_t ui_expr_bao_chi_wei_xiao;

/* ★ 顺序就是 CloudAvatars.kt 的顺序，不要重排：
 *   表情页「第 N 款」、表盘显示、持久化里存的数字全部依赖它。 */
static const expression_t s_expressions[] =
{
  { "wei_xiao",        "微笑",     &ui_expr_wei_xiao        },
  { "ping_jing",       "平静",     &ui_expr_ping_jing       },
  { "xi_xi",           "嘻嘻",     &ui_expr_xi_xi           },
  { "ou_ye",           "欧耶",     &ui_expr_ou_ye           },
  { "ha_ha_ha",        "哈哈哈",   &ui_expr_ha_ha_ha        },
  { "ai",              "哀",       &ui_expr_ai              },
  { "ao_li_gei",       "奥利给",   &ui_expr_ao_li_gei       },
  { "bao_chi_wei_xiao","保持微笑", &ui_expr_bao_chi_wei_xiao },
};

#define EXPR_N ((int)(sizeof(s_expressions) / sizeof(s_expressions[0])))

/* LVGL 是单线程（LV_OS_NONE），这个状态只在 render 线程读写；
 * 但 expression_set 可能被工具线程调用，所以还是加锁 —— 写盘时持锁，
 * 保证落盘顺序与内存态一致。 */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int  s_current = EXPRESSION_SMILE;
static bool s_loaded;

int expression_count(void)
{
    return EXPR_N;
}

const expression_t *expression_get(int index)
{
    if (index < 0 || index >= EXPR_N)
      {
        index = 0;
      }

    return &s_expressions[index];
}

/* 读 /data/expression。只在 s_loaded == false 时调用一次。 */
static void expression_load_locked(void)
{
    char buf[8];
    char *end = NULL;
    ssize_t n;
    long value;
    int fd;

    if (s_loaded)
      {
        return;
      }

    /* 先置位：即使失败也只尝试一次，避免每次取表情都 open 一个不存在的文件 */
    s_loaded = true;

    fd = open(EXPRESSION_FILE, O_RDONLY);
    if (fd < 0)
      {
        syslog(LOG_INFO, "[%s] no saved expression (%d), using %s\n",
               TAG, errno, s_expressions[s_current].id);
        return;
      }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
      {
        syslog(LOG_WARNING, "[%s] %s is empty, using default\n",
               TAG, EXPRESSION_FILE);
        return;
      }

    buf[n] = '\0';
    value = strtol(buf, &end, 10);

    if (end == buf || value < 0 || value >= EXPR_N)
      {
        syslog(LOG_WARNING, "[%s] invalid expression '%s', using default\n",
               TAG, buf);
        return;
      }

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
      {
        end++;
      }

    if (*end != '\0')
      {
        syslog(LOG_WARNING, "[%s] trailing junk in %s, using default\n",
               TAG, EXPRESSION_FILE);
        return;
      }

    s_current = (int)value;
    syslog(LOG_INFO, "[%s] loaded expression: %s (%ld)\n",
           TAG, s_expressions[s_current].id, value);
}

/* 写 /data/expression。调用方必须已确认索引发生变化。 */
static int expression_save_locked(int index)
{
    char value = (char)('0' + index);
    int fd;

    fd = open(EXPRESSION_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
      {
        syslog(LOG_WARNING,
               "[%s] cannot write %s (%d), expression stays in RAM only\n",
               TAG, EXPRESSION_FILE, errno);
        return -1;
      }

    if (write(fd, &value, 1) != 1)
      {
        syslog(LOG_WARNING,
               "[%s] short write to %s (%d), expression stays in RAM only\n",
               TAG, EXPRESSION_FILE, errno);
        close(fd);
        return -1;
      }

    close(fd);
    syslog(LOG_INFO, "[%s] saved expression: %s (%d)\n",
           TAG, s_expressions[index].id, index);
    return 0;
}

int expression_current(void)
{
    int index;

    pthread_mutex_lock(&s_lock);
    expression_load_locked();
    index = s_current;
    pthread_mutex_unlock(&s_lock);

    return index;
}

int expression_set(int index)
{
    if (index < 0 || index >= EXPR_N)
      {
        return -1;
      }

    pthread_mutex_lock(&s_lock);
    expression_load_locked();

    /* 节流：没变就完全不动 flash */
    if (s_current == index)
      {
        pthread_mutex_unlock(&s_lock);
        return 0;
      }

    s_current = index;
    (void)expression_save_locked(index);
    pthread_mutex_unlock(&s_lock);

    /* 持久化失败已降级为内存态，对调用方仍算切换成功 */
    return 0;
}

/****************************************************************************
 * health_store.h - 健康数据小文件持久化（步数/心率历史共用）
 *
 * 2026-10-31: 新增。之前步数/心率都只在 RAM，重启即丢。这里提供一个
 * 极简的「名字 -> JSON 文本文件」存取层，文件落在 /data/ai_agent/health/
 * 下（littlefs，掉电保持）。
 *
 * 设计约束：
 *  - 只存文本（JSON），解析交给调用方（cJSON）；
 *  - 写入用 tmp + rename 原子替换，避免半写坏文件；
 *  - 目录不存在时自动创建；
 *  - 允许并发调用（内部互斥），但**不要**在高频路径上每步都调 save。
 ****************************************************************************/

#ifndef AI_AGENT_HEALTH_STORE_H
#define AI_AGENT_HEALTH_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（创建目录）。幂等。返回 0 成功。 */
int health_store_init(void);

/** 读取 name 对应的 JSON 文本。
 *  返回实际长度（>=0）；文件不存在返回 -ENOENT；其他负值为错误。
 *  buf 保证以 '\0' 结尾（cap>0 时）。 */
int health_store_load(const char *name, char *buf, size_t cap);

/** 原子保存 name 对应的 JSON 文本（tmp + rename）。
 *  返回 0 成功。 */
int health_store_save(const char *name, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_HEALTH_STORE_H */

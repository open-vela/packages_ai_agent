/*
 * Host-only shim for the ai_agent regression tests.
 *
 * NuttX 的 <sys/types.h> 提供 OK / ERROR，glibc 不提供。agent_compat.h
 * 被各个源文件最先包含，所以在编译命令里用 -include 把这两个宏放在最前面。
 */

#ifndef AI_AGENT_HOST_COMPAT_H
#define AI_AGENT_HOST_COMPAT_H

#include <sys/types.h>

#ifndef OK
#define OK 0
#endif

#ifndef ERROR
#define ERROR -1
#endif

#endif /* AI_AGENT_HOST_COMPAT_H */

/*
 * Host test stub: tool_registry_invalidate().
 *
 * skill_loader.c 的 skill_loader_refresh() 会调用它。host 用例只链接
 * skill_loader.c，不需要整个工具注册表，因此在这里提供一个空实现。
 */

#include "tools/tool_registry.h"

void tool_registry_invalidate(void)
{
}

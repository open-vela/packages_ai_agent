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

#pragma once

#include <stdbool.h>

/**
 * time_sync.h — 网络时间同步(SNTP)守护
 *
 * 起一个监督线程:链路通了以后自己发 48 字节 SNTP 请求(UDP/123),
 * 用服务器的 transmit timestamp 调 settimeofday(),之后每 30 分钟再校。
 * 之所以不用 apps/netutils/ntpclient:它的启动挂在 netinit 的
 * NETINIT_HAVE_NETDEV 分支里,而 AP 没有 netdev(网络经 usrsock 走 CP),
 * 开机根本执行不到,时钟就停在 1970。
 *
 * 幂等,重复调用无副作用。
 *
 * @return 0 成功(或已在运行);负值线程创建失败。
 */
int time_sync_start(void);

/** 是否至少成功同步过一次(用于诊断命令)。 */
bool time_sync_has_synced(void);

/** 最近一次失败原因(人类可读,如 "resolve … failed"/"recv … errno")。 */
const char* time_sync_last_error(void);

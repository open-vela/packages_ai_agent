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
/**
 * cell_net.h — 4G 蜂窝模块（合宙 Air780E）接入
 *
 * 硬件：UART2 = /dev/ttyS0（PA20/PA27，排针第 8/10 脚），115200 8N1。
 * 数据面：NuttX 用户态 pppd 自己 open /dev/tun 建 ppp 网卡并配置 IP/DNS，
 * 因此本模块只负责 AT 控制和“拉起 pppd 线程”。
 *
 * 串口是独占资源：AT 与 pppd 不能同时读写同一条 UART。PPP 运行期间
 * cell_at()/cell_uart_probe() 会返回 -EBUSY，而不是把 AT 字节插进 PPP 帧里。
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 读取 APN 配置（net.apn，缺省 cmnet）。可以重复调用。返回 0。 */
int cell_net_init(void);

/** 当前 APN。返回指向静态缓冲的指针；cell_set_apn() 会原地更新。 */
const char *cell_get_apn(void);

/** 设置并持久化 APN（写入 config key "net.apn"）。成功返回 0，失败负 errno。 */
int cell_set_apn(const char *apn);

/**
 * 发送一条 AT 命令并收集响应（含 OK/ERROR/CONNECT）。
 * @param cmd     不含 CR/LF 的命令，例如 "AT+CSQ"
 * @param out     响应缓冲，可为 NULL
 * @param out_cap 响应缓冲大小
 * @return 0 收到 OK/CONNECT；-EBUSY PPP 占用串口；-EIO 收到 ERROR；
 *         -ETIMEDOUT 1.5s 内无最终响应；其他负 errno 为 I/O 错误。
 */
int cell_at(const char *cmd, char *out, size_t out_cap);

/** 打开 /dev/ttyS0 并发 "AT" 等 "OK"。成功返回 0。 */
int cell_uart_probe(void);

/** 启动后台 pppd 线程（幂等）。成功/已在运行返回 0，未编译 PPPD 返回 -ENOSYS。 */
int cell_ppp_start(void);

/** 请求停止 PPP。见 cell_net.c 中的说明：本树 pppd 没有取消接口。 */
int cell_ppp_stop(void);

/** 多行状态文本：UART、APN、PPP 线程、最近 AT 响应、ppp0 是否有 IPv4。 */
int cell_get_status(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

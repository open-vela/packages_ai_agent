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

/**
 * ui_palette.h — 真机 UI 统一调色板（对齐 HachimoDock 设备端设计语言）。
 *
 * 取代各 view 文件里散落的霓虹 hex 色值。HachimoDock 设备端（pet_p4_renderer.c）
 * 的设计语言：近黑底 + 白卡片 + 深灰投影 + 浅描边 + 柔和低饱和状态色 + 品牌橙
 * 作为「主任务/焦点/积极反馈」的强调色。ink/muted 用于卡片上的主/次文字。
 *
 * 状态色语义与 pet_status_view 的 agent 状态镜像一致：
 * 灰=待机、teal=聆听、蓝=思考、橙=执行、琥珀=等待、红=错误、绿=结论/成功。
 *
 * 颜色统一为 RGB 0xRRGGBB，供 lv_color_hex() 使用（RGB565 平台自动量化）。
 */

#pragma once

#include <stdint.h>

/* 背景与面板 */
#define UI_PAL_BG           0x0F1011  /* 全局近黑背景 */
#define UI_PAL_CARD         0xFFFFFF  /* 白卡片底 */
#define UI_PAL_CARD_SHADOW  0x2E3230  /* 卡片投影（深灰） */
#define UI_PAL_LINE         0xD8DDDA  /* 卡片浅描边 / 分隔线 */
#define UI_PAL_TRACK        0x232628  /* 弧环 / guide ring 未激活轨道（黑底暗轨） */

/* 文字 */
#define UI_PAL_INK          0x1C1F1F  /* 主文字（卡片上，近黑） */
#define UI_PAL_MUTED        0x5C6260  /* 次文字（灰） */
#define UI_PAL_TEXT_DIM     0xC9CECC  /* 黑底上的状态 / 说明文字（比卡片 muted 亮，黑底对比需要） */

/* 品牌强调 */
#define UI_PAL_ACCENT       0xFA730B  /* 品牌橙：主操作 / 焦点 / 积极反馈 */

/* 状态色（柔和低饱和） */
#define UI_PAL_IDLE         0xAAB0AD  /* 待机   灰   */
#define UI_PAL_LISTEN       0x00B0A4  /* 聆听   teal */
#define UI_PAL_THINK        0x60C4FF  /* 思考   蓝   */
#define UI_PAL_EXEC         0xFA730B  /* 执行   品牌橙 */
#define UI_PAL_WAIT         0xFFA31F  /* 等待   琥珀 */
#define UI_PAL_ERROR        0xEB5952  /* 错误   红   */
#define UI_PAL_DONE         0x55C185  /* 结论/成功 绿 */

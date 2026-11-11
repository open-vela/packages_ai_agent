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
 * standby_view.h — 待机界面（暗色数字时钟）。
 *
 * 长时间无操作后(由 lvgl_ui_channel.c 的 60s 空闲计时器触发),主界面切到一块
 * 极简的暗色数字时钟(大号 HH:MM + 日期 + "待机中"提示),比宠物/粒子页更暗更静,
 * 一眼可辨是"睡了"。任意真实交互都会调 standby_view_hide() 即时切回先前页面。
 *
 * 转场只对内容层做淡入(背景与主屏同色,换屏瞬时不可见),不整屏 flush。
 *
 * 与主腿的关系:本模块是一个独立 LVGL 屏幕,只在空闲超时后被 show,不被任何
 * cold-start 或 set_page 逻辑加载;退出时记住并恢复 show 前的活动屏。
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化待机屏(暗色数字时钟),但不加载。首次 show 前必须 init,重复调用幂等。
 * 必须在 LVGL 线程(LVGL 已初始化后)调用。
 */
int standby_view_init(void);

/**
 * 渐隐渐显进入待机屏(记住当前活动屏)。重复调用(已可见)为 no-op。
 * 线程安全:异步调度到 LVGL 线程。
 */
void standby_view_show(void);

/**
 * 渐隐渐显退出待机屏,恢复 show 前的活动屏。重复调用(已隐藏)为 no-op。
 * 线程安全:异步调度到 LVGL 线程。
 */
void standby_view_hide(void);

/**
 * 同步版本:调用者已经处在 LVGL 线程里(例如审批/通知的 async 回调),需要
 * 在紧接着加载自己的屏之前把待机屏让开,不能等异步回调排到后面(否则异步的
 * 退屏会把刚加载的审批页/通知页又顶掉)。只允许在 LVGL 线程调用。
 */
void standby_view_show_now(void);
void standby_view_hide_now(void);

/**
 * 待机屏当前是否可见。
 */
bool standby_view_is_visible(void);

#ifdef __cplusplus
}
#endif

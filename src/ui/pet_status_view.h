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
 * pet_status_view.h — 圆屏宠物状态环（M7 差异化页，纯 LVGL 退路）。
 *
 * UI方案.md §2.4 方案①（圆屏宠物状态环）+ §2.4 退路：若 Rust/mirui 的
 * no_std runtime 不就绪，差异化腿降级为纯 LVGL 复刻 HachimoDock 的「宠物
 * 动画镜像 agent 状态」视觉。本模块即该退路的 CP 侧实现——一个独立 LVGL
 * 屏幕，承载一只随 agent 状态变色/变动的宠物脸 + 状态环，作为待机首屏。
 *
 * 与主腿（聊天气泡 chat_view）的关系：宠物屏是 cold-start 首屏与会话空闲
 * 时的回屏；任一聊天消息（send/send_user）会自动切到 chat_view（既有
 * 自动 show 路径），消息空闲一段时间后由 lvgl_ui_channel 的空闲定时器调
 * pet_status_view_show() 回到宠物屏。状态由 AP 经 RPMsg SET_PET_STATE 驱动，
 * 也会镜像 SET_PTT（录音/识别中）以最低成本让宠物在 PTT 流程中“活”起来。
 *
 * 线程安全：与 chat_view 一致，所有 widget 操作经 lv_async_call 调度到
 * LVGL 线程；set_state 仅记录目标态 + 投递异步任务。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 宠物状态。语义借 HachimoDock 的 agent 状态镜像。 */
enum pet_state {
    PET_STATE_IDLE      = 0,  /* 待机：呼吸 + 绿环满，眼睛居中 */
    PET_STATE_LISTENING = 1, /* 听音(PTT 录音)：环旋转 + 青色，眼睛朝下 */
    PET_STATE_THINKING  = 2, /* 思考(ASR/LLM)：环旋转 + 蓝色，眼睛朝上 */
    PET_STATE_EXECUTING = 3, /* 执行(工具/动作)：环填充 + 橙色，眼睛聚焦 */
    PET_STATE_WAITING   = 4, /* 等待确认：眨眼 + 黄色 */
    PET_STATE_ERROR     = 5, /* 错误：抖动 + 红色，眼睛 X */
};

/** 宠物形象。**每次烧录随机固定一只**（首次启动选中后写入 config 的
 *  `pet_type`，整片烧录清 NVS 才会换），运行中不换、更不随页面切换：所有
 * 页面和状态都只换这只宠物的表情。`set_ptype` 仅作调试，改了会持久化。 */
enum pet_type {
    PET_TYPE_CAT  = 0,  /* 白猫脸（粉耳 + 腮红 + 2×2 眼） */
    PET_TYPE_FROG = 1,  /* 绿青蛙（2×2 大眼） */
    PET_TYPE_DOG  = 2,  /* 棕狗（垂耳 + 黑鼻 + 粉吐舌） */
    PET_TYPE_PIG  = 3,  /* 粉猪（大耳 + 两个黑鼻孔） */
    PET_TYPE_COUNT = 4,
};

/**
 * 初始化宠物屏（创建 widgets，但不加载到前台）。
 * 必须在 LVGL 已起（lvgl_ui_channel_init 之后）后调用。
 * 幂等。返回 0 成功。
 */
int pet_status_view_init(void);

/**
 * 将宠物屏加载到前台（保存当前屏为 prev，便于 hide 恢复）。
 * 经 lv_async_call 在 LVGL 线程执行。幂等。
 */
void pet_status_view_show(void);

/**
 * 恢复 show 之前的屏幕（宠物屏退到后台但不销毁）。
 * 经 lv_async_call 在 LVGL 线程执行。
 */
void pet_status_view_hide(void);

/** 宠物屏当前是否在前台（缓存的 visible 标志）。 */
bool pet_status_view_is_visible(void);

/**
 * 宠物屏是否为当前活动屏（与 lv_screen_active() 实时比较）。
 * 仅可在 LVGL 线程调用；供让位计时器等线程内逻辑判断“前台被谁占用”。
 */
bool pet_status_view_is_active(void);

/**
 * 设置宠物状态（驱动颜色/动画/眼睛/文案）。
 * 线程安全：异步调度到 LVGL 线程。无视觉效果时静默返回。
 */
void pet_status_view_set_state(enum pet_state state);

/**
 * 设置 ERROR 态下显示的具体原因文案（2~3 行、短句，如
 * "未联网\n请先连接 Wi-Fi"）。仅在 ERROR 态替换通用「出错了」标题；
 * 切回其它状态时自动清空。传 NULL/空串恢复通用文案。
 * 线程安全：异步调度到 LVGL 线程；须在 set_state(ERROR) 之前调用。
 */
void pet_status_view_set_error_text(const char *text);

/**
 * 调试用：切换宠物类型（0=猫 1=青蛙 2=狗 3=猪）。越界值被忽略，不回落。
 * 新值写入 config（`pet_type`），重启后保持——形象是烧录期固定的，
 * 不要在验收里用它当「切形象」功能。线程安全：异步调度到 LVGL 线程。
 */
void pet_status_view_set_type(int type);

/**
 * 当前固定形象（0..PET_TYPE_COUNT-1）。只读一个 int，可在任意线程调用。
 */
int pet_status_view_get_type(void);

#ifdef __cplusplus
}
#endif

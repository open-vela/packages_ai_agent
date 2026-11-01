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

/****************************************************************************
 * Loongson LS2K0300 (LOONG-HAT) Peripheral Tools for AI Agent
 *
 * Ported from apps/examples/ls_driver_test driver logic so the agent
 * can control real on-board peripherals:
 *
 *   - board_led          : GPIO72 (red) / GPIO73 (green), active-low
 *   - board_pwm          : PWM2 on GPIO88 (blue LED), duty 0-100%
 *   - board_buzzer       : GPIO75 passive buzzer, software 1kHz square wave
 *   - board_key_read     : GPIO87 (KEY1) / GPIO86 (KEY2), active-low input
 *   - board_thermal_read : on-die thermal sensor /dev/thermal0 (milli-C)
 *   - board_adc_read     : MCP3204 CH0-3 via bare-register SPI2, Vref 3.3V
 *   - board_light_read   : BH1750 lux via I2C1 (0x23/0x5C)
 *   - board_oled_show    : SSD1306 128x64 via I2C1 (0x3C)
 *
 * All hardware paths mirror ls_driver_test; device nodes are opened
 * per call so multiple tools can share the bus safely.
 ****************************************************************************/

#ifndef __APPS_PACKAGES_AI_AGENT_SRC_TOOLS_TOOL_2K0300_H
#define __APPS_PACKAGES_AI_AGENT_SRC_TOOLS_TOOL_2K0300_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int tool_board_led_execute(const char *input_json,
                           char *output, size_t output_size);

int tool_board_pwm_execute(const char *input_json,
                           char *output, size_t output_size);

int tool_board_buzzer_execute(const char *input_json,
                              char *output, size_t output_size);

int tool_board_key_read_execute(const char *input_json,
                                char *output, size_t output_size);

int tool_board_thermal_read_execute(const char *input_json,
                                    char *output, size_t output_size);

int tool_board_adc_read_execute(const char *input_json,
                                char *output, size_t output_size);

int tool_board_light_read_execute(const char *input_json,
                                  char *output, size_t output_size);

int tool_board_oled_show_execute(const char *input_json,
                                 char *output, size_t output_size);

/* ── Skill-compat interfaces (same names as smarthome skill) ────
 *
 * The agent_skills/smarthome.md skill teaches the LLM these tool
 * names; they map skill semantics onto real board hardware:
 *
 *   smart_light_control:
 *     device "living_room" -> red LED (GPIO72)
 *     device "bedroom"     -> green LED (GPIO73)
 *     device "kitchen"     -> blue LED via PWM (GPIO88)
 *     action turn_on / turn_off / set_brightness / set_color
 *
 *   env_sensor_read:
 *     temperature -> on-die thermal sensor (/dev/thermal0)
 *     light       -> BH1750 lux (I2C1)
 *     humidity    -> not available on this board (reported in JSON)
 */
int tool_smart_light_control_execute(const char *input_json,
                                     char *output, size_t output_size);

int tool_env_sensor_read_execute(const char *input_json,
                                 char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_PACKAGES_AI_AGENT_SRC_TOOLS_TOOL_2K0300_H */

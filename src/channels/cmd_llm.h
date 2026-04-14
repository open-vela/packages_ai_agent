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

void cmd_set_llm(int argc, char** argv);
void cmd_set_vision_llm(int argc, char** argv);
void cmd_list_models(int argc, char** argv);
void cmd_router_status(void);
void cmd_router_profile(int argc, char** argv);
void cmd_router_set(int argc, char** argv);
void cmd_router_clear(int argc, char** argv);
void cmd_router_model(int argc, char** argv);

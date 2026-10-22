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
 * agent_llm.h — change the chat model without handling credentials.
 *
 * A provisioned device can carry a model name its account does not serve.
 * Re-provisioning means writing the key to staging, which on a serial only
 * device puts the key on the wire.  This call keeps the configured host,
 * path and key and replaces only the model of backend 0, then applies and
 * persists it through the same path set_llm uses.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success, -EINVAL for an empty or oversized name, -ENODEV when
 * no backend is configured, -EIO when the update could not be applied. */
int agent_llm_set_model(const char *model);

#ifdef __cplusplus
}
#endif

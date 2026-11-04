/****************************************************************************
 * apps/ai_agent: HTTP ASR backend header (self-hosted whisper)
 ****************************************************************************/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Register the self-hosted HTTP ASR backend ("http") with the
 * voice_asr framework. Register it BEFORE volc_asr_register() so it
 * becomes the default backend; volc stays available via
 * voice_asr_set_backend(). */
int http_asr_register(void);

/* Reload the endpoint URL from the config store (claw_config_get
 * key "asr_http_url"), falling back to CONFIG_AI_AGENT_ASR_HTTP_URL. */
int http_asr_init(void);

/* Override the endpoint base URL at runtime (no query string needed;
 * task/language/output params are appended automatically). */
void http_asr_set_url(const char *url);

#ifdef __cplusplus
}
#endif

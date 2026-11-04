/****************************************************************************
 * apps/packages/ai_agent/src/tools/tool_weather.h
 *
 * Desktop companion "refresh_weather" agent tool — public interface.
 *
 * Pulls current weather from Open-Meteo (no API key) and writes
 * /tmp/weather.txt as a single line:  "<weather_text> <temp_celsius>\n"
 * e.g.  "晴 25"
 *
 * The info_screen app reads /tmp/weather.txt to render the weather line.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __PACKAGES_AI_AGENT_SRC_TOOLS_TOOL_WEATHER_H
#define __PACKAGES_AI_AGENT_SRC_TOOLS_TOOL_WEATHER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Tool name as registered in the agent tool registry (tool_registry.c) */
#define WEATHER_TOOL_NAME "refresh_weather"

/* Path of the file consumed by the info_screen framebuffer app */
#define WEATHER_FILE_PATH "/tmp/weather.txt"

/* How often the background thread refreshes weather (seconds) */
#define WEATHER_REFRESH_INTERVAL_SEC 600

/* Delay before the first refresh after boot (seconds) — let WiFi come up */
#define WEATHER_FIRST_DELAY_SEC 30

/* Maximum age of /tmp/weather.txt that the reader still treats as valid */
#define WEATHER_MAX_AGE_SEC 1800

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/* Execute the refresh_weather tool synchronously.
 *
 * input_json  - JSON parameters (this tool takes none, may be NULL/empty)
 * output      - caller buffer receiving a human readable result string
 * output_size - size of the output buffer
 *
 * Returns 0 on success (file written), <0 on error (file NOT touched so the
 * previous /tmp/weather.txt, if any, is preserved).
 */
int tool_refresh_weather_execute(const char *input_json,
                                 char *output, size_t output_size);

/* Start the background weather-refresh thread.  Call once during agent
 * bring-up.  The thread sleeps WEATHER_FIRST_DELAY_SEC after boot, then
 * refreshes every WEATHER_REFRESH_INTERVAL_SEC.
 */
void weather_tool_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __PACKAGES_AI_AGENT_SRC_TOOLS_TOOL_WEATHER_H */

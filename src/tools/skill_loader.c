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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "tools/skill_loader.h"
#include "tools/tool_registry.h"
#include "agent_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "skills";

/* ── Built-in skill contents ─────────────────────────────────── */

#define BUILTIN_WEATHER \
    "# Weather\n" \
    "\n" \
    "Get current weather and forecasts using web_search.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks about weather, temperature, or forecasts.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time to know the current date\n" \
    "2. Use web_search with a query like \"weather in [city] today\"\n" \
    "3. Extract temperature, conditions, and forecast from results\n" \
    "4. Present in a concise, friendly format\n" \
    "\n" \
    "## Example\n" \
    "User: \"What's the weather in Tokyo?\"\n" \
    "→ get_current_time\n" \
    "→ web_search \"weather Tokyo today February 2026\"\n" \
    "→ \"Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north.\"\n"

#define BUILTIN_DAILY_BRIEFING \
    "# Daily Briefing\n" \
    "\n" \
    "Compile a personalized daily briefing for the user.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks for a daily briefing, morning update, or \"what's new today\".\n" \
    "Also useful as a heartbeat/cron task.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time for today's date\n" \
    "2. Read " AGENT_MEMORY_DIR "/MEMORY.md for user preferences and context\n" \
    "3. Read today's daily note if it exists\n" \
    "4. Use web_search for relevant news based on user interests\n" \
    "5. Compile a concise briefing covering:\n" \
    "   - Date and time\n" \
    "   - Weather (if location known from USER.md)\n" \
    "   - Relevant news/updates based on user interests\n" \
    "   - Any pending tasks from memory\n" \
    "   - Any scheduled cron jobs\n" \
    "\n" \
    "## Format\n" \
    "Keep it brief — 5-10 bullet points max. Use the user's preferred language.\n"

#define BUILTIN_SKILL_CREATOR \
    "# Skill Creator\n" \
    "\n" \
    "Create new skills for AI Agent.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to create a new skill, teach the bot something, or add a new capability.\n" \
    "\n" \
    "## How to create a skill\n" \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n" \
    "2. Write a SKILL.md file with this structure:\n" \
    "   - `# Title` — clear name\n" \
    "   - Brief description paragraph\n" \
    "   - `## When to use` — trigger conditions\n" \
    "   - `## How to use` — step-by-step instructions\n" \
    "   - `## Example` — concrete example (optional but helpful)\n" \
    "3. Save to `" AGENT_SKILLS_DIR "<name>.md` using write_file\n" \
    "4. The skill will be automatically available after the next conversation\n" \
    "\n" \
    "## Best practices\n" \
    "- Keep skills concise — the context window is limited\n" \
    "- Focus on WHAT to do, not HOW (the agent is smart)\n" \
    "- Include specific tool calls the agent should use\n" \
    "- Test by asking the agent to use the new skill\n" \
    "\n" \
    "## Example\n" \
    "To create a \"translate\" skill:\n" \
    "write_file path=\"" AGENT_SKILLS_DIR "translate.md\" content=\"# Translate\\n\\nTranslate text between languages.\\n\\n" \
    "## When to use\\nWhen the user asks to translate text.\\n\\n" \
    "## How to use\\n1. Identify source and target languages\\n" \
    "2. Translate directly using your language knowledge\\n" \
    "3. For specialized terms, use web_search to verify\\n\"\n"

#define BUILTIN_SYSTEM_HEALTH \
    "# System Health Check\n\n" \
    "Check AI Agent system status and summarize key info.\n\n" \
    "## When to use\n" \
    "When user asks about system status, health check, or running state.\n\n" \
    "## How to use\n" \
    "1. get_current_time to get current time\n" \
    "2. list_dir to list /data/agent/ files\n" \
    "3. read_file /data/agent/config/config.json to check config\n" \
    "4. cron_list to check scheduled tasks\n" \
    "5. Summarize: time, file count, config status, cron jobs\n"

#define BUILTIN_REMINDER \
    "# Reminder\n\n" \
    "Set timed reminders that auto-notify the user.\n\n" \
    "## When to use\n" \
    "When user says remind me, set alarm, notify me later.\n\n" \
    "## How to use\n" \
    "1. get_current_time for current epoch\n" \
    "2. Parse user request into schedule_type and timing\n" \
    "3. Set channel/chat_id matching the message source (feishu/system)\n" \
    "4. cron_add to create the job\n" \
    "5. Confirm with trigger time\n"

#define BUILTIN_NOTE_TAKER \
    "# Note Taker\n\n" \
    "Quick notes saved to daily diary files.\n\n" \
    "## When to use\n" \
    "ONLY when user explicitly asks to take a note, save a memo, or record something.\n" \
    "Do NOT auto-save notes for other tasks (weather, search, etc.).\n\n" \
    "## How to use\n" \
    "1. get_current_time for today's date\n" \
    "2. Path: " AGENT_DATA_DIR "/memory/daily/YYYY-MM-DD.md\n" \
    "3. read_file to check if today's diary exists\n" \
    "4. If exists: edit_file to append. If not: write_file to create\n" \
    "5. Format: - [HH:MM] content\n"

#define BUILTIN_TRANSLATE \
    "# Translate\n\n" \
    "Translate text between languages.\n\n" \
    "## When to use\n" \
    "When user asks to translate text.\n\n" \
    "## How to use\n" \
    "1. Identify source and target languages\n" \
    "2. Translate using language knowledge\n" \
    "3. For specialized terms, use web_search to verify\n" \
    "4. Provide translation with key term notes if needed\n"

#define BUILTIN_NEWS_DIGEST \
    "# News Digest\n\n" \
    "Search and compile news summaries based on user interests.\n\n" \
    "## When to use\n" \
    "When user asks about recent news, headlines, or latest updates on a topic.\n\n" \
    "## How to use\n" \
    "1. get_current_time for current date\n" \
    "2. Determine search keywords from user request or MEMORY.md interests\n" \
    "3. news_search for relevant news (top_headlines=true for headlines)\n" \
    "4. web_search to supplement if needed\n" \
    "5. Compile 3-5 items: title, source, one-line summary\n"

#define BUILTIN_FEISHU_TEST \
    "# Feishu Integration Test\n\n" \
    "Test Feishu Bot capabilities end-to-end.\n\n" \
    "## When to use\n" \
    "When user says test feishu, feishu test, or verify feishu connection.\n\n" \
    "## How to use\n" \
    "Run these tests in sequence, report each result:\n" \
    "1. get_current_time - verify time\n" \
    "2. get_weather location=Beijing - verify weather\n" \
    "3. write_file + read_file a test file - verify file I/O\n" \
    "4. read_file " AGENT_DATA_DIR "/memory/MEMORY.md - verify memory\n" \
    "5. cron_list - verify cron\n" \
    "6. Summarize all results with pass/fail status\n"

#define BUILTIN_TASK_MANAGER \
    "# Task Manager\n\n" \
    "Manage a TODO list with add, complete, and view.\n\n" \
    "## When to use\n" \
    "When user says add task, TODO, done with X, what's pending.\n\n" \
    "## How to use\n" \
    "Task file: " AGENT_DATA_DIR "/TASKS.md\n" \
    "- View: read_file the task file\n" \
    "- Add: get_current_time, then edit_file/write_file to append: - [ ] [YYYY-MM-DD] desc\n" \
    "- Complete: edit_file to change - [ ] to - [x]\n"

#define BUILTIN_VISUAL_GUIDE \
    "# Visual Guide\n\n" \
    "Describe the camera view aloud as an informational aid. This skill does not guarantee a safe route and must not replace a mobility aid.\n\n" \
    "## When to use\n" \
    "When the user asks what is ahead, requests a scene description, or asks the device to read visible text aloud.\n\n" \
    "## How to use\n" \
    "1. Call peripheral_status. In its devices array, require the camera and audio_playback entries to have available=true.\n" \
    "2. Call network_probe with apply=false. A failure is a warning, not a reason to skip local hardware checks.\n" \
    "3. Call camera_capture with resolution=high and ask for observable objects, approximate relative positions, and visible text. Never state that a route is certainly safe.\n" \
    "4. Extract analysis from the result. If it is empty, speak a short failure message.\n" \
    "5. Call tts_speak with chunks no longer than 100 Chinese characters or 300 English characters.\n\n" \
    "## Fallback\n" \
    "- Camera unavailable: use tts_speak to report that the camera is not ready.\n" \
    "- TTS unavailable: return the camera analysis as text.\n" \
    "- Network unavailable: do not change DNS automatically; report the diagnostic result.\n"

#define BUILTIN_DEVICE_SELF_CHECK \
    "# Device Self Check\n\n" \
    "Check the board's network and multimedia peripherals, then report a concise pass/fail summary.\n\n" \
    "## When to use\n" \
    "When the user asks whether the camera, microphone, speaker, display, I2S, or network is ready.\n\n" \
    "## How to use\n" \
    "1. Call peripheral_status.\n" \
    "2. Call network_probe with apply=false.\n" \
    "3. Report every missing device node and all reachable DNS servers.\n" \
    "4. If the speaker is available and the user requested an audible result, call tts_speak with the summary.\n" \
    "5. Never call network_probe with apply=true unless the user explicitly asks to change DNS.\n\n" \
    "## Result\n" \
    "Separate hardware presence from end-to-end functional verification. A present device node means registered, not that image or audio quality has been verified.\n"

#define BUILTIN_MEAL_PLANNER \
    "# Meal Planner\n\n" \
    "Create a daily three-meal plan from the date, saved preferences, and optional activity data.\n\n" \
    "## When to use\n" \
    "When the user asks what to eat, requests a meal plan, or wants a daily calorie estimate.\n\n" \
    "## How to use\n" \
    "1. Call get_current_time and determine whether today is a weekday.\n" \
    "2. Read " AGENT_MEMORY_DIR "/MEMORY.md for allergies, preferences, and goals. Never recommend a known allergen.\n" \
    "3. Optionally call get_steps to estimate activity. If unavailable, use a neutral activity level.\n" \
    "4. Provide breakfast, lunch, and dinner with approximate calories and a daily total. Label all nutrition numbers as estimates.\n" \
    "5. For medical diets, advise confirmation with a qualified clinician.\n"

/* Built-in skill registry */
typedef struct {
    const char *filename;   /* e.g. "weather" */
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    { "weather",        BUILTIN_WEATHER        },
    { "daily-briefing", BUILTIN_DAILY_BRIEFING },
    { "skill-creator",  BUILTIN_SKILL_CREATOR  },
    { "system-health",  BUILTIN_SYSTEM_HEALTH  },
    { "reminder",       BUILTIN_REMINDER       },
    { "note-taker",     BUILTIN_NOTE_TAKER     },
    { "translate",      BUILTIN_TRANSLATE      },
    { "news-digest",    BUILTIN_NEWS_DIGEST    },
    { "feishu-test",    BUILTIN_FEISHU_TEST    },
    { "task-manager",   BUILTIN_TASK_MANAGER   },
    { "visual-guide",   BUILTIN_VISUAL_GUIDE   },
    { "device-self-check", BUILTIN_DEVICE_SELF_CHECK },
    { "meal-planner",   BUILTIN_MEAL_PLANNER   },
};

#define NUM_BUILTINS (sizeof(s_builtins) / sizeof(s_builtins[0]))

/* ── Install built-in skills if missing ──────────────────────── */

static void install_builtin(const builtin_skill_t *skill)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, skill->filename);

    /* Check if already exists */
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        syslog(LOG_DEBUG, "[%s] Skill exists: %s\n", TAG, path);
        return;
    }

    /* Write built-in skill */
    f = fopen(path, "w");
    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot write skill: %s\n", TAG, path);
        return;
    }

    fputs(skill->content, f);
    fclose(f);
    syslog(LOG_INFO, "[%s] Installed built-in skill: %s\n", TAG, path);
}

int skill_loader_init(void)
{
    syslog(LOG_INFO, "[%s] Initializing skills system\n", TAG);

    /* Ensure skills directory exists */
    mkdir(AGENT_SKILLS_DIR, 0755);

    for (size_t i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    syslog(LOG_INFO, "[%s] Skills system ready (%d built-in)\n", TAG, (int)NUM_BUILTINS);
    return OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title"
 * Returns pointer past "# " or the line itself if no prefix.
 */
static const char *extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    /*
     * On Vela/NuttX we have real directories, so we can simply opendir
     * on the skills directory and iterate over .md files.
     * (Original version used flat namespace readdir from mount root.)
     */
    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if (!dir) {
        syslog(LOG_WARNING, "[%s] Cannot open skills directory for enumeration: %s\n", TAG, AGENT_SKILLS_DIR);
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        /* Match .md files only */
        if (name_len < 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Skip hidden files */
        if (name[0] == '.') continue;

        /* Build full path */
        char full_path[256];
        snprintf(full_path, sizeof(full_path), "%s%s", AGENT_SKILLS_DIR, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        extract_description(f, desc, sizeof(desc));
        fclose(f);

        /* Append to summary */
        off += snprintf(buf + off, size - off,
            "- **%s**: %s (read with: read_file %s)\n",
            title, desc, full_path);
    }

    closedir(dir);

    buf[off] = '\0';
    syslog(LOG_INFO, "[%s] Skills summary: %d bytes\n", TAG, (int)off);
    return off;
}

/* ── Hot-reload support ──────────────────────────────────────── */

static uint32_t s_last_skill_hash;

/* Simple hash of directory listing: file count + total size */
static uint32_t compute_skills_hash(void)
{
    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if (!dir) {
        return 0;
    }

    uint32_t hash = 5381;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (name_len < 4 || strcmp(name + name_len - 3, ".md") != 0) {
            continue;
        }
        if (name[0] == '.') {
            continue;
        }

        /* Hash filename */
        for (size_t i = 0; i < name_len; i++) {
            hash = ((hash << 5) + hash) + (unsigned char)name[i];
        }

        /* Hash file size + mtime (detects content changes) */
        char path[256];
        snprintf(path, sizeof(path), "%s%s", AGENT_SKILLS_DIR, name);
        struct stat st;
        if (stat(path, &st) == 0) {
            hash = ((hash << 5) + hash) + (uint32_t)st.st_size;
            hash = ((hash << 5) + hash) + (uint32_t)st.st_mtime;
        }
    }

    closedir(dir);
    return hash;
}

bool skill_loader_check_changed(void)
{
    uint32_t current = compute_skills_hash();
    if (s_last_skill_hash == 0) {
        s_last_skill_hash = current;
        return false;
    }
    if (current != s_last_skill_hash) {
        s_last_skill_hash = current;
        return true;
    }
    return false;
}

void skill_loader_refresh(void)
{
    s_last_skill_hash = compute_skills_hash();
    /* Invalidate tool registry so next get_tools_json rebuilds */
    tool_registry_invalidate();
    syslog(LOG_INFO, "[%s] Skills refreshed (hash=%08x)\n",
        TAG, s_last_skill_hash);
}

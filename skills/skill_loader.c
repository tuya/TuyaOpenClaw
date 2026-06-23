/**
 * @file skill_loader.c
 * @brief Skill loader implementation for DuckyClaw
 *
 * Ported from TuyaOpen/apps/mimiclaw/skills/skill_loader.c.
 * Key adaptations:
 *   - MIMI_LOG*  → PR_INFO / PR_WARN / PR_ERR / PR_DEBUG
 *   - MIMI_SPIFFS_SKILLS_DIR / MIMI_SKILLS_PREFIX → CLAW_SKILLS_DIR / CLAW_SKILLS_PREFIX
 *   - tal_fopen / tal_fclose … → claw_fopen / claw_fclose … (SD/flash switching)
 *   - /spiffs/ paths in built-in skill text → CLAW_FS_ROOT_PATH
 */

#include "skill_loader.h"

#include "tool_files.h"
#include "tal_log.h"

#include <string.h>
#include <stdio.h>

/* ================================================================== */
/*  Built-in skill definitions                                        */
/* ================================================================== */

#define BUILTIN_WEATHER                                                      \
    "# Weather\n"                                                            \
    "\n"                                                                     \
    "Get current weather and forecasts using web_search.\n"                  \
    "\n"                                                                     \
    "## When to use\n"                                                       \
    "When the user asks about weather, temperature, or forecasts.\n"         \
    "\n"                                                                     \
    "## How to use\n"                                                        \
    "1. Use get_current_time to know the current date\n"                     \
    "2. Use web_search with a query like \"weather in [city] today\"\n"      \
    "3. Extract temperature, conditions, and forecast from results\n"        \
    "4. Present in a concise, friendly format\n"                             \
    "\n"                                                                     \
    "## Example\n"                                                           \
    "User: \"What's the weather in Tokyo?\"\n"                               \
    "-> get_current_time\n"                                                  \
    "-> web_search \"weather Tokyo today\"\n"                                \
    "-> \"Tokyo: 8C, partly cloudy. High 12C, low 4C. Light wind.\"\n"

#define BUILTIN_DAILY_BRIEFING                                               \
    "# Daily Briefing\n"                                                     \
    "\n"                                                                     \
    "Compile a personalized daily briefing for the user.\n"                  \
    "\n"                                                                     \
    "## When to use\n"                                                       \
    "When the user asks for a daily briefing, morning update, "              \
    "or \"what's new today\".\n"                                             \
    "Also useful as a heartbeat/cron task.\n"                                \
    "\n"                                                                     \
    "## How to use\n"                                                        \
    "1. Use get_current_time for today's date\n"                             \
    "2. Read " CLAW_FS_ROOT_PATH "/memory/MEMORY.md for user "              \
    "preferences and context\n"                                              \
    "3. Read today's daily note if it exists\n"                              \
    "4. Use web_search for relevant news based on user interests\n"          \
    "5. Compile a concise briefing covering:\n"                              \
    "   - Date and time\n"                                                   \
    "   - Weather (if location known from USER.md)\n"                        \
    "   - Relevant news/updates based on user interests\n"                   \
    "   - Any pending tasks from memory\n"                                   \
    "   - Any scheduled cron jobs\n"                                         \
    "\n"                                                                     \
    "## Format\n"                                                            \
    "Keep it brief - 5-10 bullet points max. "                               \
    "Use the user's preferred language.\n"

#define BUILTIN_SKILL_CREATOR                                                \
    "# Skill Creator\n"                                                      \
    "\n"                                                                     \
    "Create new skills for DuckyClaw.\n"                                     \
    "\n"                                                                     \
    "## When to use\n"                                                       \
    "When the user asks to create a new skill, teach the bot "               \
    "something, or add a new capability.\n"                                  \
    "\n"                                                                     \
    "## How to create a skill\n"                                             \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n"          \
    "2. Write a SKILL.md file with this structure:\n"                        \
    "   - `# Title` - clear name\n"                                          \
    "   - Brief description paragraph\n"                                     \
    "   - `## When to use` - trigger conditions\n"                           \
    "   - `## How to use` - step-by-step instructions\n"                     \
    "   - `## Example` - concrete example (optional but helpful)\n"          \
    "3. Save to `" CLAW_SKILLS_PREFIX "<name>.md` using write_file\n"        \
    "4. The skill will be automatically available after the "                \
    "next conversation\n"                                                    \
    "\n"                                                                     \
    "## Best practices\n"                                                    \
    "- Keep skills concise - the context window is limited\n"                \
    "- Focus on WHAT to do, not HOW (the agent is smart)\n"                  \
    "- Include specific tool calls the agent should use\n"                   \
    "- Test by asking the agent to use the new skill\n"

#define BUILTIN_WATCH_FOR_VISITOR                                            \
    "# Watch for visitor\n"                                                  \
    "\n"                                                                     \
    "Periodically check the camera for visitors. Uses cron to run every "    \
    "30 seconds until the user says to stop. Each run must use "             \
    "device_camera_take_photo to get the latest image.\n"                     \
    "\n"                                                                     \
    "## When to use\n"                                                       \
    "When the user asks to watch for someone coming (e.g. \"watch for "     \
    "visitors\", \"tell me when someone arrives\"). Stop when the user "    \
    "says to stop (e.g. \"stop watching\", \"cancel\", \"stop checking\").\n" \
    "\n"                                                                     \
    "## How to use\n"                                                        \
    "1. **Start watching**: Call cron_add with:\n"                           \
    "   - name: \"watch-for-visitor\"\n"                                      \
    "   - schedule_type: \"every\"\n"                                        \
    "   - interval_s: 30\n"                                                  \
    "   - message: A short instruction that when the job fires, the agent "   \
    "must call device_camera_take_photo to get the latest image, then "       \
    "determine whether a person is present; if yes, tell the user.\n"        \
    "   Example message: \"[Watch for visitor] Call device_camera_take_photo, " \
    "check the image for a person; if someone is present, tell the user.\"\n" \
    "2. Confirm to the user that watching has started (every 30s).\n"       \
    "3. **Stop watching**: When the user says to stop, call cron_list, "     \
    "find the job whose name is \"watch-for-visitor\", note its id, then "    \
    "call cron_remove with that job_id. Tell the user watching has stopped.\n" \
    "\n"                                                                     \
    "## Example\n"                                                           \
    "User: \"Watch for visitors\"\n"                                         \
    "-> cron_add(name=\"watch-for-visitor\", schedule_type=\"every\", "       \
    "interval_s=30, message=...)\n"                                         \
    "-> Reply: Started checking the camera every 30 seconds; I will tell " \
    "you if someone arrives.\n"                                             \
    "User: \"Stop watching\"\n"                                               \
    "-> cron_list, find job watch-for-visitor, cron_remove(job_id)\n"        \
    "-> Reply: Watching has stopped.\n"

/* ------------------------------------------------------------------ */
/*  Built-in skill table                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

typedef struct {
    const char *filename;
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    {"weather",           BUILTIN_WEATHER           },
    {"daily-briefing",    BUILTIN_DAILY_BRIEFING    },
    {"skill-creator",     BUILTIN_SKILL_CREATOR     },
    // {"watch-for-visitor", BUILTIN_WATCH_FOR_VISITOR },
};

#define NUM_BUILTINS ((int)(sizeof(s_builtins) / sizeof(s_builtins[0])))

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

static void install_builtin(const builtin_skill_t *skill)
{
    if (!skill || !skill->filename || !skill->content) {
        return;
    }

    char path[160] = {0};
    snprintf(path, sizeof(path), "%s%s.md", CLAW_SKILLS_PREFIX, skill->filename);

    BOOL_T exists = FALSE;
    if (claw_fs_is_exist(path, &exists) == OPRT_OK && exists) {
        PR_DEBUG("skill exists: %s", path);
        return;
    }

    TUYA_FILE f = claw_fopen(path, "w");
    if (!f) {
        PR_ERR("cannot write skill: %s", path);
        return;
    }

    int wn = claw_fwrite((void *)skill->content, (int)strlen(skill->content), f);
    claw_fclose(f);
    if (wn < 0) {
        PR_ERR("write skill failed: %s", path);
        return;
    }

    PR_INFO("installed built-in skill: %s", path);
}

/* ------------------------------------------------------------------ */

static const char *extract_title(const char *line, size_t len,
                                 char *out, size_t out_size)
{
    const char *start = line;
    if (!line || !out || out_size == 0) {
        return "";
    }

    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len  -= 2;
    }

    while (len > 0 && (start[len - 1] == '\n' ||
                       start[len - 1] == '\r' ||
                       start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

static void extract_description(TUYA_FILE f, char *out, size_t out_size)
{
    if (!f || !out || out_size == 0) {
        return;
    }

    size_t off = 0;
    char *line = (char *)claw_malloc(256);
    if (!line) {
        return;
    }
    memset(line, 0, 256);

    while (claw_fgets(line, 256, f) && off < out_size - 1) {
        size_t len = strlen(line);

        if (len == 0 ||
            (len == 1 && line[0] == '\n' && off > 0) ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        if (line[0] == '\n') {
            continue;
        }

        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    claw_free(line);

    while (off > 0 && out[off - 1] == ' ') {
        off--;
    }
    out[off] = '\0';
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

OPERATE_RET skill_loader_init(void)
{
    BOOL_T exists = FALSE;
    if (claw_fs_is_exist(CLAW_SKILLS_DIR, &exists) != OPRT_OK || !exists) {
        int mk_rt = claw_fs_mkdir(CLAW_SKILLS_DIR);
        if (mk_rt != OPRT_OK) {
            PR_ERR("mkdir failed: %s rt=%d", CLAW_SKILLS_DIR, mk_rt);
            return mk_rt;
        }
    }

    for (int i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    PR_INFO("skills ready (%d built-in)", NUM_BUILTINS);
    return OPRT_OK;
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return 0;
    }

    TUYA_DIR dir = NULL;
    if (claw_dir_open(CLAW_SKILLS_DIR, &dir) != OPRT_OK || !dir) {
        PR_WARN("cannot open skills dir: %s", CLAW_SKILLS_DIR);
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;

    char *full_path = (char *)claw_malloc(256);
    char *desc = (char *)claw_malloc(256);
    if (!full_path || !desc) {
        claw_free(full_path);
        claw_free(desc);
        claw_dir_close(dir);
        return 0;
    }

    while (off < size - 1) {
        TUYA_FILEINFO info = NULL;
        if (claw_dir_read(dir, &info) != OPRT_OK || !info) {
            break;
        }

        const char *name = NULL;
        if (claw_dir_name(info, &name) != OPRT_OK || !name) {
            continue;
        }

        size_t name_len = strlen(name);
        if (name_len < 4 || strcmp(name + name_len - 3, ".md") != 0) {
            continue;
        }

        memset(full_path, 0, 256);
        snprintf(full_path, 256, "%s/%s", CLAW_SKILLS_DIR, name);

        TUYA_FILE f = claw_fopen(full_path, "r");
        if (!f) {
            continue;
        }

        char first_line[128] = {0};
        if (!claw_fgets(first_line, sizeof(first_line), f)) {
            claw_fclose(f);
            continue;
        }

        char title[64] = {0};
        (void)extract_title(first_line, strlen(first_line), title, sizeof(title));

        memset(desc, 0, 256);
        extract_description(f, desc, 256);
        claw_fclose(f);

        off += (size_t)snprintf(buf + off, size - off,
                                "- **%s**: %s (read with: read_file %s)\n",
                                title,
                                desc[0] ? desc : "(no description)",
                                full_path);
    }

    claw_free(full_path);
    claw_free(desc);
    claw_dir_close(dir);

    if (off >= size) {
        off = size - 1;
    }
    buf[off] = '\0';

    PR_INFO("skills summary bytes=%u", (unsigned)off);
    return off;
}

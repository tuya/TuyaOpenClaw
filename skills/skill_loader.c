/**
 * @file skill_loader.c
 * @brief Skill loader implementation for TuyaOpenClaw
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
    "Create new skills for TuyaOpenClaw.\n"                                  \
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
/*  Lua skills (lua_run / lua_gpio / lua_delay)                       */
/*                                                                    */
/*  Synced from docs/skills/lua_*.md. Keep these condensed: full      */
/*  reference + extra examples live in the docs tree, and can be      */
/*  pushed to the device via write_file when richer guidance is       */
/*  needed.                                                           */
/* ------------------------------------------------------------------ */

#define BUILTIN_LUA_RUN                                                      \
    "# Lua Run\n"                                                            \
    "\n"                                                                     \
    "Execute inline Lua 5.5 on the device via the lua_run_script tool. "     \
    "Use it for computations, string/table/UTF-8 work, JSON-like text "      \
    "splitting, or any logic easier to express in Lua than as MCP tool "     \
    "chains. Hardware control is also available when the gpio / delay "      \
    "modules are compiled in (see lua_gpio / lua_delay skills).\n"           \
    "\n"                                                                     \
    "## When to use\n"                                                       \
    "- Need a small calculation, string/table/UTF-8 transformation, or "     \
    "conditional logic that returns a single final value.\n"                 \
    "- Need to drive a GPIO pin or insert ms/us delays from a script.\n"     \
    "- Need to parse a short JSON-like text (already obtained via another "  \
    "tool) and pick out one field.\n"                                        \
    "\n"                                                                     \
    "Do NOT use it for: filesystem / network / camera / display control "    \
    "(not exposed); long loops > script timeout; sharing state between "     \
    "calls (every call gets a fresh lua_State).\n"                           \
    "\n"                                                                     \
    "## How to use\n"                                                        \
    "Call `lua_run_script` with:\n"                                          \
    "- `code` (string, required): plain Lua 5.5 source — pre-compiled "      \
    "bytecode is rejected.\n"                                                \
    "- `timeout_ms` (int, optional): wall-clock budget, default 3000, "      \
    "100..60000.\n"                                                          \
    "Anything you want returned MUST be print()'d. Multiple values in one "  \
    "print are tab-separated and the call ends with a newline (standard "    \
    "Lua print). Errors come back as `ERROR: <msg>` plus a traceback.\n"     \
    "\n"                                                                     \
    "## Sandbox\n"                                                           \
    "Available standard libraries: base (`print` / `tostring` / `tonumber` " \
    "/ `pairs` / `ipairs` / `pcall` / ...), `string`, `table`, `math`, "     \
    "`utf8`, `coroutine`.\n"                                                 \
    "`os` subset: only `os.time` and `os.date` — `os.execute`, "             \
    "`os.remove`, `os.rename`, `os.exit`, etc. are NOT available.\n"         \
    "Hardware modules (when compiled in): `gpio.*`, `delay.*` — see the "    \
    "dedicated skills for full reference.\n"                                 \
    "NOT available: `io`, `package` / `require`, `debug`, `dofile`, "        \
    "`loadfile`, and any filesystem / network / shell access.\n"             \
    "\n"                                                                     \
    "## Example\n"                                                           \
    "Compute the sum 1..100:\n"                                              \
    "```\n"                                                                  \
    "lua_run_script({code=\"local r=0 for i=1,100 do r=r+i end print(r)\"})\n" \
    "```\n"                                                                  \
    "Returns: `5050`.\n"                                                     \
    "\n"                                                                     \
    "## Tips\n"                                                              \
    "- Always `print()` your result — anything not printed is invisible.\n"  \
    "- For tables, serialize first (`table.concat`, `string.format`, "       \
    "manual JSON). `print(t)` only yields `table: 0x...`.\n"                 \
    "- Wrap risky code in `pcall` so the tool returns success and the "      \
    "model can decide what to do with the error.\n"

#define BUILTIN_LUA_GPIO                                                     \
    "# Lua GPIO\n"                                                           \
    "\n"                                                                     \
    "Read and write T5AI GPIO pins from inside a `lua_run_script` call — "   \
    "drive an LED, read a button, generate simple pulse sequences, etc. "    \
    "Combine with the `lua_delay` skill for timed sequences.\n"              \
    "\n"                                                                     \
    "## When to use\n"                                                       \
    "- Toggle an LED / relay / buzzer / motor-driver enable pin.\n"          \
    "- Read a digital input (button, IR sensor, hall sensor).\n"             \
    "- Run a short bit-bang sequence (e.g. HC-SR04 trigger pulse).\n"        \
    "\n"                                                                     \
    "Do NOT use it for: high-speed toggling (> ~1 kHz — Lua + tkl "          \
    "overhead is roughly 100 us per call); long blocking loops; PWM/SPI/"    \
    "I2S (use the dedicated peripheral tools).\n"                            \
    "\n"                                                                     \
    "## API (auto-loaded as the global `gpio` table)\n"                      \
    "- `gpio.set_direction(pin, mode)` — configure a pin. Optional: "        \
    "`set_level` / `get_level` re-init internally as needed.\n"              \
    "- `gpio.set_level(pin, level)` — drive HIGH (`1`) or LOW (`0`).\n"      \
    "- `gpio.get_level(pin)` -> `0` or `1` — read current input level.\n"    \
    "\n"                                                                     \
    "`pin`: integer in `0..55` (T5AI native P0..P55). Out-of-range raises "  \
    "`gpio: pin <n> out of range`.\n"                                        \
    "`mode` strings:\n"                                                      \
    "- `\"input\"`         — INPUT / PULLUP   (button, digital sensor)\n"    \
    "- `\"output\"`        — OUTPUT / PUSH_PULL (LED, MOSFET, relay enable)\n"\
    "- `\"input_output\"`  — alias of `output` (TuyaOpen has no INOUT)\n"   \
    "- `\"output_od\"`     — OUTPUT / OPEN_DRAIN (I2C / 1-Wire)\n"          \
    "- `\"input_output_od\"` — alias of `output_od`\n"                       \
    "- `\"disable\"`       — INPUT / FLOATING (release the pin)\n"           \
    "\n"                                                                     \
    "## Example\n"                                                           \
    "Light up the LED on P10:\n"                                             \
    "```\n"                                                                  \
    "lua_run_script({code=\"gpio.set_direction(10,'output') "                \
    "gpio.set_level(10,1) print('LED on')\"})\n"                             \
    "```\n"                                                                  \
    "Read a button on P12 (active-low with internal pullup):\n"              \
    "```\n"                                                                  \
    "lua_run_script({code=\"gpio.set_direction(12,'input') "                 \
    "print(gpio.get_level(12)==0 and 'pressed' or 'released')\"})\n"         \
    "```\n"                                                                  \
    "\n"                                                                     \
    "## Tips\n"                                                              \
    "- GPIO levels persist between `lua_run_script` calls — explicitly "     \
    "drive low (or call `set_direction(pin,'disable')`) to release.\n"       \
    "- Confirm board-specific LED / button pin numbers from USER.md or "     \
    "device docs before driving them.\n"                                     \
    "- Combine with `delay.delay_ms` for blink / debounce loops.\n"

#define BUILTIN_LUA_DELAY                                                    \
    "# Lua Delay\n"                                                          \
    "\n"                                                                     \
    "Block the current task for a fixed amount of time inside a "            \
    "`lua_run_script` call — typical use is pacing a GPIO pulse train, "     \
    "waiting for a peripheral to settle, or rate-limiting a sensor read "    \
    "loop.\n"                                                                \
    "\n"                                                                     \
    "## When to use\n"                                                       \
    "- Inter-step delays in a bit-bang / reset / I2C-bit-bang sequence.\n"   \
    "- Sample at a steady cadence inside a Lua loop (e.g. 100 ms x 10).\n"   \
    "- Wait for an external module (LCD reset, power-on enable) to be "      \
    "ready.\n"                                                               \
    "\n"                                                                     \
    "Do NOT use it for: sleeping longer than the script timeout (default "   \
    "3000 ms — bumps the script over its budget); replacing background "    \
    "scheduling (use cron_add); sub-microsecond timing (delay_us is best-"   \
    "effort, expect ~10 us jitter from interrupts/scheduling).\n"            \
    "\n"                                                                     \
    "## API (auto-loaded as the global `delay` table)\n"                     \
    "- `delay.delay_ms(ms)` — yield the task for `ms` milliseconds. Backed " \
    "by `tal_system_sleep`; lets other tasks run.\n"                         \
    "- `delay.delay_us(us)` — busy/blocking sleep for `us` microseconds. "   \
    "Backed by `tkl_system_sleep_us`. **Capped at 1_000_000 us**; longer "   \
    "values raise an error suggesting `delay_ms`.\n"                         \
    "\n"                                                                     \
    "Negative arguments are clamped to 0. Both delays count against the "    \
    "script's overall `timeout_ms`, so a 2500 ms `delay_ms` leaves only "    \
    "500 ms for everything else under the default 3000 ms budget — bump "    \
    "`timeout_ms` if needed.\n"                                              \
    "\n"                                                                     \
    "## Example\n"                                                           \
    "Blink P10 three times:\n"                                               \
    "```\n"                                                                  \
    "lua_run_script({code=\"gpio.set_direction(10,'output') "                \
    "for i=1,3 do gpio.set_level(10,1) delay.delay_ms(150) "                 \
    "gpio.set_level(10,0) delay.delay_ms(150) end print('done')\"})\n"       \
    "```\n"                                                                  \
    "Generate a ~10 us trigger pulse on P5 (HC-SR04 style):\n"               \
    "```\n"                                                                  \
    "lua_run_script({code=\"gpio.set_direction(5,'output') "                 \
    "gpio.set_level(5,1) delay.delay_us(10) gpio.set_level(5,0) "            \
    "print('triggered')\"})\n"                                               \
    "```\n"                                                                  \
    "\n"                                                                     \
    "## Tips\n"                                                              \
    "- Use `delay_ms` for any wait >= 1 ms — `delay_us` busy-waits and "     \
    "starves other tasks on the same core.\n"                                \
    "- For sleeps longer than the default 3000 ms script timeout, raise "    \
    "`timeout_ms` on `lua_run_script`.\n"                                    \
    "- Currently `delay_us` is implemented only on T5AI; ports to other "    \
    "boards need an equivalent `tkl_system_sleep_us`.\n"

/* ------------------------------------------------------------------ */

typedef struct {
    const char *filename;
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    {"weather",           BUILTIN_WEATHER           },
    {"daily-briefing",    BUILTIN_DAILY_BRIEFING    },
    {"skill-creator",     BUILTIN_SKILL_CREATOR     },
    {"lua_run",           BUILTIN_LUA_RUN           },
    {"lua_gpio",          BUILTIN_LUA_GPIO          },
    {"lua_delay",         BUILTIN_LUA_DELAY         },
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

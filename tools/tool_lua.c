/**
 * @file tool_lua.c
 * @brief MCP tool wrapper around the embedded Lua 5.5 runtime.
 *
 * Exposes one tool to the LLM:
 *   lua_run_script(code: string, timeout_ms: int = 3000) -> string
 *
 * The handler:
 *   1. Reads `code` (required) and `timeout_ms` (optional, default 3000)
 *      from the MCP property list.
 *   2. Allocates a CONFIG_LUA_OUTPUT_BUFFER_SIZE byte capture buffer
 *      (PSRAM via claw_malloc when ENABLE_EXT_RAM is set).
 *   3. Invokes lua_runtime_run_string(), which spins up a fresh
 *      sandboxed lua_State, runs the snippet under a count-based
 *      deadline, and captures print() output and any error.
 *   4. Returns the captured text via ai_mcp_return_value_set_str.
 *
 * Compile-time gating:
 *   - The MCP tool is only registered when CONFIG_ENABLE_LUA_TOOL is y.
 *   - When the symbol is not defined or is 0, tool_lua_register() is a
 *     no-op that returns OPRT_OK so callers don't need to ifdef.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tool_lua.h"

#include "tal_api.h"
#include "app_base_config.h"

#if defined(ENABLE_LUA_TOOL) && (ENABLE_LUA_TOOL == 1)

#include "ai_mcp_server.h"
#include "lua_runtime.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Tunables (mirror lua/Kconfig defaults so this file compiles even before
 * Kconfig has been regenerated)
 * --------------------------------------------------------------------------- */
#ifndef CONFIG_LUA_OUTPUT_BUFFER_SIZE
#define CONFIG_LUA_OUTPUT_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_LUA_DEFAULT_TIMEOUT_MS
#define CONFIG_LUA_DEFAULT_TIMEOUT_MS 3000
#endif

/* ---------------------------------------------------------------------------
 * Property extraction helpers (mirroring tool_files.c / tool_cron.c style)
 * --------------------------------------------------------------------------- */
static const char *__get_str_prop(const MCP_PROPERTY_LIST_T *properties,
                                  const char                *name)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_STRING) {
        return prop->default_val.str_val;
    }
    return NULL;
}

static bool __get_int_prop(const MCP_PROPERTY_LIST_T *properties,
                           const char                *name,
                           int                       *out)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_INTEGER) {
        *out = prop->default_val.int_val;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * MCP tool callback: lua_run_script
 * --------------------------------------------------------------------------- */
static OPERATE_RET __tool_lua_run(const MCP_PROPERTY_LIST_T *properties,
                                  MCP_RETURN_VALUE_T        *ret_val,
                                  void                      *user_data)
{
    (void)user_data;

    const char *code = __get_str_prop(properties, "code");
    if (!code || code[0] == '\0') {
        ai_mcp_return_value_set_str(ret_val, "Error: 'code' is required and must be a non-empty string");
        return OPRT_INVALID_PARM;
    }

    int timeout_ms = CONFIG_LUA_DEFAULT_TIMEOUT_MS;
    (void)__get_int_prop(properties, "timeout_ms", &timeout_ms);
    if (timeout_ms <= 0) {
        timeout_ms = CONFIG_LUA_DEFAULT_TIMEOUT_MS;
    }

    size_t buf_size = (size_t)CONFIG_LUA_OUTPUT_BUFFER_SIZE;
    char  *buf      = (char *)claw_malloc(buf_size);
    if (!buf) {
        ai_mcp_return_value_set_str(ret_val, "Error: out of memory");
        return OPRT_MALLOC_FAILED;
    }
    buf[0] = '\0';

    OPERATE_RET rt = lua_runtime_run_string(code, (uint32_t)timeout_ms, buf, buf_size);

    /* Always surface whatever the runtime captured back to the LLM, even
     * on failure — the buffer holds either the print() output, an
     * "ERROR: <msg>" trailer, or the "no output" notice. */
    ai_mcp_return_value_set_str(ret_val, buf);

    PR_DEBUG("lua_run_script: rt=%d code_len=%u timeout_ms=%d out_len=%u",
             rt, (unsigned)strlen(code), timeout_ms, (unsigned)strlen(buf));

    claw_free(buf);

    /* Return OPRT_OK so the MCP server forwards the captured text (including
     * any "ERROR: ..." traceback) to the LLM instead of discarding it as
     * a generic "Tool execution failed" JSON-RPC error. */
    (void)rt;
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
OPERATE_RET tool_lua_register(void)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "lua_run_script",
        "Execute an inline Lua 5.5 script on the device and return whatever it "
        "prints. Use this tool when you want to perform a small computation, "
        "data transformation, or piece of logic that is easier to express as "
        "Lua than as a chain of MCP tools. Also supports basic GPIO control "
        "and blocking delays when the corresponding hardware modules are "
        "compiled in (see 'Hardware modules' below).\n"
        "\n"
        "Sandbox:\n"
        "- Standard library subset: base, 'string', 'table', 'math', 'utf8', "
        "'coroutine'.\n"
        "- 'os' subset: only os.time() and os.date() are available. "
        "os.execute / os.remove / os.rename / os.exit / os.getenv etc. are "
        "NOT available.\n"
        "- 'io', 'package'/'require', 'debug', 'dofile', 'loadfile', and any "
        "filesystem / network / shell access are NOT available. Pre-compiled "
        "Lua bytecode is rejected (text source only).\n"
        "- 'print(...)' is captured: every value you pass to print becomes part "
        "of the tool's response (separated by tabs, terminated by newline).\n"
        "- Each call runs in a fresh interpreter; nothing carries between "
        "invocations (globals, modules, hooks all reset).\n"
        "\n"
        "Hardware modules (auto-loaded into globals when compiled in; "
        "see 'lua_gpio' / 'lua_delay' skills for full reference):\n"
        "- gpio.set_direction(pin, mode): mode is one of 'input', 'output', "
        "'input_output', 'output_od', 'input_output_od', 'disable'.\n"
        "- gpio.set_level(pin, level): drive HIGH (1) or LOW (0).\n"
        "- gpio.get_level(pin): returns 0 or 1.\n"
        "  pin range on T5AI: 0..55. set_level/get_level internally re-init "
        "the pin so an explicit set_direction is optional.\n"
        "- delay.delay_ms(ms): yields the task for ms milliseconds.\n"
        "- delay.delay_us(us): blocking sleep, capped at 1_000_000 us. Use "
        "delay_ms for longer waits.\n"
        "  Both delays count against the script's wall-clock timeout.\n"
        "\n"
        "Limits:\n"
        "- Wall-clock budget defaults to 3000 ms; override with 'timeout_ms'. "
        "Exceeding it raises a Lua error 'execution timed out'.\n"
        "- Captured output is truncated at the device's configured buffer size; "
        "expect '[output truncated]' on overflow.\n"
        "\n"
        "Return value:\n"
        "- On success: the captured print() output, or "
        "'Lua script completed with no output.' if the script printed nothing.\n"
        "- On error  : whatever was printed before the error, followed by "
        "'ERROR: <message>' and a stack traceback.\n"
        "\n"
        "Tip: write your final result with print() — anything not printed is "
        "invisible to the caller.",
        __tool_lua_run,
        NULL,
        MCP_PROP_STR("code",
                     "Lua 5.5 source code to execute. Must be plain text "
                     "(not pre-compiled bytecode)."),
        MCP_PROP_INT_DEF_RANGE("timeout_ms",
                               "Wall-clock execution budget in milliseconds. "
                               "Defaults to 3000.",
                               CONFIG_LUA_DEFAULT_TIMEOUT_MS, 100, 60000)
    ));

    PR_DEBUG("Lua MCP tool (lua_run_script) registered");
    return OPRT_OK;
}

#else /* !ENABLE_LUA_TOOL */

OPERATE_RET tool_lua_register(void)
{
    /* Feature disabled at compile time: registration is a no-op so callers
     * don't need conditional compilation in tools_register.c. */
    return OPRT_OK;
}

#endif /* ENABLE_LUA_TOOL */

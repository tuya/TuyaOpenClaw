/**
 * @file tools_register.c
 * @brief MCP tools registration for DuckyClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "tools_register.h"
#include "tool_files.h"
#include "tool_cron.h"
#include "tool_openclaw_ctrl.h"
#include "cron_service.h"
#include "heartbeat.h"
#include "memory_manager.h"
#include "session_manager.h"
#include "skill_loader.h"

#include "tal_api.h"

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
#include "tool_exec.h"
#endif

#if defined(ENABLE_HARDWARE_MCP) && (ENABLE_HARDWARE_MCP == 1)
#include "tool_hw.h"
#endif

#if defined(ENABLE_LUA_TOOL) && (ENABLE_LUA_TOOL == 1)
#include "tool_lua.h"
#endif

#if defined(ENABLE_LUA_MODULE_GPIO) && (ENABLE_LUA_MODULE_GPIO == 1)
#include "lua_module_gpio.h"
#endif

#if defined(ENABLE_LUA_MODULE_DELAY) && (ENABLE_LUA_MODULE_DELAY == 1)
#include "lua_module_delay.h"
#endif
/***********************************************************
***********************function define**********************
***********************************************************/

static OPERATE_RET __ai_mcp_init(void *data)
{
    OPERATE_RET rt = OPRT_OK;

    #if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
        rt = ai_mcp_server_init("DuckyClaw MCP Server", "1.0");
        if (rt != OPRT_OK) {
            return rt;
        }
    #endif

    /* Initialize filesystem (mount SD card if needed, create default files) */
    TUYA_CALL_ERR_LOG(tool_files_fs_init());

    /* Initialize and start cron service */
    TUYA_CALL_ERR_LOG(cron_service_init());
    TUYA_CALL_ERR_LOG(cron_service_start());

    /* Initialize and start heartbeat service */
    TUYA_CALL_ERR_LOG(heartbeat_init());
    TUYA_CALL_ERR_LOG(heartbeat_start());

    /* Initialize memory and session managers */
    TUYA_CALL_ERR_LOG(memory_manager_init());
    TUYA_CALL_ERR_LOG(session_manager_init());

    /* Initialize skill loader (creates dir + installs built-in skills) */
    TUYA_CALL_ERR_LOG(skill_loader_init());

    /* Register file operation tools */
    TUYA_CALL_ERR_LOG(tool_files_register());

    /* Register cron tools */
    TUYA_CALL_ERR_LOG(tool_cron_register());

    /* Register exec/system tools */
    #if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    TUYA_CALL_ERR_LOG(tool_exec_register());
    #endif

    /* Register OpenClaw/PC control tool */
    TUYA_CALL_ERR_LOG(tool_openclaw_ctrl_register());

    /* Register hardware peripheral tools */
    #if defined(ENABLE_HARDWARE_MCP) && (ENABLE_HARDWARE_MCP == 1)
    TUYA_CALL_ERR_LOG(tool_hw_register());
    #endif

    /* Register embedded Lua scripting tool */
    #if defined(ENABLE_LUA_TOOL) && (ENABLE_LUA_TOOL == 1)
    TUYA_CALL_ERR_LOG(tool_lua_register());
    #endif

    /* Register Lua hardware modules */
    #if defined(ENABLE_LUA_MODULE_GPIO) && (ENABLE_LUA_MODULE_GPIO == 1)
    lua_module_gpio_register();
    #endif

    #if defined(ENABLE_LUA_MODULE_DELAY) && (ENABLE_LUA_MODULE_DELAY == 1)
    lua_module_delay_register();
    #endif

    PR_DEBUG("MCP Server initialized successfully with tools");
    return rt;
}

/**
 * @brief Initialize and register all MCP tools
 *
 * This function is called during MCP server initialization to register
 * all custom tools for the DuckyClaw project.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_registry_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    tal_event_subscribe(EVENT_MQTT_CONNECTED, "ai_mcp_init", __ai_mcp_init, SUBSCRIBE_TYPE_ONETIME);

    PR_DEBUG("DuckyClaw MCP tools registered successfully");

    return rt;
}

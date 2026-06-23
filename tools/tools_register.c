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

    TUYA_CALL_ERR_LOG(tool_files_fs_init());

    TUYA_CALL_ERR_LOG(cron_service_init());
    TUYA_CALL_ERR_LOG(cron_service_start());

    TUYA_CALL_ERR_LOG(heartbeat_init());
    TUYA_CALL_ERR_LOG(heartbeat_start());

    TUYA_CALL_ERR_LOG(memory_manager_init());
    TUYA_CALL_ERR_LOG(session_manager_init());

    TUYA_CALL_ERR_LOG(skill_loader_init());

    TUYA_CALL_ERR_LOG(tool_files_register());
    TUYA_CALL_ERR_LOG(tool_cron_register());

    #if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    TUYA_CALL_ERR_LOG(tool_exec_register());
    #endif

    TUYA_CALL_ERR_LOG(tool_openclaw_ctrl_register());

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

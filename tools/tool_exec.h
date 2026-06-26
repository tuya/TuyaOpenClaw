/**
 * @file tool_exec.h
 * @brief MCP exec/system tools for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc.
 * All Rights Reserved.
 *
 */

#ifndef __TOOL_EXEC_H__
#define __TOOL_EXEC_H__

#include "tuya_cloud_types.h"
#include "ai_mcp_server.h"
#include "tuya_app_config.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register exec/system related MCP tools
 *
 * Registers tools like exec, get_system_info, list_devices, handle_command
 * (and best-effort aliases under pi.*).
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_exec_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOOL_EXEC_H__ */

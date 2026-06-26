/**
 * @file tool_cron.h
 * @brief MCP cron (scheduled task) tools for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __TOOL_CRON_H__
#define __TOOL_CRON_H__

#include "tuya_cloud_types.h"
#include "ai_mcp_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register all cron MCP tools
 *
 * Registers cron_add, cron_list, and cron_remove tools
 * with the MCP server.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_cron_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOOL_CRON_H__ */

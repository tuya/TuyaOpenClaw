/**
 * @file tool_files.h
 * @brief MCP file operation tools for TuyaOpenClaw
 * @version 0.2
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __TOOL_FILES_H__
#define __TOOL_FILES_H__

#include "app_base_config.h"
#include "ai_mcp_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Initialize filesystem
 *
 * Mounts SD card if CLAW_USE_SDCARD is enabled.
 * Creates default config directory and files if not present.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_files_fs_init(void);

/**
 * @brief Register all file operation MCP tools
 *
 * Registers read_file, write_file, edit_file, and list_dir tools
 * with the MCP server.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_files_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOOL_FILES_H__ */

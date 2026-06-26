/**
 * @file session_manager.h
 * @brief Multi-session conversation history manager for TuyaOpenClaw
 * @version 0.1
 * @date 2026-03-04
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Ported from mimiclaw/memory/session_mgr.
 * Manages per-chat_id JSONL conversation history files.
 */

#ifndef __SESSION_MANAGER_H__
#define __SESSION_MANAGER_H__

#include "tuya_cloud_types.h"
#include "app_base_config.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Session files directory */
#ifndef CLAW_SESSION_DIR
#define CLAW_SESSION_DIR CLAW_FS_ROOT_PATH "/sessions"
#endif

/* Maximum messages kept in ring buffer for history retrieval */
#ifndef CLAW_SESSION_MAX_MSGS
#define CLAW_SESSION_MAX_MSGS 20
#endif

/***********************************************************
***********************typedef define***********************
***********************************************************/

/**
 * @brief Callback for session_list enumeration
 * @param name      Session filename (e.g. "tg_12345.jsonl")
 * @param user_data User-provided context
 */
typedef void (*session_list_cb_t)(const char *name, void *user_data);

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Initialize session manager (create directory if needed)
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET session_manager_init(void);

/**
 * @brief Append a message to a session's JSONL file
 * @param[in] chat_id  Chat/session identifier
 * @param[in] role     Message role ("user" or "assistant")
 * @param[in] content  Message content
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET session_append(const char *chat_id, const char *role, const char *content);

/**
 * @brief Get recent conversation history as JSON array string
 * @param[in]  chat_id   Chat/session identifier
 * @param[out] buf       Output buffer for JSON array
 * @param[in]  size      Buffer size
 * @param[in]  max_msgs  Maximum number of recent messages to retrieve
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs);

/**
 * @brief Clear (delete) a specific session file
 * @param[in] chat_id  Chat/session identifier
 * @return OPERATE_RET OPRT_OK on success, OPRT_NOT_FOUND if not found
 */
OPERATE_RET session_clear(const char *chat_id);

/**
 * @brief Clear all session files
 * @param[out] out_removed  Number of files deleted (optional, can be NULL)
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET session_clear_all(uint32_t *out_removed);

/**
 * @brief List all sessions via callback
 * @param[in]  cb         Callback invoked for each session file
 * @param[in]  user_data  User-provided context passed to callback
 * @param[out] out_count  Number of sessions found (optional, can be NULL)
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET session_list(session_list_cb_t cb, void *user_data, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* __SESSION_MANAGER_H__ */

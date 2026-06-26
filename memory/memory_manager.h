/**
 * @file memory_manager.h
 * @brief Long-term memory and daily notes manager for TuyaOpenClaw
 * @version 0.1
 * @date 2026-03-04
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Ported from mimiclaw/memory/memory_store.
 * Provides persistent long-term memory (MEMORY.md) and daily notes (YYYY-MM-DD.md).
 */

#ifndef __MEMORY_MANAGER_H__
#define __MEMORY_MANAGER_H__

#include "tuya_cloud_types.h"
#include "app_base_config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Memory directory under filesystem root */
#ifndef CLAW_MEMORY_DIR
#define CLAW_MEMORY_DIR CLAW_FS_ROOT_PATH "/memory"
#endif

/* Long-term memory file */
#ifndef CLAW_MEMORY_FILE
#define CLAW_MEMORY_FILE CLAW_MEMORY_DIR "/MEMORY.md"
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Initialize memory store (create directory if needed)
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET memory_manager_init(void);

/**
 * @brief Read long-term memory (MEMORY.md full text)
 * @param[out] buf     Output buffer
 * @param[in]  size    Buffer size
 * @return OPERATE_RET OPRT_OK on success, OPRT_NOT_FOUND if file missing
 */
OPERATE_RET memory_read_long_term(char *buf, size_t size);

/**
 * @brief Overwrite long-term memory (called after AI summarization)
 * @param[in] content  New memory content
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET memory_write_long_term(const char *content);

/**
 * @brief Append a note to today's date file (YYYY-MM-DD.md)
 * @param[in] note  Note content to append
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET memory_append_today(const char *note);

/**
 * @brief Read recent N days of notes, separated by "---"
 * @param[out] buf   Output buffer
 * @param[in]  size  Buffer size
 * @param[in]  days  Number of days to look back
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET memory_read_recent(char *buf, size_t size, int days);

#ifdef __cplusplus
}
#endif

#endif /* __MEMORY_MANAGER_H__ */

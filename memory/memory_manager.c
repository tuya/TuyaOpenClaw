/**
 * @file memory_manager.c
 * @brief Long-term memory and daily notes manager for TuyaOpenClaw
 * @version 0.1
 * @date 2026-03-04
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Ported from mimiclaw/memory/memory_store.
 * Uses claw_* filesystem macros from tool_files.h for SD/Flash abstraction.
 */

#include "memory_manager.h"
#include "tool_files.h"

#include "tal_api.h"
#include "tal_time_service.h"

#include <string.h>
#include <stdio.h>

/***********************************************************
***********************function define**********************
***********************************************************/

/**
 * @brief Get date string (YYYY-MM-DD) for N days ago
 */
static void __get_date_str(char *buf, size_t size, int days_ago)
{
    if (!buf || size == 0) {
        return;
    }

    TIME_T now = tal_time_get_posix();
    if (days_ago > 0) {
        now -= (TIME_T)days_ago * 86400;
    }
    // TIME_T is unsigned int, so it cannot be negative
    // The check if(now < 0) is invalid because unsigned types can't be negative

    POSIX_TM_S tm_now = {0};
    if (tal_time_get_local_time_custom(now, &tm_now) != OPRT_OK) {
        if (!tal_time_gmtime_r(&now, &tm_now)) {
            // Check if buffer is large enough for the fallback date
            if (size >= sizeof("1970-01-01")) {
                snprintf(buf, size, "1970-01-01");
            } else {
                // If buffer is too small, just fill with safe string
                strncpy(buf, "1970", size - 1);
                buf[size - 1] = '\0';
            }
            return;
        }
    }

    // We need to ensure the buffer is large enough for our date format
    // The format is YYYY-MM-DD which requires 10 chars + 1 for null terminator = 11
    if (size < 11) {
        if (size >= 3) {
            strcpy(buf, "NA");  // Not enough space, return "NA"
        } else if (size == 2) {
            buf[0] = 'N';
            buf[1] = '\0';
        } else if (size == 1) {
            buf[0] = '\0';  // Just null terminate
        }
        return;
    }
    
    // Use a temporary larger buffer to format the date string safely
    char temp_buf[32];  // Large enough for any possible date format result
    int formatted_len = snprintf(temp_buf, sizeof(temp_buf), "%04d-%02d-%02d",
                                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

    // Now copy to the actual buffer, ensuring we don't exceed the size
    if (formatted_len >= 0 && (size_t)formatted_len < size) {
        strcpy(buf, temp_buf);
    } else {
        // Fallback to simple date if the formatted date doesn't fit
        if (size >= sizeof("1970-01-01")) {
            snprintf(buf, size, "1970-01-01");
        } else {
            // If buffer is too small, just fill with safe string
            if (size > 1) {
                buf[0] = 'N';
                buf[1] = '\0';
            } else if (size == 1) {
                buf[0] = '\0';
            }
        }
    }
}

OPERATE_RET memory_manager_init(void)
{
    /* Create memory directory */
    claw_fs_mkdir(CLAW_MEMORY_DIR);

    PR_INFO("Memory manager initialized: %s", CLAW_MEMORY_DIR);
    return OPRT_OK;
}

OPERATE_RET memory_read_long_term(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return OPRT_INVALID_PARM;
    }

    TUYA_FILE f = claw_fopen(CLAW_MEMORY_FILE, "r");
    if (!f) {
        buf[0] = '\0';
        return OPRT_NOT_FOUND;
    }

    int n = claw_fread(buf, (int)(size - 1), f);
    if (n < 0) {
        buf[0] = '\0';
        claw_fclose(f);
        return OPRT_COM_ERROR;
    }
    buf[n] = '\0';
    claw_fclose(f);

    PR_DEBUG("memory_read_long_term: %d bytes", n);
    return OPRT_OK;
}

OPERATE_RET memory_write_long_term(const char *content)
{
    if (!content) {
        return OPRT_INVALID_PARM;
    }

    TUYA_FILE f = claw_fopen(CLAW_MEMORY_FILE, "w");
    if (!f) {
        PR_ERR("Failed to open memory file for writing: %s", CLAW_MEMORY_FILE);
        return OPRT_FILE_OPEN_FAILED;
    }

    int len = (int)strlen(content);
    int n   = claw_fwrite((void *)content, len, f);
    claw_fclose(f);

    if (n < 0) {
        PR_ERR("Failed to write memory file");
        return OPRT_COM_ERROR;
    }

    PR_DEBUG("memory_write_long_term: %d bytes written", n);
    return OPRT_OK;
}

OPERATE_RET memory_append_today(const char *note)
{
    if (!note) {
        return OPRT_INVALID_PARM;
    }

    char date_str[16] = {0};
    char path[128]    = {0};
    __get_date_str(date_str, sizeof(date_str), 0);
    snprintf(path, sizeof(path), "%s/%s.md", CLAW_MEMORY_DIR, date_str);

    /* Try append first; if file doesn't exist, create with date header */
    TUYA_FILE f = claw_fopen(path, "a");
    if (!f) {
        f = claw_fopen(path, "w");
        if (!f) {
            PR_ERR("Failed to create daily note: %s", path);
            return OPRT_FILE_OPEN_FAILED;
        }
        char hdr[64] = {0};
        int  hn      = snprintf(hdr, sizeof(hdr), "# %s\n\n", date_str);
        if (hn > 0) {
            (void)claw_fwrite(hdr, hn, f);
        }
    }

    (void)claw_fwrite((void *)note, (int)strlen(note), f);
    (void)claw_fwrite("\n", 1, f);
    claw_fclose(f);

    PR_DEBUG("memory_append_today: %s -> %s", date_str, path);
    return OPRT_OK;
}

OPERATE_RET memory_read_recent(char *buf, size_t size, int days)
{
    if (!buf || size == 0 || days <= 0) {
        return OPRT_INVALID_PARM;
    }

    size_t off = 0;
    buf[0]     = '\0';

    for (int i = 0; i < days && off < size - 1; i++) {
        char date_str[16] = {0};
        char path[128]    = {0};

        __get_date_str(date_str, sizeof(date_str), i);
        snprintf(path, sizeof(path), "%s/%s.md", CLAW_MEMORY_DIR, date_str);

        TUYA_FILE f = claw_fopen(path, "r");
        if (!f) {
            continue;
        }

        /* Add separator between days */
        if (off > 0 && off < size - 8) {
            off += (size_t)snprintf(buf + off, size - off, "\n---\n");
        }

        int n = claw_fread(buf + off, (int)(size - off - 1), f);
        if (n > 0) {
            off += (size_t)n;
        }
        buf[off] = '\0';
        claw_fclose(f);
    }

    PR_DEBUG("memory_read_recent: %d days, %zu bytes", days, off);
    return OPRT_OK;
}
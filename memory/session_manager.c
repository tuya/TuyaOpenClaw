/**
 * @file session_manager.c
 * @brief Multi-session conversation history manager for TuyaOpenClaw
 * @version 0.1
 * @date 2026-03-04
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Ported from mimiclaw/memory/session_mgr.
 * Uses claw_* filesystem macros from tool_files.h for SD/Flash abstraction.
 * Each session is stored as a JSONL file (one JSON object per line).
 */

#include "session_manager.h"
#include "tool_files.h"

#include "tal_api.h"
#include "tal_time_service.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/***********************************************************
***********************function define**********************
***********************************************************/
static const char *__json_get_string(const cJSON *item)
{
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }

    return item->valuestring;
}

/**
 * @brief Build session file path from chat_id
 */
static void __session_path(const char *chat_id, char *buf, size_t size)
{
    snprintf(buf, size, "%s/tg_%s.jsonl", CLAW_SESSION_DIR, chat_id);
}

/**
 * @brief Normalize chat_id by stripping "tg_" prefix and ".jsonl" suffix
 */
static void __session_normalize_chat_id(const char *chat_id, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!chat_id || chat_id[0] == '\0') {
        return;
    }

    const char *start = chat_id;
    if (strncmp(chat_id, "tg_", 3) == 0) {
        start = chat_id + 3;
    }

    const char *suffix = strstr(start, ".jsonl");
    size_t      n      = suffix ? (size_t)(suffix - start) : strlen(start);
    if (n >= out_size) {
        n = out_size - 1;
    }
    if (n > 0) {
        memcpy(out, start, n);
    }
    out[n] = '\0';
}

OPERATE_RET session_manager_init(void)
{
    /* Create sessions directory */
    claw_fs_mkdir(CLAW_SESSION_DIR);

    PR_INFO("Session manager initialized: %s", CLAW_SESSION_DIR);
    return OPRT_OK;
}

OPERATE_RET session_append(const char *chat_id, const char *role, const char *content)
{
    TUYA_CHECK_NULL_RETURN(chat_id, OPRT_INVALID_PARM);
    TUYA_CHECK_NULL_RETURN(role, OPRT_INVALID_PARM);
    TUYA_CHECK_NULL_RETURN(content, OPRT_INVALID_PARM);

    char path[128] = {0};
    __session_path(chat_id, path, sizeof(path));

    TUYA_FILE f = claw_fopen(path, "a");
    if (!f) {
        PR_ERR("Failed to open session file: %s", path);
        return OPRT_FILE_OPEN_FAILED;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        claw_fclose(f);
        return OPRT_CR_CJSON_ERR;
    }

    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)tal_time_get_posix());

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (!line) {
        claw_fclose(f);
        return OPRT_CR_CJSON_ERR;
    }

    (void)claw_fwrite(line, (int)strlen(line), f);
    (void)claw_fwrite("\n", 1, f);
    cJSON_free(line);
    claw_fclose(f);

    PR_DEBUG("session_append: chat_id=%s role=%s", chat_id, role);
    return OPRT_OK;
}

OPERATE_RET session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    TUYA_CHECK_NULL_RETURN(chat_id, OPRT_INVALID_PARM);
    TUYA_CHECK_NULL_RETURN(buf, OPRT_INVALID_PARM);
    if (size == 0 || max_msgs <= 0) {
        return OPRT_INVALID_PARM;
    }
    if (max_msgs > CLAW_SESSION_MAX_MSGS) {
        max_msgs = CLAW_SESSION_MAX_MSGS;
    }

    char path[128] = {0};
    __session_path(chat_id, path, sizeof(path));

    TUYA_FILE f = claw_fopen(path, "r");
    if (!f) {
        snprintf(buf, size, "[]");
        return OPRT_OK;
    }

    /* Ring buffer to keep only the most recent max_msgs entries */
    cJSON *ring[CLAW_SESSION_MAX_MSGS] = {0};
    int    count                       = 0;
    int    write_idx                   = 0;

    char *line = (char *)claw_malloc(2048);
    if (!line) {
        claw_fclose(f);
        return OPRT_MALLOC_FAILED;
    }
    memset(line, 0, 2048);
    while (claw_fgets(line, 2048, f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }

        cJSON *obj = cJSON_Parse(line);
        if (!obj) {
            continue;
        }

        /* Overwrite oldest entry in ring buffer */
        if (count >= max_msgs && ring[write_idx]) {
            cJSON_Delete(ring[write_idx]);
        }
        ring[write_idx] = obj;
        write_idx       = (write_idx + 1) % max_msgs;
        if (count < max_msgs) {
            count++;
        }
    }
    claw_fclose(f);
    claw_free(line);

    /* Build output JSON array from ring buffer */
    cJSON *arr   = cJSON_CreateArray();
    int    start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int    idx     = (start + i) % max_msgs;
        cJSON *src     = ring[idx];
        cJSON *entry   = cJSON_CreateObject();
        cJSON *j_role    = cJSON_GetObjectItem(src, "role");
        cJSON *j_content = cJSON_GetObjectItem(src, "content");

        cJSON_AddStringToObject(entry, "role",
                                __json_get_string(j_role) ? __json_get_string(j_role) : "user");
        cJSON_AddStringToObject(entry, "content",
                                __json_get_string(j_content) ? __json_get_string(j_content) : "");
        cJSON_AddItemToArray(arr, entry);
    }

    /* Free ring buffer entries */
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON_Delete(ring[idx]);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (!json_str) {
        snprintf(buf, size, "[]");
        return OPRT_CR_CJSON_ERR;
    }

    strncpy(buf, json_str, size - 1);
    buf[size - 1] = '\0';
    cJSON_free(json_str);

    PR_DEBUG("session_get_history: chat_id=%s count=%d", chat_id, count);
    return OPRT_OK;
}

OPERATE_RET session_clear(const char *chat_id)
{
    TUYA_CHECK_NULL_RETURN(chat_id, OPRT_INVALID_PARM);
    if (chat_id[0] == '\0') {
        return OPRT_INVALID_PARM;
    }
    /* Path traversal protection */
    if (strchr(chat_id, '/') || strchr(chat_id, '\\')) {
        PR_WARN("session_clear: rejected chat_id with path separator: %s", chat_id);
        return OPRT_INVALID_PARM;
    }

    char normalized[96] = {0};
    __session_normalize_chat_id(chat_id, normalized, sizeof(normalized));
    if (normalized[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    char path[128] = {0};
    __session_path(normalized, path, sizeof(path));

    OPERATE_RET rt = claw_fs_remove(path);
    if (rt == OPRT_OK) {
        PR_INFO("session_clear: removed %s", path);
    } else {
        PR_WARN("session_clear: file not found %s", path);
    }
    return (rt == OPRT_OK) ? OPRT_OK : OPRT_NOT_FOUND;
}

OPERATE_RET session_clear_all(uint32_t *out_removed)
{
    uint32_t removed = 0;
    if (out_removed) {
        *out_removed = 0;
    }

    TUYA_DIR dir = NULL;
    if (claw_dir_open(CLAW_SESSION_DIR, &dir) != OPRT_OK || !dir) {
        PR_WARN("session_clear_all: cannot open dir %s", CLAW_SESSION_DIR);
        return OPRT_FILE_OPEN_FAILED;
    }

    while (1) {
        TUYA_FILEINFO info = NULL;
        if (claw_dir_read(dir, &info) != OPRT_OK || !info) {
            break;
        }

        const char *name = NULL;
        if (claw_dir_name(info, &name) != OPRT_OK || !name || name[0] == '\0') {
            continue;
        }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        BOOL_T is_regular = FALSE;
        if (claw_dir_is_regular(info, &is_regular) != OPRT_OK || !is_regular) {
            continue;
        }

        char path[160] = {0};
        snprintf(path, sizeof(path), "%s/%s", CLAW_SESSION_DIR, name);
        if (claw_fs_remove(path) == OPRT_OK) {
            removed++;
        }
    }

    claw_dir_close(dir);
    if (out_removed) {
        *out_removed = removed;
    }

    PR_INFO("session_clear_all: removed %u files", removed);
    return (removed > 0) ? OPRT_OK : OPRT_NOT_FOUND;
}

OPERATE_RET session_list(session_list_cb_t cb, void *user_data, uint32_t *out_count)
{
    uint32_t count = 0;
    if (out_count) {
        *out_count = 0;
    }

    TUYA_DIR dir = NULL;
    if (claw_dir_open(CLAW_SESSION_DIR, &dir) != OPRT_OK || !dir) {
        PR_WARN("session_list: cannot open dir %s", CLAW_SESSION_DIR);
        return OPRT_FILE_OPEN_FAILED;
    }

    while (1) {
        TUYA_FILEINFO info = NULL;
        if (claw_dir_read(dir, &info) != OPRT_OK || !info) {
            break;
        }

        const char *name = NULL;
        if (claw_dir_name(info, &name) == OPRT_OK && name && name[0] != '\0') {
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }

            count++;
            if (cb) {
                cb(name, user_data);
            }
        }
    }

    claw_dir_close(dir);
    if (out_count) {
        *out_count = count;
    }

    PR_DEBUG("session_list: found %u sessions", count);
    return (count > 0) ? OPRT_OK : OPRT_NOT_FOUND;
}

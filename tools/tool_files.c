/**
 * @file tool_files.c
 * @brief MCP file operation tools for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Implements read_file, write_file, edit_file, and list_dir MCP tools
 * using the ai_mcp_server.h interface.
 */

#include "tool_files.h"

#include "tal_api.h"
#include "cJSON.h"
#include "tkl_fs.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/***********************************************************
************************macro define************************
***********************************************************/

#define MAX_FILE_SIZE  (32 * 1024)

/***********************************************************
***********************variable define**********************
***********************************************************/


/***********************************************************
***********************function define**********************
***********************************************************/

/**
 * @brief Validate file path security
 *
 * Only allows paths under CLAW_FS_ROOT_PATH and rejects path traversal attempts.
 *
 * @param path File path to validate
 * @return true if path is valid, false otherwise
 */
static bool __validate_path(const char *path)
{
    if (!path || path[0] == '\0') {
        return false;
    }
#if CLAW_FS_ROOT_PATH_EMPTY
    /* Linux platform: root path is empty, allow any absolute path */
    if (path[0] != '/') {
        return false;
    }
#else
    /* Other platforms: must start with CLAW_FS_ROOT_PATH */
    size_t root_len = strlen(CLAW_FS_ROOT_PATH);
    if (root_len == 0) {
        /* Empty root path case - should not happen when CLAW_FS_ROOT_PATH_EMPTY is 0 */
        if (path[0] != '/') {
            return false;
        }
    } else {
        if (strncmp(path, CLAW_FS_ROOT_PATH "/", root_len + 1) != 0) {
            return false;
        }
    }
#endif
    /* Check for path traversal attempts: "/../", "/..", "../", ".." */
    if (strstr(path, "/../") != NULL ||
        strstr(path, "/..") != NULL ||
        strncmp(path, "../", 3) == 0 ||
        strcmp(path, "..") == 0) {
        return false;
    }
    return true;
}

/**
 * @brief Helper to extract a string property from MCP property list
 *
 * @param properties MCP property list
 * @param name Property name to find
 * @return const char* Property string value, or NULL if not found
 */
static const char *__get_str_property(const MCP_PROPERTY_LIST_T *properties, const char *name)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_STRING) {
        return prop->default_val.str_val;
    }
    return NULL;
}

/**
 * @brief MCP tool callback: read_file
 *
 * Reads a file from the filesystem and returns its content as a string.
 *
 * Properties:
 * - path (string, required): The file path to read (must start with CLAW_FS_ROOT_PATH)
 *
 * @return OPERATE_RET OPRT_OK on success
 */
static OPERATE_RET __tool_read_file(const MCP_PROPERTY_LIST_T *properties,
                                    MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *path = __get_str_property(properties, "path");
    if (!__validate_path(path)) {
#if CLAW_FS_ROOT_PATH_EMPTY
        ai_mcp_return_value_set_str(ret_val, "Error: invalid path, must be an absolute path starting with /");
#else
        ai_mcp_return_value_set_str(ret_val, "Error: invalid path, must start with " CLAW_FS_ROOT_PATH);
#endif
        return OPRT_INVALID_PARM;
    }

    TUYA_FILE f = claw_fopen(path, "r");
    if (!f) {
        ai_mcp_return_value_set_str(ret_val, "Error: file not found");
        return OPRT_NOT_FOUND;
    }

    size_t max_read = MAX_FILE_SIZE;
    char *buf = (char *)claw_malloc(max_read + 1);
    if (!buf) {
        claw_fclose(f);
        return OPRT_MALLOC_FAILED;
    }

    int n = claw_fread(buf, (int)max_read, f);
    claw_fclose(f);

    if (n < 0) {
        claw_free(buf);
        ai_mcp_return_value_set_str(ret_val, "Error: read failed");
        return OPRT_COM_ERROR;
    }
    buf[n] = '\0';

    ai_mcp_return_value_set_str(ret_val, buf);
    claw_free(buf);

    PR_DEBUG("read_file path=%s bytes=%u", path, (unsigned)n);
    return OPRT_OK;
}

/**
 * @brief MCP tool callback: write_file
 *
 * Writes content to a file in the filesystem.
 *
 * Properties:
 * - path (string, required): The file path to write (must start with CLAW_FS_ROOT_PATH)
 * - content (string, required): The content to write
 *
 * @return OPERATE_RET OPRT_OK on success
 */
static OPERATE_RET __tool_write_file(const MCP_PROPERTY_LIST_T *properties,
                                     MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *path    = __get_str_property(properties, "path");
    const char *content = __get_str_property(properties, "content");

    if (!__validate_path(path)) {
#if CLAW_FS_ROOT_PATH_EMPTY
        ai_mcp_return_value_set_str(ret_val, "Error: invalid path, must be an absolute path starting with /");
#else
        ai_mcp_return_value_set_str(ret_val, "Error: invalid path, must start with " CLAW_FS_ROOT_PATH);
#endif
        return OPRT_INVALID_PARM;
    }

    if (!content) {
        ai_mcp_return_value_set_str(ret_val, "Error: content is required");
        return OPRT_INVALID_PARM;
    }

    TUYA_FILE f = claw_fopen(path, "w");
    if (!f) {
        ai_mcp_return_value_set_str(ret_val, "Error: open file failed");
        return OPRT_COM_ERROR;
    }

    int wn = claw_fwrite((void *)content, (int)strlen(content), f);
    claw_fclose(f);

    if (wn < 0) {
        ai_mcp_return_value_set_str(ret_val, "Error: write failed");
        return OPRT_COM_ERROR;
    }

    char msg[64];
    int ret = snprintf(msg, sizeof(msg), "OK: wrote %u bytes", (unsigned)strlen(content));
    if (ret < 0 || (size_t)ret >= sizeof(msg)) {
        ai_mcp_return_value_set_str(ret_val, "OK: write done");
    } else {
        ai_mcp_return_value_set_str(ret_val, msg);
    }
    ai_mcp_return_value_set_str(ret_val, msg);

    PR_DEBUG("write_file path=%s bytes=%u", path, (unsigned)strlen(content));
    return OPRT_OK;
}

/**
 * @brief MCP tool callback: edit_file
 *
 * Replaces the first occurrence of old_string with new_string in a file.
 *
 * Properties:
 * - path (string, required): The file path to edit (must start with CLAW_FS_ROOT_PATH)
 * - old_string (string, required): The string to find and replace
 * - new_string (string, required): The replacement string
 *
 * @return OPERATE_RET OPRT_OK on success
 */
static OPERATE_RET __tool_edit_file(const MCP_PROPERTY_LIST_T *properties,
                                    MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *path  = __get_str_property(properties, "path");
    const char *old_s = __get_str_property(properties, "old_string");
    const char *new_s = __get_str_property(properties, "new_string");

    if (!__validate_path(path)) {
#if CLAW_FS_ROOT_PATH_EMPTY
        ai_mcp_return_value_set_str(ret_val, "Error: invalid path, must be an absolute path starting with /");
#else
        ai_mcp_return_value_set_str(ret_val, "Error: invalid path, must start with " CLAW_FS_ROOT_PATH);
#endif
        return OPRT_INVALID_PARM;
    }

    if (!old_s || !new_s) {
        ai_mcp_return_value_set_str(ret_val, "Error: old_string and new_string are required");
        return OPRT_INVALID_PARM;
    }

    /* Read the original file */
    TUYA_FILE f = claw_fopen(path, "r");
    if (!f) {
        ai_mcp_return_value_set_str(ret_val, "Error: file not found");
        return OPRT_NOT_FOUND;
    }

    char *buf = (char *)claw_malloc(MAX_FILE_SIZE + 1);
    if (!buf) {
        claw_fclose(f);
        return OPRT_MALLOC_FAILED;
    }

    int n = claw_fread(buf, MAX_FILE_SIZE, f);
    claw_fclose(f);

    if (n < 0) {
        claw_free(buf);
        ai_mcp_return_value_set_str(ret_val, "Error: read failed");
        return OPRT_COM_ERROR;
    }
    buf[n] = '\0';

    /* Find old_string */
    char *pos = strstr(buf, old_s);
    if (!pos) {
        claw_free(buf);
        ai_mcp_return_value_set_str(ret_val, "Error: old_string not found");
        return OPRT_NOT_FOUND;
    }

    /* Build new content */
    size_t prefix_len = (size_t)(pos - buf);
    size_t suffix_off = prefix_len + strlen(old_s);
    size_t new_s_len = strlen(new_s);
    size_t suffix_len = strlen(buf + suffix_off);

    /* Check for size_t overflow before addition */
    if (prefix_len > SIZE_MAX - new_s_len - suffix_len - 1) {
        claw_free(buf);
        ai_mcp_return_value_set_str(ret_val, "Error: file content too large");
        return OPRT_INVALID_PARM;
    }

    size_t need = prefix_len + new_s_len + suffix_len + 1;

    char *new_buf = (char *)claw_malloc(need);
    if (!new_buf) {
        claw_free(buf);
        return OPRT_MALLOC_FAILED;
    }

    memcpy(new_buf, buf, prefix_len);
    memcpy(new_buf + prefix_len, new_s, strlen(new_s));
    strcpy(new_buf + prefix_len + strlen(new_s), buf + suffix_off);

    /* Write back */
    f = claw_fopen(path, "w");
    if (!f) {
        claw_free(new_buf);
        claw_free(buf);
        ai_mcp_return_value_set_str(ret_val, "Error: open file for writing failed");
        return OPRT_COM_ERROR;
    }

    (void)claw_fwrite(new_buf, (int)strlen(new_buf), f);
    claw_fclose(f);

    ai_mcp_return_value_set_str(ret_val, "OK: edit done");

    claw_free(new_buf);
    claw_free(buf);

    PR_DEBUG("edit_file path=%s", path);
    return OPRT_OK;
}

/**
 * @brief MCP tool callback: list_dir
 *
 * Lists files in a directory under the filesystem root.
 *
 * Properties:
 * - prefix (string, optional): Directory path to list (defaults to CLAW_FS_ROOT_PATH)
 *
 * @return OPERATE_RET OPRT_OK on success
 */
static OPERATE_RET __tool_list_dir(const MCP_PROPERTY_LIST_T *properties,
                                   MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *prefix = __get_str_property(properties, "prefix");
    if (!prefix || !__validate_path(prefix)) {
        prefix = CLAW_FS_ROOT_PATH "/";
    }

    TUYA_DIR dir = NULL;
    if (claw_dir_open(prefix, &dir) != OPRT_OK || !dir) {
        ai_mcp_return_value_set_str(ret_val, "Error: open dir failed");
        return OPRT_COM_ERROR;
    }

    /* Build directory listing string */
    size_t buf_size = 4096;
    char *buf = (char *)claw_malloc(buf_size);
    if (!buf) {
        claw_dir_close(dir);
        return OPRT_MALLOC_FAILED;
    }

    size_t off = 0;
    int ret = snprintf(buf + off, buf_size - off, "Dir: %s\n", prefix);
    if (ret < 0 || (size_t)ret >= buf_size - off) {
        claw_dir_close(dir);
        claw_free(buf);
        ai_mcp_return_value_set_str(ret_val, "Error: directory path too long");
        return OPRT_INVALID_PARM;
    }
    off += (size_t)ret;

    while (off < buf_size - 2) {
        TUYA_FILEINFO info = NULL;
        if (claw_dir_read(dir, &info) != OPRT_OK || !info) {
            break;
        }

        const char *name = NULL;
        if (claw_dir_name(info, &name) != OPRT_OK || !name) {
            continue;
        }

        off += snprintf(buf + off, buf_size - off, "- %s\n", name);
    }

    claw_dir_close(dir);

    ai_mcp_return_value_set_str(ret_val, buf);
    claw_free(buf);

    PR_DEBUG("list_dir prefix=%s", prefix);
    return OPRT_OK;
}

/**
 * __str_to_lower - Copy src into dst, converting ASCII to lowercase.
 * @dst:      Output buffer.
 * @dst_size: Size of dst including NUL.
 * @src:      Input string.
 */
static void __str_to_lower(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

/**
 * __find_in_dir - List all entries in dir_path, record matches, and enqueue
 *                subdirectories for further scanning.
 *
 * @dir_path:  Absolute path to the directory to scan (must end without '/').
 * @query_lo:  Lowercase search keyword.
 * @buf:       Output accumulation buffer.
 * @buf_size:  Size of buf.
 * @offset:    Current write position in buf (updated in-place).
 * @found:     Match counter (updated in-place).
 * @queue:     Array of char pointers used as a BFS directory queue.
 * @q_head:    BFS queue head index.
 * @q_tail:    BFS queue tail index (updated in-place).
 * @max_queue: Maximum number of entries in queue.
 */
static void __find_in_dir(const char *dir_path, const char *query_lo,
                           char *buf, size_t buf_size, size_t *offset, int *found,
                           char **queue, int q_head, int *q_tail, int max_queue)
{
    TUYA_DIR dir = NULL;
    if (claw_dir_open(dir_path, &dir) != OPRT_OK || !dir) {
        return;
    }

    char *full_path = (char *)claw_malloc(256);
    if (!full_path) {
        claw_dir_close(dir);
        return;
    }

    for (;;) {
        TUYA_FILEINFO info = NULL;
        if (claw_dir_read(dir, &info) != OPRT_OK || !info) {
            break;
        }

        const char *entry_name = NULL;
        if (claw_dir_name(info, &entry_name) != OPRT_OK || !entry_name) {
            continue;
        }

        /* Skip . and .. */
        if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0) {
            continue;
        }

        /* Build full path */
        size_t dp_len = strlen(dir_path);
        if (dp_len + 1 + strlen(entry_name) >= 256) {
            continue;
        }
        snprintf(full_path, 256, "%s/%s", dir_path, entry_name);

        /* Case-insensitive substring match in either direction */
        char name_lo[128];
        __str_to_lower(name_lo, sizeof(name_lo), entry_name);

        if (strstr(name_lo, query_lo) || strstr(query_lo, name_lo)) {
            if (*offset < buf_size - 4) {
                *offset += snprintf(buf + *offset, buf_size - *offset,
                                    "- %s\n", full_path);
                (*found)++;
            }
        }

        /* Try to open as directory; if it succeeds, enqueue for BFS */
        if (*q_tail < max_queue) {
            TUYA_DIR subdir = NULL;
            if (claw_dir_open(full_path, &subdir) == OPRT_OK && subdir) {
                claw_dir_close(subdir);
                char *qpath = claw_malloc(strlen(full_path) + 1);
                if (qpath) {
                    strcpy(qpath, full_path);
                    queue[(*q_tail)++] = qpath;
                }
            }
        }
    }

    claw_free(full_path);
    claw_dir_close(dir);
}

/**
 * @brief MCP tool callback: find_path
 *
 * Recursively searches the device filesystem for files or directories whose
 * names contain the given keyword (case-insensitive, substring match).
 * Searches start from CLAW_FS_ROOT_PATH up to a fixed BFS depth so that
 * the correct path can be discovered even when the caller provides a
 * misspelled name or an entirely wrong directory prefix.
 *
 * Properties:
 * - name (string, required): Filename keyword to search for
 *
 * @return OPERATE_RET OPRT_OK on success
 */
static OPERATE_RET __tool_find_path(const MCP_PROPERTY_LIST_T *properties,
                                    MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
#define FIND_PATH_BUF_SIZE  2048
#define FIND_PATH_MAX_QUEUE 24

    const char *name_query = __get_str_property(properties, "name");
    if (!name_query || strlen(name_query) == 0) {
        ai_mcp_return_value_set_str(ret_val, "Error: name parameter is required");
        return OPRT_INVALID_PARM;
    }

    char query_lo[128];
    __str_to_lower(query_lo, sizeof(query_lo), name_query);

    char *result = (char *)claw_malloc(FIND_PATH_BUF_SIZE);
    if (!result) {
        return OPRT_MALLOC_FAILED;
    }

    size_t offset = 0;
    offset += snprintf(result + offset, FIND_PATH_BUF_SIZE - offset,
                       "Search results for '%s' under " CLAW_FS_ROOT_PATH ":\n", name_query);

    /* BFS queue */
    char *queue[FIND_PATH_MAX_QUEUE] = {0};
    int q_head = 0, q_tail = 0;
    int found  = 0;

    /* Seed the queue with the filesystem root */
    char *root_copy = claw_malloc(strlen(CLAW_FS_ROOT_PATH) + 1);
    if (!root_copy) {
        claw_free(result);
        return OPRT_MALLOC_FAILED;
    }
    strcpy(root_copy, CLAW_FS_ROOT_PATH);
    queue[q_tail++] = root_copy;

    while (q_head < q_tail) {
        char *dir = queue[q_head++];
        __find_in_dir(dir, query_lo,
                      result, FIND_PATH_BUF_SIZE, &offset, &found,
                      queue, q_head, &q_tail, FIND_PATH_MAX_QUEUE);
        claw_free(dir);
    }

    /* Free any queue entries that were never dequeued (queue overflow guard) */
    while (q_head < q_tail) {
        claw_free(queue[q_head++]);
    }

    if (found == 0) {
        offset += snprintf(result + offset, FIND_PATH_BUF_SIZE - offset,
                           "No matches found for '%s'.\n"
                           "Tip: use list_dir to browse directories.\n",
                           name_query);
    } else {
        offset += snprintf(result + offset, FIND_PATH_BUF_SIZE - offset,
                           "Found %d match(es).\n", found);
    }

    ai_mcp_return_value_set_str(ret_val, result);
    claw_free(result);

    PR_DEBUG("find_path name=%s found=%d", name_query, found);
    return OPRT_OK;

#undef FIND_PATH_BUF_SIZE
#undef FIND_PATH_MAX_QUEUE
}

/**
 * @brief Create a file with default content if it does not exist
 *
 * @param path File path to create
 * @param default_content Default content to write
 * @return OPERATE_RET OPRT_OK on success
 */
static OPERATE_RET __create_default_file(const char *path, const char *default_content)
{
    /* Check if file already exists by trying to open for read */
    TUYA_FILE f = claw_fopen(path, "r");
    if (f) {
        claw_fclose(f);
        PR_DEBUG("File already exists: %s", path);
        return OPRT_OK;
    }

    /* Create file with default content */
    f = claw_fopen(path, "w");
    if (!f) {
        PR_ERR("Failed to create file: %s", path);
        return OPRT_COM_ERROR;
    }

    if (default_content && strlen(default_content) > 0) {
        claw_fwrite((void *)default_content, (int)strlen(default_content), f);
    }

    claw_fclose(f);
    PR_DEBUG("Created default file: %s", path);
    return OPRT_OK;
}

/**
 * @brief Initialize filesystem
 *
 * Mounts SD card if CLAW_USE_SDCARD is enabled.
 * Creates default config directory and files if not present.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_files_fs_init(void)
{
    OPERATE_RET rt = OPRT_OK;

#if (CLAW_USE_SDCARD == 1)
    /* Mount SD card filesystem */
    rt = claw_fs_mount(CLAW_FS_MOUNT_PATH, DEV_SDCARD);
    if (rt != OPRT_OK) {
        PR_ERR("Mount SD card failed: %d, retrying...", rt);
        /* Retry mount */
        for (int i = 0; i < 3; i++) {
            tal_system_sleep(1000);
            rt = claw_fs_mount(CLAW_FS_MOUNT_PATH, DEV_SDCARD);
            if (rt == OPRT_OK) {
                break;
            }
            PR_ERR("Mount SD card retry %d failed: %d", i + 1, rt);
        }
        if (rt != OPRT_OK) {
            PR_ERR("Mount SD card failed after retries");
            return rt;
        }
    }
    PR_DEBUG("SD card mounted at %s", CLAW_FS_MOUNT_PATH);
#endif

    /* Create config directory */
    claw_fs_mkdir(CLAW_CONFIG_DIR);

    /* Create default config files */
    TUYA_CALL_ERR_LOG(
        __create_default_file(USER_FILE,
                              "# User Config\n\n"
                              "(No user profile configured yet. Learn about the user through conversations\n"
                              "and update this file using edit_file to personalize future interactions.)\n"));

    TUYA_CALL_ERR_LOG(
        __create_default_file(SOUL_FILE,
                              "# Soul Config\n\n"
                              "You are a warm, helpful AI assistant. You speak naturally and concisely.\n"
                              "When delivering reminders, use a gentle and friendly tone.\n"
                              "Adapt your communication style to match the user's language preference.\n"));

    PR_DEBUG("Filesystem initialized, root: %s", CLAW_FS_ROOT_PATH);
    return OPRT_OK;
}

/**
 * @brief Register all file operation MCP tools
 *
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET tool_files_register(void)
{
    OPERATE_RET rt = OPRT_OK;

    /* read_file tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "read_file",
        "Read a file from the device filesystem.\n"
        "Parameters:\n"
        "- path (string): The file path to read, must start with " CLAW_FS_ROOT_PATH ".\n"
        "Response:\n"
        "- Returns the file content as a string.",
        __tool_read_file,
        NULL,
        MCP_PROP_STR("path", "The file path to read, must start with " CLAW_FS_ROOT_PATH)
    ));

    /* write_file tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "write_file",
        "Write content to a file on the device filesystem.\n"
        "Parameters:\n"
        "- path (string): The file path to write, must start with " CLAW_FS_ROOT_PATH ".\n"
        "- content (string): The content to write to the file.\n"
        "Response:\n"
        "- Returns the number of bytes written.",
        __tool_write_file,
        NULL,
        MCP_PROP_STR("path", "The file path to write, must start with " CLAW_FS_ROOT_PATH),
        MCP_PROP_STR("content", "The content to write to the file")
    ));

    /* edit_file tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "edit_file",
        "Replace the first occurrence of a string in a file.\n"
        "Parameters:\n"
        "- path (string): The file path to edit, must start with " CLAW_FS_ROOT_PATH ".\n"
        "- old_string (string): The string to find.\n"
        "- new_string (string): The replacement string.\n"
        "Response:\n"
        "- Returns 'OK: edit done' on success.",
        __tool_edit_file,
        NULL,
        MCP_PROP_STR("path", "The file path to edit, must start with " CLAW_FS_ROOT_PATH),
        MCP_PROP_STR("old_string", "The string to find and replace"),
        MCP_PROP_STR("new_string", "The replacement string")
    ));

    /* list_dir tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "list_dir",
        "List files in a directory on the device filesystem.\n"
        "Parameters:\n"
        "- prefix (string, optional): Directory path to list, defaults to " CLAW_FS_ROOT_PATH ".\n"
        "Response:\n"
        "- Returns a list of filenames in the directory.",
        __tool_list_dir,
        NULL,
        MCP_PROP_STR_DEF("prefix", "Directory path to list, defaults to " CLAW_FS_ROOT_PATH, CLAW_FS_ROOT_PATH "/")
    ));

    /* find_path tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "find_path",
        "Recursively search the device filesystem for files or directories whose\n"
        "name contains the given keyword (case-insensitive, substring match).\n"
        "Use this tool when you are unsure of the exact path or spelling of a file.\n"
        "Parameters:\n"
        "- name (string, required): Filename keyword to search for.\n"
        "Response:\n"
        "- Returns a list of matching paths under " CLAW_FS_ROOT_PATH ".",
        __tool_find_path,
        NULL,
        MCP_PROP_STR("name", "Filename keyword to search for (case-insensitive substring match)")
    ));

    PR_DEBUG("File operation MCP tools registered successfully");
    return OPRT_OK;
}

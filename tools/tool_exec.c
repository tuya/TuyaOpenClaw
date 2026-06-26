/**
 * @file tool_exec.c
 * @brief MCP exec/system tools for TuyaOpenClaw
 * @version 0.2
 * @date 2026   -04-20
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Implements exec, get_system_info, list_devices, handle_command MCP tools
 * using the ai_mcp_server.h interface.
 */

#include "tool_exec.h"
#include "app_base_config.h"
#include "ai_mcp_server.h"
#include "cJSON.h"
#include "tal_api.h"

#include <string.h>

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PI_SYS_MAX_OUTPUT_BYTES (64 * 1024)

// =============================
// Internal: pi_system_interface
// =============================

static uint32_t __now_ms(void)
{
    return tal_system_get_millisecond();
}

static OPERATE_RET __pi_system_read_text_file(const char *path, size_t max_bytes, char **out_buf, size_t *out_len)
{
    if (!path || !out_buf || max_bytes == 0) {
        return OPRT_INVALID_PARM;
    }

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return OPRT_COM_ERROR;
    }

    char *buf = (char *)tal_malloc(max_bytes + 1);
    if (!buf) {
        close(fd);
        return OPRT_MALLOC_FAILED;
    }

    ssize_t n = read(fd, buf, max_bytes);
    close(fd);

    if (n < 0) {
        tal_free(buf);
        return OPRT_COM_ERROR;
    }

    buf[n] = '\0';
    *out_buf = buf;
    if (out_len) {
        *out_len = (size_t)n;
    }

    return OPRT_OK;
#else
    (void)path;
    (void)max_bytes;
    (void)out_len;
    *out_buf = NULL;
    return OPRT_NOT_SUPPORTED;
#endif
}

static OPERATE_RET __pi_system_get_hostname(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return OPRT_INVALID_PARM;
    }

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    if (gethostname(buf, buf_len) != 0) {
        return OPRT_COM_ERROR;
    }
    buf[buf_len - 1] = '\0';
    return OPRT_OK;
#else
    (void)buf;
    (void)buf_len;
    return OPRT_NOT_SUPPORTED;
#endif
}

static OPERATE_RET __pi_system_get_uptime_s(uint64_t *uptime_s)
{
    if (!uptime_s) {
        return OPRT_INVALID_PARM;
    }

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    char *content = NULL;
    OPERATE_RET rt = __pi_system_read_text_file("/proc/uptime", 128, &content, NULL);
    if (rt != OPRT_OK || !content) {
        return (rt == OPRT_OK) ? OPRT_COM_ERROR : rt;
    }

    double up = 0.0;
    if (sscanf(content, "%lf", &up) != 1) {
        tal_free(content);
        return OPRT_COM_ERROR;
    }

    tal_free(content);
    if (up < 0) {
        return OPRT_COM_ERROR;
    }

    *uptime_s = (uint64_t)up;
    return OPRT_OK;
#else
    (void)uptime_s;
    return OPRT_NOT_SUPPORTED;
#endif
}

static OPERATE_RET __pi_system_exec_capture(const char *command,
                                           uint32_t timeout_ms,
                                           char **out_buf,
                                           size_t *out_len,
                                           int *exit_code,
                                           bool *truncated)
{
    if (!command || !out_buf) {
        return OPRT_INVALID_PARM;
    }

    *out_buf = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (exit_code) {
        *exit_code = -1;
    }
    if (truncated) {
        *truncated = false;
    }

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    int pipe_fd[2] = { -1, -1 };
    if (pipe(pipe_fd) != 0) {
        return OPRT_COM_ERROR;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return OPRT_COM_ERROR;
    }

    if (pid == 0) {
        (void)dup2(pipe_fd[1], STDOUT_FILENO);
        (void)dup2(pipe_fd[1], STDERR_FILENO);

        close(pipe_fd[0]);
        close(pipe_fd[1]);

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipe_fd[1]);

    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);
    }

    char *buf = (char *)tal_malloc(PI_SYS_MAX_OUTPUT_BYTES + 1);
    if (!buf) {
        close(pipe_fd[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return OPRT_MALLOC_FAILED;
    }

    size_t used = 0;
    bool child_exited = false;
    int status = 0;

    char *tmp = (char *)claw_malloc(1024);
    if (!tmp) {
        close(pipe_fd[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        tal_free(buf);
        return OPRT_MALLOC_FAILED;
    }

    uint32_t start_ms = __now_ms();

    while (1) {
        if (!child_exited) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                child_exited = true;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_fd[0], &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000; /* 200ms */

        int sel = select(pipe_fd[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(pipe_fd[0], &rfds)) {
            ssize_t n = read(pipe_fd[0], tmp, 1024);
            if (n > 0) {
                size_t to_copy = (size_t)n;
                if (used + to_copy > PI_SYS_MAX_OUTPUT_BYTES) {
                    to_copy = PI_SYS_MAX_OUTPUT_BYTES - used;
                    if (truncated) {
                        *truncated = true;
                    }
                }
                if (to_copy > 0) {
                    memcpy(buf + used, tmp, to_copy);
                    used += to_copy;
                }
            }
        }

        if (child_exited) {
            while (used < PI_SYS_MAX_OUTPUT_BYTES) {
                ssize_t n = read(pipe_fd[0], tmp, 1024);
                if (n <= 0) {
                    break;
                }
                size_t to_copy = (size_t)n;
                if (used + to_copy > PI_SYS_MAX_OUTPUT_BYTES) {
                    to_copy = PI_SYS_MAX_OUTPUT_BYTES - used;
                    if (truncated) {
                        *truncated = true;
                    }
                }
                if (to_copy > 0) {
                    memcpy(buf + used, tmp, to_copy);
                    used += to_copy;
                }
            }
            break;
        }

        if (timeout_ms > 0) {
            uint32_t now = __now_ms();
            if ((uint32_t)(now - start_ms) >= timeout_ms) {
                (void)kill(pid, SIGKILL);
                (void)waitpid(pid, &status, 0);
                close(pipe_fd[0]);
                claw_free(tmp);
                buf[used] = '\0';
                *out_buf = buf;
                if (out_len) {
                    *out_len = used;
                }
                if (exit_code) {
                    *exit_code = -1;
                }
                return OPRT_TIMEOUT;
            }
        }
    }

    close(pipe_fd[0]);
    claw_free(tmp);

    buf[used] = '\0';
    *out_buf = buf;
    if (out_len) {
        *out_len = used;
    }

    if (exit_code) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *exit_code = 128 + WTERMSIG(status);
        } else {
            *exit_code = -1;
        }
    }

    return OPRT_OK;
#else
    (void)command;
    (void)timeout_ms;
    (void)out_len;
    (void)exit_code;
    (void)truncated;
    return OPRT_NOT_SUPPORTED;
#endif
}

// =============================
// Internal: pi_device_manager
// =============================

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
static char *__read_small_file_trim(const char *path)
{
    char *buf = NULL;
    if (__pi_system_read_text_file(path, 256, &buf, NULL) != OPRT_OK || !buf) {
        return NULL;
    }

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[len - 1] = '\0';
        len--;
    }
    return buf;
}

static void __add_netifs(cJSON *arr)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        return;
    }

    char *path = (char *)claw_malloc(256);
    if (!path) {
        closedir(d);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        const char *ifname = ent->d_name;
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            break;
        }

        cJSON_AddStringToObject(obj, "name", ifname);

        snprintf(path, 256, "/sys/class/net/%s/address", ifname);
        char *mac = __read_small_file_trim(path);
        if (mac) {
            cJSON_AddStringToObject(obj, "mac", mac);
            tal_free(mac);
        }

        snprintf(path, 256, "/sys/class/net/%s/operstate", ifname);
        char *state = __read_small_file_trim(path);
        if (state) {
            cJSON_AddStringToObject(obj, "state", state);
            tal_free(state);
        }

        cJSON_AddItemToArray(arr, obj);
    }

    claw_free(path);
    closedir(d);
}

static void __add_block_devs(cJSON *arr)
{
    DIR *d = opendir("/sys/block");
    if (!d) {
        return;
    }

    char *path = (char *)claw_malloc(256);
    if (!path) {
        closedir(d);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        const char *dev = ent->d_name;

        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            break;
        }

        cJSON_AddStringToObject(obj, "name", dev);

        snprintf(path, 256, "/sys/block/%s/size", dev);
        char *size_s = __read_small_file_trim(path);
        if (size_s) {
            cJSON_AddStringToObject(obj, "size_sectors", size_s);
            tal_free(size_s);
        }

        snprintf(path, 256, "/sys/block/%s/device/model", dev);
        char *model = __read_small_file_trim(path);
        if (model) {
            cJSON_AddStringToObject(obj, "model", model);
            tal_free(model);
        }

        snprintf(path, 256, "/sys/block/%s/removable", dev);
        char *rem = __read_small_file_trim(path);
        if (rem) {
            cJSON_AddStringToObject(obj, "removable", rem);
            tal_free(rem);
        }

        cJSON_AddItemToArray(arr, obj);
    }

    claw_free(path);
    closedir(d);
}

static void __add_usb_devs(cJSON *arr)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) {
        return;
    }

    char *base = (char *)claw_malloc(256);
    char *path = (char *)claw_malloc(300);
    if (!base || !path) {
        claw_free(base);
        claw_free(path);
        closedir(d);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        snprintf(base, 256, "/sys/bus/usb/devices/%s", ent->d_name);

        snprintf(path, 300, "%s/idVendor", base);
        char *vendor = __read_small_file_trim(path);
        if (!vendor) {
            continue; /* not a device node */
        }

        snprintf(path, 300, "%s/idProduct", base);
        char *product_id = __read_small_file_trim(path);

        snprintf(path, 300, "%s/product", base);
        char *product = __read_small_file_trim(path);

        snprintf(path, 300, "%s/manufacturer", base);
        char *mfg = __read_small_file_trim(path);

        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            tal_free(vendor);
            if (product_id) {
                tal_free(product_id);
            }
            if (product) {
                tal_free(product);
            }
            if (mfg) {
                tal_free(mfg);
            }
            break;
        }

        cJSON_AddStringToObject(obj, "sysfs", ent->d_name);
        cJSON_AddStringToObject(obj, "idVendor", vendor);
        if (product_id) {
            cJSON_AddStringToObject(obj, "idProduct", product_id);
        }
        if (mfg) {
            cJSON_AddStringToObject(obj, "manufacturer", mfg);
        }
        if (product) {
            cJSON_AddStringToObject(obj, "product", product);
        }

        cJSON_AddItemToArray(arr, obj);

        tal_free(vendor);
        if (product_id) {
            tal_free(product_id);
        }
        if (product) {
            tal_free(product);
        }
        if (mfg) {
            tal_free(mfg);
        }
    }

    claw_free(base);
    claw_free(path);
    closedir(d);
}
#endif

static OPERATE_RET __pi_device_manager_list_devices(cJSON **out_json)
{
    if (!out_json) {
        return OPRT_INVALID_PARM;
    }

    *out_json = NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return OPRT_MALLOC_FAILED;
    }

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    cJSON *net = cJSON_CreateArray();
    cJSON *usb = cJSON_CreateArray();
    cJSON *blk = cJSON_CreateArray();
    if (!net || !usb || !blk) {
        if (net) cJSON_Delete(net);
        if (usb) cJSON_Delete(usb);
        if (blk) cJSON_Delete(blk);
        cJSON_Delete(root);
        return OPRT_MALLOC_FAILED;
    }

    __add_netifs(net);
    __add_usb_devs(usb);
    __add_block_devs(blk);

    cJSON_AddItemToObject(root, "net", net);
    cJSON_AddItemToObject(root, "usb", usb);
    cJSON_AddItemToObject(root, "block", blk);

    cJSON_AddStringToObject(root, "platform", "linux");
#else
    cJSON_AddStringToObject(root, "platform", "non-linux");
    cJSON_AddStringToObject(root, "error", "not supported on this platform");
#endif

    *out_json = root;
    return OPRT_OK;
}

// =============================
// Internal: raspberry_pi_controller
// =============================

static bool __is_allowed_safe_command(const char *cmd)
{
    if (!cmd || cmd[0] == '\0' || CLAW_WS_AUTH_TOKEN[0] == '\0') {
        PR_ERR("command is empty or auth token is empty");
        return false;
    }

    /* Blacklist of dangerous commands/patterns that could compromise the device */
    static const char *blacklist[] = {
        "rm -rf /",
        "mkfs",
        "dd if=",
        "> /dev/sd",
        "chmod -R 777 /",
        ":(){ :|:",       /* fork bomb */
        NULL
    };

    for (int i = 0; blacklist[i] != NULL; i++) {
        if (strstr(cmd, blacklist[i]) != NULL) {
            return false;
        }
    }

    return true;
}

static OPERATE_RET __raspberry_pi_controller_exec(const char *command,
                                                 uint32_t timeout_ms,
                                                 bool unsafe,
                                                 cJSON **out_json)
{
    if (!command || !out_json) {
        return OPRT_INVALID_PARM;
    }

    *out_json = NULL;

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    /* Always enforce blacklist regardless of unsafe flag */
    if (!__is_allowed_safe_command(command)) {
        cJSON *json = cJSON_CreateObject();
        if (!json) {
            return OPRT_MALLOC_FAILED;
        }
        cJSON_AddStringToObject(json, "error", "command blocked by security blacklist");
        cJSON_AddStringToObject(json, "command", command);
        *out_json = json;
        return OPRT_INVALID_PARM;
    }

    char *output = NULL;
    size_t out_len = 0;
    int exit_code = -1;
    bool truncated = false;

    OPERATE_RET rt = __pi_system_exec_capture(command, timeout_ms, &output, &out_len, &exit_code, &truncated);

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        if (output) {
            tal_free(output);
        }
        return OPRT_MALLOC_FAILED;
    }

    cJSON_AddStringToObject(json, "command", command);
    cJSON_AddNumberToObject(json, "timeout_ms", (double)timeout_ms);
    cJSON_AddBoolToObject(json, "unsafe", unsafe ? 1 : 0);

    cJSON_AddNumberToObject(json, "rt", (double)rt);
    cJSON_AddNumberToObject(json, "exit_code", (double)exit_code);
    cJSON_AddBoolToObject(json, "truncated", truncated ? 1 : 0);

    if (output) {
        cJSON_AddStringToObject(json, "output", output);
        tal_free(output);
    } else {
        cJSON_AddStringToObject(json, "output", "");
    }

    (void)out_len;
    *out_json = json;
    return OPRT_OK;
#else
    (void)timeout_ms;
    (void)unsafe;
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return OPRT_MALLOC_FAILED;
    }
    cJSON_AddStringToObject(json, "error", "not supported on this platform");
    cJSON_AddStringToObject(json, "command", command);
    *out_json = json;
    return OPRT_NOT_SUPPORTED;
#endif
}

static OPERATE_RET __raspberry_pi_controller_get_system_info(cJSON **out_json)
{
    if (!out_json) {
        return OPRT_INVALID_PARM;
    }

    *out_json = NULL;

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return OPRT_MALLOC_FAILED;
    }

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    char hostname[128] = {0};
    if (__pi_system_get_hostname(hostname, sizeof(hostname)) == OPRT_OK) {
        cJSON_AddStringToObject(json, "hostname", hostname);
    }

    uint64_t uptime_s = 0;
    if (__pi_system_get_uptime_s(&uptime_s) == OPRT_OK) {
        cJSON_AddNumberToObject(json, "uptime_s", (double)uptime_s);
    }

    char *osr = NULL;
    if (__pi_system_read_text_file("/etc/os-release", 2048, &osr, NULL) == OPRT_OK && osr) {
        cJSON_AddStringToObject(json, "os_release", osr);
        tal_free(osr);
    }

    char *loadavg = NULL;
    if (__pi_system_read_text_file("/proc/loadavg", 256, &loadavg, NULL) == OPRT_OK && loadavg) {
        cJSON_AddStringToObject(json, "loadavg", loadavg);
        tal_free(loadavg);
    }

    cJSON_AddStringToObject(json, "platform", "linux");
#else
    cJSON_AddStringToObject(json, "platform", "non-linux");
    cJSON_AddStringToObject(json, "error", "not supported on this platform");
#endif

    *out_json = json;
    return OPRT_OK;
}

// =============================
// Internal: device_command_handler
// =============================

static const char *__json_get_str(cJSON *obj, const char *name)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(v) && v->valuestring) {
        return v->valuestring;
    }
    return NULL;
}

static uint32_t __json_get_u32(cJSON *obj, const char *name, uint32_t def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 0) {
            return def;
        }
        return (uint32_t)v->valuedouble;
    }
    return def;
}

static bool __json_get_bool(cJSON *obj, const char *name, bool def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    return def;
}

static OPERATE_RET __device_command_handler_handle(const char *request_json, char **response_json)
{
    if (!request_json || !response_json) {
        return OPRT_INVALID_PARM;
    }

    *response_json = NULL;

    cJSON *req = cJSON_Parse(request_json);
    if (!req) {
        return OPRT_INVALID_PARM;
    }

    const char *action = __json_get_str(req, "action");

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(req);
        return OPRT_MALLOC_FAILED;
    }

    if (!action) {
        cJSON_AddStringToObject(resp, "error", "missing action");
    } else if (strcmp(action, "exec") == 0) {
        const char *cmd = __json_get_str(req, "command");
        uint32_t timeout_ms = __json_get_u32(req, "timeout_ms", 5000);
        bool unsafe = __json_get_bool(req, "unsafe", true);

        if (!cmd) {
            cJSON_AddStringToObject(resp, "error", "missing command");
        } else {
            cJSON *out = NULL;
            OPERATE_RET rt = __raspberry_pi_controller_exec(cmd, timeout_ms, unsafe, &out);
            cJSON_AddNumberToObject(resp, "rt", (double)rt);
            if (out) {
                cJSON_AddItemToObject(resp, "result", out);
            }
        }
    } else if (strcmp(action, "system_info") == 0) {
        cJSON *out = NULL;
        OPERATE_RET rt = __raspberry_pi_controller_get_system_info(&out);
        cJSON_AddNumberToObject(resp, "rt", (double)rt);
        if (out) {
            cJSON_AddItemToObject(resp, "result", out);
        }
    } else if (strcmp(action, "list_devices") == 0) {
        cJSON *out = NULL;
        OPERATE_RET rt = __pi_device_manager_list_devices(&out);
        cJSON_AddNumberToObject(resp, "rt", (double)rt);
        if (out) {
            cJSON_AddItemToObject(resp, "result", out);
        }
    } else {
        cJSON_AddStringToObject(resp, "error", "unknown action");
        cJSON_AddStringToObject(resp, "action", action);
    }

    char *printed = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(req);

    if (!printed) {
        return OPRT_MALLOC_FAILED;
    }

    size_t len = strlen(printed);
    char *copy = (char *)tal_malloc(len + 1);
    if (!copy) {
        cJSON_free(printed);
        return OPRT_MALLOC_FAILED;
    }
    memcpy(copy, printed, len + 1);
    cJSON_free(printed);

    *response_json = copy;
    return OPRT_OK;
}

static const char *__get_str_prop(const MCP_PROPERTY_LIST_T *properties, const char *name)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_STRING) {
        return prop->default_val.str_val;
    }
    return NULL;
}

static bool __get_int_prop(const MCP_PROPERTY_LIST_T *properties, const char *name, int *out)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_INTEGER) {
        *out = prop->default_val.int_val;
        return true;
    }
    return false;
}

static bool __get_bool_prop(const MCP_PROPERTY_LIST_T *properties, const char *name, bool *out)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_BOOLEAN) {
        *out = prop->default_val.bool_val;
        return true;
    }
    return false;
}

static OPERATE_RET __tool_pi_exec(const MCP_PROPERTY_LIST_T *properties,
                                 MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    (void)user_data;

    const char *cmd = __get_str_prop(properties, "command");
    if (!cmd || cmd[0] == '\0') {
        ai_mcp_return_value_set_str(ret_val, "Error: command is required");
        return OPRT_INVALID_PARM;
    }

    int timeout_ms = 5000;
    (void)__get_int_prop(properties, "timeout_ms", &timeout_ms);
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    bool unsafe = true;
    (void)__get_bool_prop(properties, "unsafe", &unsafe);

    cJSON *out = NULL;
    OPERATE_RET rt = __raspberry_pi_controller_exec(cmd, (uint32_t)timeout_ms, unsafe, &out);
    if (out) {
        ai_mcp_return_value_set_json(ret_val, out);
        return OPRT_OK;
    }

    ai_mcp_return_value_set_str(ret_val, "Error: failed to execute");
    return rt;
}

static OPERATE_RET __tool_pi_system_info(const MCP_PROPERTY_LIST_T *properties,
                                        MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    (void)properties;
    (void)user_data;

    cJSON *out = NULL;
    OPERATE_RET rt = __raspberry_pi_controller_get_system_info(&out);
    if (out) {
        ai_mcp_return_value_set_json(ret_val, out);
        return OPRT_OK;
    }

    ai_mcp_return_value_set_str(ret_val, "Error: failed to get system info");
    return rt;
}

static OPERATE_RET __tool_pi_list_devices(const MCP_PROPERTY_LIST_T *properties,
                                         MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    (void)properties;
    (void)user_data;

    cJSON *out = NULL;
    OPERATE_RET rt = __pi_device_manager_list_devices(&out);
    if (out) {
        ai_mcp_return_value_set_json(ret_val, out);
        return OPRT_OK;
    }

    ai_mcp_return_value_set_str(ret_val, "Error: failed to list devices");
    return rt;
}

static OPERATE_RET __tool_pi_handle_command(const MCP_PROPERTY_LIST_T *properties,
                                           MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    (void)user_data;

    const char *req = __get_str_prop(properties, "request_json");
    if (!req) {
        ai_mcp_return_value_set_str(ret_val, "Error: request_json is required");
        return OPRT_INVALID_PARM;
    }

    char *resp = NULL;
    OPERATE_RET rt = __device_command_handler_handle(req, &resp);
    if (rt != OPRT_OK || !resp) {
        ai_mcp_return_value_set_str(ret_val, "Error: command handler failed");
        if (resp) {
            tal_free(resp);
        }
        return rt;
    }

    ai_mcp_return_value_set_str(ret_val, resp);
    tal_free(resp);
    return OPRT_OK;
}

OPERATE_RET tool_exec_register(void)
{
    OPERATE_RET rt = OPRT_OK;
    OPERATE_RET add_rt = OPRT_OK;

    // NOTE:
    // Some cloud/tool-list pipelines may reject or filter names like "exec".
    // Register safer primary names first, then try best-effort aliases.

    PR_INFO("[tool_exec] register tools...");

    // Primary (prefer using these in skills/prompts)
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "shell_exec",
        "Execute a shell command on device and capture output.",
        __tool_pi_exec,
        NULL,
        MCP_PROP_STR("command", "Shell command to execute (runs via /bin/sh -c)."),
        MCP_PROP_INT_DEF_RANGE("timeout_ms", "Timeout in milliseconds (0=no timeout).", 5000, 0, 600000),
        MCP_PROP_BOOL_DEF("unsafe", "Compatibility flag (no effect; all commands are allowed).", TRUE)
    ));

    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "shell_get_system_info",
        "Get basic system information.",
        __tool_pi_system_info,
        NULL
    ));

    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "shell_list_devices",
        "List local devices (net/usb/block) via sysfs on Linux.",
        __tool_pi_list_devices,
        NULL
    ));

    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "shell_handle_command",
        "Handle a JSON command request and return a JSON string response.",
        __tool_pi_handle_command,
        NULL,
        MCP_PROP_STR("request_json", "JSON request like {\"action\":\"exec\",...}." )
    ));

    // Best-effort aliases (won't break init if rejected)
    add_rt = AI_MCP_TOOL_ADD(
        "exec",
        "Execute a command on device and capture output.",
        __tool_pi_exec,
        NULL,
        MCP_PROP_STR("command", "Shell command to execute (runs via /bin/sh -c)."),
        MCP_PROP_INT_DEF_RANGE("timeout_ms", "Timeout in milliseconds (0=no timeout).", 5000, 0, 600000),
        MCP_PROP_BOOL_DEF("unsafe", "Compatibility flag (no effect; all commands are allowed).", TRUE)
    );
    if (add_rt != OPRT_OK) {
        PR_WARN("[tool_exec] add alias 'exec' failed rt:%d", add_rt);
    }

    add_rt = AI_MCP_TOOL_ADD(
        "get_system_info",
        "Get basic system information.",
        __tool_pi_system_info,
        NULL
    );
    if (add_rt != OPRT_OK) {
        PR_WARN("[tool_exec] add alias 'get_system_info' failed rt:%d", add_rt);
    }

    add_rt = AI_MCP_TOOL_ADD(
        "list_devices",
        "List local devices (net/usb/block) via sysfs on Linux.",
        __tool_pi_list_devices,
        NULL
    );
    if (add_rt != OPRT_OK) {
        PR_WARN("[tool_exec] add alias 'list_devices' failed rt:%d", add_rt);
    }

    add_rt = AI_MCP_TOOL_ADD(
        "handle_command",
        "Handle a JSON command request and return a JSON string response.",
        __tool_pi_handle_command,
        NULL,
        MCP_PROP_STR("request_json", "JSON request like {\"action\":\"exec\",...}." )
    );
    if (add_rt != OPRT_OK) {
        PR_WARN("[tool_exec] add alias 'handle_command' failed rt:%d", add_rt);
    }

    // Dot aliases
    TUYA_CALL_ERR_LOG(AI_MCP_TOOL_ADD(
        "pi.exec",
        "Execute a command on Raspberry Pi/Linux and capture output.",
        __tool_pi_exec,
        NULL,
        MCP_PROP_STR("command", "Shell command to execute (runs via /bin/sh -c)."),
        MCP_PROP_INT_DEF_RANGE("timeout_ms", "Timeout in milliseconds (0=no timeout).", 5000, 0, 600000),
        MCP_PROP_BOOL_DEF("unsafe", "Compatibility flag (no effect; all commands are allowed).", TRUE)
    ));

    TUYA_CALL_ERR_LOG(AI_MCP_TOOL_ADD(
        "pi.system_info",
        "Get basic Raspberry Pi/Linux system information.",
        __tool_pi_system_info,
        NULL
    ));

    TUYA_CALL_ERR_LOG(AI_MCP_TOOL_ADD(
        "pi.list_devices",
        "List Raspberry Pi/Linux local devices (net/usb/block) via sysfs.",
        __tool_pi_list_devices,
        NULL
    ));

    TUYA_CALL_ERR_LOG(AI_MCP_TOOL_ADD(
        "pi.handle_command",
        "Handle a JSON command request and return a JSON string response.",
        __tool_pi_handle_command,
        NULL,
        MCP_PROP_STR("request_json", "JSON request like {\"action\":\"exec\",...}.")
    ));

    PR_INFO("[tool_exec] register tools done");
    return rt;
}
#endif

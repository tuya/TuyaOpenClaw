/**
 * @file app_base_config.h
 * @brief Base configuration macros and helpers for TuyaOpenClaw.
 *
 * Consolidates:
 *   - Filesystem type selection, root paths, and claw_f* / claw_fs_* macros
 *   - Memory allocation wrappers (claw_malloc / claw_free)
 *   - KV-backed runtime-overridable configuration helpers
 *
 * Include this file whenever you need platform-aware filesystem macros,
 * memory allocation wrappers, or KV-backed config getters.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __APP_BASE_CONFIG_H__
#define __APP_BASE_CONFIG_H__

#include "tuya_cloud_types.h"
#include "tuya_app_config.h"
#include "tal_api.h"
#include "tal_kv.h"
#include "tal_fs.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Section 1 – Filesystem type selection & SD card support
 * =========================================================================*/

#ifndef CLAW_USE_SDCARD
#define CLAW_USE_SDCARD 0
#endif

#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
#include "tkl_fs.h"
#endif

/**
 * @brief Filesystem root path macros
 *
 * CLAW_FS_ROOT_PATH  – prefix prepended to all config / data paths
 * CLAW_FS_MOUNT_PATH – SD card mount point (only when using SD card)
 */
#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
#define CLAW_FS_ROOT_PATH          "/sdcard"
#define CLAW_FS_MOUNT_PATH         "/sdcard"
#define CLAW_FS_ROOT_PATH_EMPTY    0
#else
#define CLAW_FS_ROOT_PATH          ""
#define CLAW_FS_ROOT_PATH_EMPTY    1
#endif

/* Default config file paths */
#define CLAW_CONFIG_DIR        CLAW_FS_ROOT_PATH "/config"
#define SOUL_FILE              CLAW_CONFIG_DIR "/SOUL.md"
#define USER_FILE              CLAW_CONFIG_DIR "/USER.md"

/* ===========================================================================
 * Section 2 – Unified filesystem interface macros
 *
 * When CLAW_USE_SDCARD == 1 → tkl (SD card) interfaces
 * Otherwise                 → tal (flash) interfaces
 * =========================================================================*/

#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
#define claw_fopen     tkl_fopen
#define claw_fclose    tkl_fclose
#define claw_fread     tkl_fread
#define claw_fwrite    tkl_fwrite
#define claw_fgets     tkl_fgets
#define claw_fsync(file) tkl_fsync(tkl_fileno(file))
#define claw_feof      tkl_feof
#define claw_fgetsize  tkl_fgetsize
#define claw_dir_open  tkl_dir_open
#define claw_dir_close tkl_dir_close
#define claw_dir_read  tkl_dir_read
#define claw_dir_name  tkl_dir_name
#define claw_dir_is_directory tkl_dir_is_directory
#define claw_dir_is_regular   tkl_dir_is_regular
#define claw_fs_mkdir         tkl_fs_mkdir
#define claw_fs_mount         tkl_fs_mount
#define claw_fs_remove        tkl_fs_remove
#define claw_fs_is_exist      tkl_fs_is_exist
#define claw_fs_mode          tkl_fs_mode
#define claw_fs_rename        tkl_fs_rename
#else
#define claw_fopen          tal_fopen
#define claw_fclose         tal_fclose
#define claw_fread          tal_fread
#define claw_fwrite         tal_fwrite
#define claw_fgets          tal_fgets
#define claw_fsync          tal_fsync
#define claw_feof           tal_feof
#define claw_fgetsize       tal_fgetsize
#define claw_dir_open       tal_dir_open
#define claw_dir_close      tal_dir_close
#define claw_dir_read       tal_dir_read
#define claw_dir_name       tal_dir_name
#define claw_dir_is_directory tal_dir_is_directory
#define claw_dir_is_regular   tal_dir_is_regular
#define claw_fs_mkdir         tal_fs_mkdir
#define claw_fs_remove        tal_fs_remove
#define claw_fs_is_exist      tal_fs_is_exist
#define claw_fs_mode          tal_fs_mode
#define claw_fs_rename        tal_fs_rename
#endif

/* ===========================================================================
 * Section 3 – Memory allocation wrappers
 * =========================================================================*/

#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
#define claw_malloc    tal_psram_malloc
#define claw_free      tal_psram_free
#else
#define claw_malloc    tal_malloc
#define claw_free      tal_free
#endif

/* ===========================================================================
 * Section 4 – KV-backed runtime configuration
 *
 * Each getter checks KV storage first, then falls back to the compile-time
 * default defined in tuya_app_config.h.
 * =========================================================================*/

/* ---- KV key definitions ---- */

#define APP_KV_PRODUCT_ID   "app.product_id"
#define APP_KV_UUID         "UUID_TUYAOPEN"
#define APP_KV_AUTHKEY      "AUTHKEY_TUYAOPEN"
#define APP_KV_WS_TOKEN     "ws_auth_token"   /* reuse existing ws_server key */
#define APP_KV_GW_HOST      "app.gw_host"
#define APP_KV_GW_PORT      "app.gw_port"
#define APP_KV_GW_TOKEN     "app.gw_token"
#define APP_KV_DEVICE_ID    "app.device_id"

/* ---- Generic string getter: KV first, then compile-time default ---- */

static inline OPERATE_RET app_kv_get_string(const char *kv_key, const char *build_default,
                                            char *out, size_t out_size)
{
    if (!kv_key || !out || out_size == 0) return OPRT_INVALID_PARM;

    uint8_t *buf = NULL;
    size_t   len = 0;
    OPERATE_RET rt = tal_kv_get(kv_key, &buf, &len);
    if (rt == OPRT_OK && buf && len > 0 && ((char *)buf)[0] != '\0') {
        size_t copy = (len < out_size - 1) ? len : (out_size - 1);
        memcpy(out, buf, copy);
        out[copy] = '\0';
        tal_kv_free(buf);
        return OPRT_OK;
    }
    if (buf) tal_kv_free(buf);

    if (build_default && build_default[0] != '\0') {
        snprintf(out, out_size, "%s", build_default);
        return OPRT_OK;
    }

    out[0] = '\0';
    return OPRT_NOT_FOUND;
}

static inline OPERATE_RET app_kv_set_string(const char *kv_key, const char *value)
{
    if (!kv_key || !value) return OPRT_INVALID_PARM;
    return tal_kv_set(kv_key, (const uint8_t *)value, strlen(value) + 1);
}

static inline OPERATE_RET app_kv_del(const char *kv_key)
{
    if (!kv_key) return OPRT_INVALID_PARM;
    return tal_kv_del(kv_key);
}

/* ---- Typed getters for each config item ---- */

static inline void app_cfg_get_product_id(char *out, size_t out_size)
{
    app_kv_get_string(APP_KV_PRODUCT_ID, TUYA_PRODUCT_ID, out, out_size);
}

static inline void app_cfg_get_ws_token(char *out, size_t out_size)
{
    app_kv_get_string(APP_KV_WS_TOKEN, CLAW_WS_AUTH_TOKEN, out, out_size);
}

static inline void app_cfg_get_gw_host(char *out, size_t out_size)
{
    app_kv_get_string(APP_KV_GW_HOST, OPENCLAW_GATEWAY_HOST, out, out_size);
}

static inline void app_cfg_get_gw_port(char *out, size_t out_size)
{
    char port_str[8] = {0};
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)OPENCLAW_GATEWAY_PORT);
    app_kv_get_string(APP_KV_GW_PORT, port_str, out, out_size);
}

static inline unsigned app_cfg_get_gw_port_num(void)
{
    char buf[8] = {0};
    app_cfg_get_gw_port(buf, sizeof(buf));
    unsigned port = (unsigned)strtoul(buf, NULL, 10);
    return (port > 0 && port <= 65535) ? port : (unsigned)OPENCLAW_GATEWAY_PORT;
}

static inline void app_cfg_get_gw_token(char *out, size_t out_size)
{
    app_kv_get_string(APP_KV_GW_TOKEN, OPENCLAW_GATEWAY_TOKEN, out, out_size);
}

static inline void app_cfg_get_device_id(char *out, size_t out_size)
{
    app_kv_get_string(APP_KV_DEVICE_ID, TUYAOPENCLAW_DEVICE_ID, out, out_size);
}

#ifdef __cplusplus
}
#endif

#endif /* __APP_BASE_CONFIG_H__ */

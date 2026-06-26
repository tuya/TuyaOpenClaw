/**
 * @file cli_cmd.c
 * @brief Unified CLI implementation for TuyaOpenClaw (config commands only)
 * @version 0.1
 * @date 2026-04-08
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "app_base_config.h"
#include "channels/discord_bot.h"
#include "channels/feishu_bot.h"
#include "channels/qqbot_channel.h"
#include "channels/telegram_bot.h"
#include "im_config.h"
#include "im_platform.h"
#include "proxy/http_proxy.h"
#include "tuya_authorize.h"

#include "tal_api.h"
#include "tal_cli.h"
#include "tal_kv.h"
#include "tal_log.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define CLI_LINE_SIZE      256
#define CLI_VALUE_SIZE     512
#define CLI_MASK_SIZE      64
#define CLI_UUID_LENGTH    20
#define CLI_AUTHKEY_LENGTH 32

/* ---------------------------------------------------------------------------
 * Typedefs
 * --------------------------------------------------------------------------- */
typedef OPERATE_RET (*CLI_SETTER_CB)(const char *value);

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
static void cmd_help(int argc, char *argv[]);
static void cmd_cfg_show(int argc, char *argv[]);
static void cmd_cfg_reset(int argc, char *argv[]);
static void cmd_cfg_set_product_id(int argc, char *argv[]);
static void cmd_cfg_set_auth(int argc, char *argv[]);
static void cmd_cfg_set_ws_token(int argc, char *argv[]);
static void cmd_cfg_set_gw_host(int argc, char *argv[]);
static void cmd_cfg_set_gw_port(int argc, char *argv[]);
static void cmd_cfg_set_gw_token(int argc, char *argv[]);
static void cmd_cfg_set_device_id(int argc, char *argv[]);
static void cmd_cfg_set_channel_mode(int argc, char *argv[]);
static void cmd_cfg_set_tg_token(int argc, char *argv[]);
static void cmd_cfg_set_dc_token(int argc, char *argv[]);
static void cmd_cfg_set_dc_channel(int argc, char *argv[]);
static void cmd_cfg_set_fs_appid(int argc, char *argv[]);
static void cmd_cfg_set_fs_appsecret(int argc, char *argv[]);
static void cmd_cfg_set_fs_allow(int argc, char *argv[]);
static void cmd_cfg_set_qq_appid(int argc, char *argv[]);
static void cmd_cfg_set_qq_secret(int argc, char *argv[]);
static void cmd_cfg_set_proxy(int argc, char *argv[]);
static void cmd_cfg_clear_proxy(int argc, char *argv[]);
static void cli_clear_weixin_cfg_overrides_(void);

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Echo a formatted line to CLI.
 * @param[in] fmt printf-style format string
 * @param[in] ... format arguments
 * @return none
 */
static void cli_echof_(const char *fmt, ...)
{
    char    line[CLI_LINE_SIZE] = {0};
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    tal_cli_echo(line);
}

/**
 * @brief Copy and mask a secret value for CLI display.
 * @param[in] src input string
 * @param[out] out masked output buffer
 * @param[in] out_size masked output buffer size
 * @return none
 */
static void cli_mask_copy_(const char *src, char *out, size_t out_size)
{
    size_t len;
    size_t head_len;
    size_t tail_len;

    if (out == NULL || out_size == 0) {
        return;
    }

    if (src == NULL || src[0] == '\0') {
        snprintf(out, out_size, "(empty)");
        return;
    }

    len = strlen(src);
    head_len = (len < 4) ? len : 4;
    tail_len = (len < 4) ? len : 4;

    snprintf(out, out_size, "%.*s******%.*s",
             (int)head_len, src,
             (int)tail_len, src + len - tail_len);
}

/**
 * @brief Convert a boolean state to CLI text.
 * @param[in] value boolean input
 * @return textual representation
 */
static const char *cli_bool_to_str_(bool value)
{
    return value ? "true" : "false";
}

/**
 * @brief Print one app configuration item with source information.
 * @param[in] label display label
 * @param[in] kv_key KV key
 * @param[in] build_value compile-time fallback
 * @param[in] mask whether to mask value
 * @return none
 */
static void cli_print_app_cfg_item_(const char *label, const char *kv_key, const char *build_value, bool mask)
{
    char        kv_value[CLI_VALUE_SIZE] = {0};
    char        masked[CLI_MASK_SIZE]    = {0};
    const char *source                   = "not set";
    const char *value                    = "(empty)";
    uint8_t    *buf                      = NULL;
    size_t      len                      = 0;

    if (tal_kv_get(kv_key, &buf, &len) == OPRT_OK && buf != NULL && len > 0 && ((char *)buf)[0] != '\0') {
        size_t copy_len = (len < sizeof(kv_value) - 1) ? len : (sizeof(kv_value) - 1);
        memcpy(kv_value, buf, copy_len);
        kv_value[copy_len] = '\0';
        value              = kv_value;
        source             = "kv";
    } else if (build_value != NULL && build_value[0] != '\0') {
        value  = build_value;
        source = "build";
    }

    if (buf != NULL) {
        tal_kv_free(buf);
    }

    if (mask && strcmp(value, "(empty)") != 0) {
        cli_mask_copy_(value, masked, sizeof(masked));
        cli_echof_("  %-18s %s [%s]", label, masked, source);
        return;
    }

    cli_echof_("  %-18s %s [%s]", label, value, source);
}

/**
 * @brief Print one config item using an explicit value and source.
 * @param[in] label display label
 * @param[in] value current value
 * @param[in] source value source text
 * @param[in] mask whether to mask value
 * @return none
 */
static void cli_print_cfg_value_item_(const char *label, const char *value, const char *source, bool mask)
{
    char        masked[CLI_MASK_SIZE] = {0};
    const char *display_value         = value;

    if (display_value == NULL || display_value[0] == '\0') {
        display_value = "(empty)";
    }

    if (source == NULL || source[0] == '\0') {
        source = "not set";
    }

    if (mask && strcmp(display_value, "(empty)") != 0) {
        cli_mask_copy_(display_value, masked, sizeof(masked));
        cli_echof_("  %-18s %s [%s]", label, masked, source);
        return;
    }

    cli_echof_("  %-18s %s [%s]", label, display_value, source);
}

/**
 * @brief Print one IM configuration item with source information.
 * @param[in] label display label
 * @param[in] ns IM KV namespace
 * @param[in] key IM KV key
 * @param[in] build_value compile-time fallback
 * @param[in] mask whether to mask value
 * @return none
 */
static void cli_print_im_cfg_item_(const char *label, const char *ns, const char *key, const char *build_value, bool mask)
{
    char        kv_value[CLI_VALUE_SIZE] = {0};
    char        masked[CLI_MASK_SIZE]    = {0};
    const char *source                   = "not set";
    const char *value                    = "(empty)";

    if (im_kv_get_string(ns, key, kv_value, sizeof(kv_value)) == OPRT_OK && kv_value[0] != '\0') {
        value  = kv_value;
        source = "kv";
    } else if (build_value != NULL && build_value[0] != '\0') {
        value  = build_value;
        source = "build";
    }

    if (mask && strcmp(value, "(empty)") != 0) {
        cli_mask_copy_(value, masked, sizeof(masked));
        cli_echof_("  %-18s %s [%s]", label, masked, source);
        return;
    }

    cli_echof_("  %-18s %s [%s]", label, value, source);
}

/**
 * @brief Clear all application config KV overrides.
 * @return none
 */
static void cli_clear_app_cfg_overrides_(void)
{
    static const char *const s_keys[] = {
        APP_KV_PRODUCT_ID,
        APP_KV_UUID,
        APP_KV_AUTHKEY,
        APP_KV_WS_TOKEN,
        APP_KV_GW_HOST,
        APP_KV_GW_PORT,
        APP_KV_GW_TOKEN,
        APP_KV_DEVICE_ID,
    };

    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        (void)app_kv_del(s_keys[i]);
    }
}

/**
 * @brief Clear all IM config KV overrides.
 * @return none
 */
static void cli_clear_im_cfg_overrides_(void)
{
    (void)im_kv_del(IM_NVS_BOT, IM_NVS_KEY_CHANNEL_MODE);

    (void)im_kv_del(IM_NVS_TG, IM_NVS_KEY_TG_TOKEN);

    (void)im_kv_del(IM_NVS_DC, IM_NVS_KEY_DC_TOKEN);
    (void)im_kv_del(IM_NVS_DC, IM_NVS_KEY_DC_CHANNEL_ID);
    (void)im_kv_del(IM_NVS_DC, IM_NVS_KEY_DC_LAST_MSG_ID);

    (void)im_kv_del(IM_NVS_FS, IM_NVS_KEY_FS_APP_ID);
    (void)im_kv_del(IM_NVS_FS, IM_NVS_KEY_FS_APP_SECRET);
    (void)im_kv_del(IM_NVS_FS, IM_NVS_KEY_FS_ALLOW_FROM);
    cli_clear_weixin_cfg_overrides_();

    (void)im_kv_del(IM_NVS_QQ, IM_NVS_KEY_QQ_APP_ID);
    (void)im_kv_del(IM_NVS_QQ, IM_NVS_KEY_QQ_SECRET);

    (void)http_proxy_clear();
}

/**
 * @brief Update UUID and authkey together through the authorize interface.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @param[in] usage usage text
 * @return none
 */
static void cli_set_authorize_pair_(int argc, char *argv[], const char *usage)
{
    const char *uuid    = NULL;
    const char *authkey = NULL;
    OPERATE_RET rt;

    if (argc < 3) {
        cli_echof_("Usage: %s", usage);
        cli_echof_("How to get UUID and authkey : https://platform.tuya.com/purchase/index?type=6");
        return;
    }

    uuid    = argv[1];
    authkey = argv[2];

    if (strlen(uuid) != CLI_UUID_LENGTH) {
        cli_echof_("ERR: uuid length must be %u", (unsigned)CLI_UUID_LENGTH);
        return;
    }

    if (strlen(authkey) != CLI_AUTHKEY_LENGTH) {
        cli_echof_("ERR: authkey length must be %u", (unsigned)CLI_AUTHKEY_LENGTH);
        return;
    }

    tal_cli_echo("Applying authorization update, device will reboot on success.");
    rt = tuya_authorize_write(uuid, authkey);
    if (rt != OPRT_OK) {
        cli_echof_("ERR: cfg_set_auth rt=%d", rt);
    }
}

/**
 * @brief Set an app config string value.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @param[in] usage usage text
 * @param[in] kv_key app config KV key
 * @param[in] label logical field label
 * @return none
 */
static void cli_set_app_cfg_value_(int argc, char *argv[], const char *usage, const char *kv_key, const char *label)
{
    OPERATE_RET rt;

    if (argc < 2) {
        cli_echof_("Usage: %s", usage);
        cli_echof_("How to get PID : https://pbt.tuya.com/s?p=dd46368ae3840e54f018b2c45dc1550b&u=c38c8fc0a5d14c4f66cae9f0cfcb2a24&t=2");
        return;
    }

    rt = app_kv_set_string(kv_key, argv[1]);
    cli_echof_("%s: %s rt=%d", (rt == OPRT_OK) ? "OK" : "ERR", label, rt);
}

/**
 * @brief Set an IM config value through a setter callback.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @param[in] usage usage text
 * @param[in] label logical field label
 * @param[in] setter setter callback
 * @return none
 */
static void cli_set_im_cfg_value_(int argc, char *argv[], const char *usage, const char *label, CLI_SETTER_CB setter)
{
    OPERATE_RET rt;

    if (argc < 2) {
        cli_echof_("Usage: %s", usage);
        return;
    }

    rt = setter(argv[1]);
    if (rt != OPRT_OK) {
        cli_echof_("ERR: %s rt=%d", label, rt);
        return;
    }

    cli_echof_("OK: %s saved to KV", label);
}

/**
 * @brief Clear all Weixin config KV overrides.
 * @return none
 */
static void cli_clear_weixin_cfg_overrides_(void)
{
    static const char *const s_keys[] = {
        IM_NVS_KEY_WX_TOKEN,
        IM_NVS_KEY_WX_HOST,
        IM_NVS_KEY_WX_ALLOW,
        IM_NVS_KEY_WX_UPD_BUF,
        IM_NVS_KEY_WX_CTX_TOK,
    };

    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        (void)im_kv_del(IM_NVS_WX, s_keys[i]);
    }
}

/* ---------------------------------------------------------------------------
 * Help
 * --------------------------------------------------------------------------- */
/**
 * @brief Show CLI help for config commands.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_help(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    tal_cli_echo("=== TuyaOpenClaw CLI (config) ===");
    tal_cli_echo("");
    tal_cli_echo("[Config]");
    cli_echof_("  %-28s %s", "cfg_show", "Show effective config (KV > build)");
    cli_echof_("  %-28s %s", "cfg_reset", "Clear all config KV overrides");
    cli_echof_("  %-28s %s", "cfg_set_product_id <id>", "Set Tuya product_id");
    cli_echof_("  %-28s %s", "cfg_set_auth <uuid> <authkey>", "Set Tuya uuid and authkey");
    cli_echof_("  %-28s %s", "cfg_set_ws_token <token>", "Set WebSocket token");
    cli_echof_("  %-28s %s", "cfg_set_gw_host <host>", "Set OpenClaw gateway host");
    cli_echof_("  %-28s %s", "cfg_set_gw_port <port>", "Set OpenClaw gateway port");
    cli_echof_("  %-28s %s", "cfg_set_gw_token <token>", "Set OpenClaw gateway token");
    cli_echof_("  %-28s %s", "cfg_set_device_id <id>", "Set TuyaOpenClaw device ID");
    cli_echof_("  %-28s %s", "cfg_set_channel_mode <mode>", "Set IM channel mode (telegram|discord|feishu|weixin|qqbot|OFF)");
    cli_echof_("  %-28s %s", "cfg_set_tg_token <token>", "Set Telegram token");
    cli_echof_("  %-28s %s", "cfg_set_dc_token <token>", "Set Discord token");
    cli_echof_("  %-28s %s", "cfg_set_dc_channel <id>", "Set Discord channel_id");
    cli_echof_("  %-28s %s", "cfg_set_fs_appid <id>", "Set Feishu app_id");
    cli_echof_("  %-28s %s", "cfg_set_fs_appsecret <secret>", "Set Feishu app_secret");
    cli_echof_("  %-28s %s", "cfg_set_fs_allow <csv>", "Set Feishu allow_from CSV");
    cli_echof_("  %-28s %s", "cfg_set_qq_appid <id>", "Set QQ Bot app_id");
    cli_echof_("  %-28s %s", "cfg_set_qq_secret <secret>", "Set QQ Bot client_secret");
    cli_echof_("  %-28s %s", "cfg_set_proxy <host> <port> [type]", "Set outbound proxy");
    cli_echof_("  %-28s %s", "cfg_clear_proxy", "Clear outbound proxy config");
    tal_cli_echo("");
    tal_cli_echo("Note: cfg_* changes take effect after reconnect or reboot.");
}

/* ---------------------------------------------------------------------------
 * Config commands
 * --------------------------------------------------------------------------- */
/**
 * @brief Show effective application and IM configuration.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_show(int argc, char *argv[])
{
    char                port_text[8]   = {0};
    tuya_iot_license_t  license        = {0};
    const char         *uuid_value     = NULL;
    const char         *authkey_value  = NULL;
    const char         *uuid_source    = "not set";
    const char         *authkey_source = "not set";

    (void)argc;
    (void)argv;

    tal_cli_echo("--- Effective config ---");
    tal_cli_echo("[Application]");
    cli_print_app_cfg_item_("product_id", APP_KV_PRODUCT_ID, TUYA_PRODUCT_ID, true);
    if (tuya_authorize_read(&license) == OPRT_OK) {
        if (license.uuid != NULL && license.uuid[0] != '\0') {
            uuid_value  = license.uuid;
            uuid_source = "authorize";
        }
        if (license.authkey != NULL && license.authkey[0] != '\0') {
            authkey_value  = license.authkey;
            authkey_source = "authorize";
        }
    }
    if (uuid_value == NULL && TUYA_OPENSDK_UUID[0] != '\0') {
        uuid_value  = TUYA_OPENSDK_UUID;
        uuid_source = "build";
    }
    if (authkey_value == NULL && TUYA_OPENSDK_AUTHKEY[0] != '\0') {
        authkey_value  = TUYA_OPENSDK_AUTHKEY;
        authkey_source = "build";
    }
    cli_print_cfg_value_item_("uuid", uuid_value, uuid_source, true);
    cli_print_cfg_value_item_("authkey", authkey_value, authkey_source, true);
    cli_print_app_cfg_item_("ws_token", APP_KV_WS_TOKEN, CLAW_WS_AUTH_TOKEN, true);
    cli_print_app_cfg_item_("gw_host", APP_KV_GW_HOST, OPENCLAW_GATEWAY_HOST, true);
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)OPENCLAW_GATEWAY_PORT);
    cli_print_app_cfg_item_("gw_port", APP_KV_GW_PORT, port_text, false);
    cli_print_app_cfg_item_("gw_token", APP_KV_GW_TOKEN, OPENCLAW_GATEWAY_TOKEN, true);
    cli_print_app_cfg_item_("device_id", APP_KV_DEVICE_ID, TUYAOPENCLAW_DEVICE_ID, true);

    tal_cli_echo("[IM]");
    cli_print_im_cfg_item_("channel_mode", IM_NVS_BOT, IM_NVS_KEY_CHANNEL_MODE, IM_SECRET_CHANNEL_MODE, false);
    cli_print_im_cfg_item_("tg.token", IM_NVS_TG, IM_NVS_KEY_TG_TOKEN, IM_SECRET_TG_TOKEN, true);
    cli_print_im_cfg_item_("dc.token", IM_NVS_DC, IM_NVS_KEY_DC_TOKEN, IM_SECRET_DC_TOKEN, true);
    cli_print_im_cfg_item_("dc.channel_id", IM_NVS_DC, IM_NVS_KEY_DC_CHANNEL_ID, IM_SECRET_DC_CHANNEL_ID, true);
    cli_print_im_cfg_item_("fs.app_id", IM_NVS_FS, IM_NVS_KEY_FS_APP_ID, IM_SECRET_FS_APP_ID, true);
    cli_print_im_cfg_item_("fs.app_secret", IM_NVS_FS, IM_NVS_KEY_FS_APP_SECRET, IM_SECRET_FS_APP_SECRET, true);
    cli_print_im_cfg_item_("fs.allow_from", IM_NVS_FS, IM_NVS_KEY_FS_ALLOW_FROM, IM_SECRET_FS_ALLOW_FROM, true);
    cli_print_im_cfg_item_("qq.app_id",     IM_NVS_QQ, IM_NVS_KEY_QQ_APP_ID,     IM_SECRET_QQ_APP_ID,     true);
    cli_print_im_cfg_item_("qq.secret",     IM_NVS_QQ, IM_NVS_KEY_QQ_SECRET,     IM_SECRET_QQ_CLIENT_SECRET, true);
    cli_print_im_cfg_item_("proxy.host", IM_NVS_PROXY, IM_NVS_KEY_PROXY_HOST, IM_SECRET_PROXY_HOST, true);
    cli_print_im_cfg_item_("proxy.port", IM_NVS_PROXY, IM_NVS_KEY_PROXY_PORT, IM_SECRET_PROXY_PORT, false);
    cli_print_im_cfg_item_("proxy.type", IM_NVS_PROXY, IM_NVS_KEY_PROXY_TYPE, IM_SECRET_PROXY_TYPE, false);
    cli_echof_("  %-18s %s", "proxy.enabled", cli_bool_to_str_(http_proxy_is_enabled()));

    tal_cli_echo("Note: cfg_* changes take effect after reconnect or reboot.");
}

/**
 * @brief Clear all config overrides.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_reset(int argc, char *argv[])
{
    OPERATE_RET auth_rt;

    (void)argc;
    (void)argv;

    cli_clear_app_cfg_overrides_();
    cli_clear_im_cfg_overrides_();
    auth_rt = tuya_authorize_reset();
    if (auth_rt != OPRT_OK) {
        cli_echof_("ERR: cfg_reset authorize rt=%d", auth_rt);
        return;
    }
    tal_cli_echo("OK: cleared all config KV overrides (fallback to build defaults).");
}

/**
 * @brief Set product_id override.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_product_id(int argc, char *argv[])
{
    cli_set_app_cfg_value_(argc, argv, "cfg_set_product_id <id>", APP_KV_PRODUCT_ID, "cfg_set_product_id");
}

/**
 * @brief Set UUID and authkey together.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_auth(int argc, char *argv[])
{
    cli_set_authorize_pair_(argc, argv, "cfg_set_auth <uuid> <authkey>");
}

/**
 * @brief Set WebSocket token override.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_ws_token(int argc, char *argv[])
{
    cli_set_app_cfg_value_(argc, argv, "cfg_set_ws_token <token>", APP_KV_WS_TOKEN, "cfg_set_ws_token");
}

/**
 * @brief Set gateway host override.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_gw_host(int argc, char *argv[])
{
    cli_set_app_cfg_value_(argc, argv, "cfg_set_gw_host <host>", APP_KV_GW_HOST, "cfg_set_gw_host");
}

/**
 * @brief Set gateway port override.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_gw_port(int argc, char *argv[])
{
    long port;

    if (argc < 2) {
        tal_cli_echo("Usage: cfg_set_gw_port <port>");
        return;
    }

    port = strtol(argv[1], NULL, 10);
    if (port <= 0 || port > 65535) {
        tal_cli_echo("ERR: invalid port (1..65535)");
        return;
    }

    cli_set_app_cfg_value_(argc, argv, "cfg_set_gw_port <port>", APP_KV_GW_PORT, "cfg_set_gw_port");
}

/**
 * @brief Set gateway token override.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_gw_token(int argc, char *argv[])
{
    cli_set_app_cfg_value_(argc, argv, "cfg_set_gw_token <token>", APP_KV_GW_TOKEN, "cfg_set_gw_token");
}

/**
 * @brief Set device_id override.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_device_id(int argc, char *argv[])
{
    cli_set_app_cfg_value_(argc, argv, "cfg_set_device_id <id>", APP_KV_DEVICE_ID, "cfg_set_device_id");
}

/**
 * @brief Set active IM channel mode.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_channel_mode(int argc, char *argv[])
{
    OPERATE_RET rt;
    const char *mode;

    if (argc < 2) {
        tal_cli_echo("Usage: cfg_set_channel_mode <mode> (OFF|telegram|discord|feishu|weixin|qqbot)");
        return;
    }

    mode = argv[1];
    if (strcmp(mode, IM_CHAN_OFF) != 0 &&
        strcmp(mode, IM_CHAN_TELEGRAM) != 0 &&
        strcmp(mode, IM_CHAN_DISCORD) != 0 &&
        strcmp(mode, IM_CHAN_FEISHU) != 0 &&
        strcmp(mode, IM_CHAN_WEIXIN) != 0 &&
        strcmp(mode, IM_CHAN_QQBOT) != 0) {
        tal_cli_echo("ERR: mode must be OFF | telegram | discord | feishu | weixin | qqbot");
        return;
    }

    if (strcmp(mode, IM_CHAN_WEIXIN) == 0) {
        cli_clear_weixin_cfg_overrides_();
    }

    rt = im_kv_set_string(IM_NVS_BOT, IM_NVS_KEY_CHANNEL_MODE, mode);
    if (rt != OPRT_OK) {
        cli_echof_("ERR: cfg_set_channel_mode rt=%d", rt);
        return;
    }

    cli_echof_("OK: channel_mode=%s (reconnect/reboot to take effect)", mode);
}

/**
 * @brief Set Telegram bot token.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_tg_token(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_tg_token <token>", "telegram token", telegram_set_token);
}

/**
 * @brief Set Discord bot token.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_dc_token(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_dc_token <token>", "discord token", discord_set_token);
}

/**
 * @brief Set Discord channel_id.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_dc_channel(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_dc_channel <channel_id>", "discord channel_id", discord_set_channel_id);
}

/**
 * @brief Set Feishu app_id.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_fs_appid(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_fs_appid <app_id>", "feishu app_id", feishu_set_app_id);
}

/**
 * @brief Set Feishu app_secret.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_fs_appsecret(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_fs_appsecret <app_secret>", "feishu app_secret", feishu_set_app_secret);
}

/**
 * @brief Set Feishu allow_from CSV.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_fs_allow(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_fs_allow <csv_allow_from>", "feishu allow_from", feishu_set_allow_from);
}

/**
 * @brief Set QQ Bot app_id.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_qq_appid(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_qq_appid <app_id>", "qq app_id", qqbot_set_app_id);
}

/**
 * @brief Set QQ Bot client_secret.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_qq_secret(int argc, char *argv[])
{
    cli_set_im_cfg_value_(argc, argv, "cfg_set_qq_secret <secret>", "qq client_secret", qqbot_set_client_secret);
}

/**
 * @brief Set outbound proxy configuration.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_set_proxy(int argc, char *argv[])
{
    const char *type;
    long        port;
    OPERATE_RET rt;

    if (argc < 3) {
        tal_cli_echo("Usage: cfg_set_proxy <host> <port> [type=http|https|socks5]");
        return;
    }

    port = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535) {
        tal_cli_echo("ERR: invalid port (1..65535)");
        return;
    }

    type = (argc >= 4) ? argv[3] : IM_SECRET_PROXY_TYPE;
    rt   = http_proxy_set(argv[1], (uint16_t)port, type);
    if (rt != OPRT_OK) {
        cli_echof_("ERR: cfg_set_proxy rt=%d", rt);
        return;
    }

    cli_echof_("OK: proxy=%s:%ld type=%s", argv[1], port, type);
}

/**
 * @brief Clear outbound proxy configuration.
 * @param[in] argc CLI argc
 * @param[in] argv CLI argv
 * @return none
 */
static void cmd_cfg_clear_proxy(int argc, char *argv[])
{
    OPERATE_RET rt;

    (void)argc;
    (void)argv;

    rt = http_proxy_clear();
    if (rt != OPRT_OK) {
        cli_echof_("ERR: cfg_clear_proxy rt=%d", rt);
        return;
    }

    tal_cli_echo("OK: proxy cleared");
}

/* ---------------------------------------------------------------------------
 * Command table
 * --------------------------------------------------------------------------- */
static cli_cmd_t s_cli_cmd[] = {
    {.name = "help",                  .help = "Show config CLI commands",               .func = cmd_help},
    {.name = "cfg_show",              .help = "Show effective config",                  .func = cmd_cfg_show},
    {.name = "cfg_reset",             .help = "Clear all config KV overrides",          .func = cmd_cfg_reset},
    {.name = "cfg_set_product_id",    .help = "Set Tuya product_id",                    .func = cmd_cfg_set_product_id},
    {.name = "cfg_set_auth",          .help = "Set Tuya uuid and authkey",              .func = cmd_cfg_set_auth},
    {.name = "cfg_set_ws_token",      .help = "Set WebSocket token",                    .func = cmd_cfg_set_ws_token},
    {.name = "cfg_set_gw_host",       .help = "Set OpenClaw gateway host",              .func = cmd_cfg_set_gw_host},
    {.name = "cfg_set_gw_port",       .help = "Set OpenClaw gateway port",              .func = cmd_cfg_set_gw_port},
    {.name = "cfg_set_gw_token",      .help = "Set OpenClaw gateway token",             .func = cmd_cfg_set_gw_token},
    {.name = "cfg_set_device_id",     .help = "Set device ID",                          .func = cmd_cfg_set_device_id},
    {.name = "cfg_set_channel_mode",  .help = "Set IM channel mode (OFF|telegram|discord|feishu|weixin)", .func = cmd_cfg_set_channel_mode},
    {.name = "cfg_set_tg_token",      .help = "Set Telegram token",                     .func = cmd_cfg_set_tg_token},
    {.name = "cfg_set_dc_token",      .help = "Set Discord token",                      .func = cmd_cfg_set_dc_token},
    {.name = "cfg_set_dc_channel",    .help = "Set Discord channel_id",                 .func = cmd_cfg_set_dc_channel},
    {.name = "cfg_set_fs_appid",      .help = "Set Feishu app_id",                      .func = cmd_cfg_set_fs_appid},
    {.name = "cfg_set_fs_appsecret",  .help = "Set Feishu app_secret",                  .func = cmd_cfg_set_fs_appsecret},
    {.name = "cfg_set_fs_allow",      .help = "Set Feishu allow_from CSV",              .func = cmd_cfg_set_fs_allow},
    {.name = "cfg_set_qq_appid",      .help = "Set QQ Bot app_id",                      .func = cmd_cfg_set_qq_appid},
    {.name = "cfg_set_qq_secret",     .help = "Set QQ Bot client_secret",               .func = cmd_cfg_set_qq_secret},
    {.name = "cfg_set_proxy",         .help = "Set outbound proxy",                     .func = cmd_cfg_set_proxy},
    {.name = "cfg_clear_proxy",       .help = "Clear outbound proxy",                   .func = cmd_cfg_clear_proxy},
};

/* ---------------------------------------------------------------------------
 * Public functions
 * --------------------------------------------------------------------------- */
/**
 * @brief Register all unified CLI commands.
 * @return none
 */
void app_cli_init(void)
{
    OPERATE_RET rt = tal_cli_cmd_register(s_cli_cmd, sizeof(s_cli_cmd) / sizeof(s_cli_cmd[0]));

    if (rt != OPRT_OK) {
        PR_ERR("tal_cli_cmd_register failed: %d", rt);
    }
    PR_DEBUG("app_cli_init: tal_cli_cmd_register success");
}

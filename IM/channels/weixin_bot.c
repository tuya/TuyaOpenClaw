/**
 * @file weixin_bot.c
 * @brief Weixin (WeChat) iLink Bot channel implementation.
 *
 * Protocol: JSON over HTTPS (POST for API, GET for QR endpoints).
 * Auth    : Bearer token in Authorization header; no OAuth flow after login.
 * Login   : One-time QR scan — URL is printed to serial log for PC browser.
 *
 * @version 1.0
 * @date 2026-03-31
 * @copyright Copyright (c) Tuya Inc.
 */
#include "channels/weixin_bot.h"

#include "bus/message_bus.h"
#include "cJSON.h"
#include "certs/tls_cert_bundle.h"
#include "http_client_interface.h"
#include "im_config.h"
#include "im_utils.h"
#include "mbedtls/base64.h"

#include <inttypes.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

static const char *TAG = "weixin";

#define WX_API_HOST             IM_WX_API_HOST
#define WX_API_PORT             443
#define WX_LONG_POLL_TIMEOUT_MS ((IM_WX_POLL_TIMEOUT_S + 5) * 1000)
#define WX_API_TIMEOUT_MS       15000
#define WX_QR_POLL_TIMEOUT_MS   ((IM_WX_QR_POLL_TIMEOUT_S + 5) * 1000)

#define WX_RESP_BUF_SIZE        (16 * 1024)
#define WX_SMALL_BUF_SIZE       (2 * 1024)
#define WX_QR_URL_SIZE          512

#define WX_SESSION_EXPIRED_CODE (-14)
#define WX_MSG_TYPE_TEXT        1
#define WX_MSG_TYPE_VOICE       3
#define WX_MSG_TYPE_BOT         2
#define WX_MSG_STATE_FINISH     2

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */

static char          s_bot_token[256]        = {0};
static char          s_base_host[128]        = {0};
static char          s_allow_from[96]        = {0};
static char          s_get_updates_buf[4096] = {0};
static char          s_context_token[512]    = {0};

static THREAD_HANDLE s_poll_thread           = NULL;
static THREAD_HANDLE s_qr_thread             = NULL;

static uint8_t      *s_wx_cacert             = NULL;
static size_t        s_wx_cacert_len         = 0;
static bool          s_wx_tls_no_verify      = false;
static char          s_wx_cert_host[128]     = {0};

static volatile bool     s_session_paused      = false;
static volatile uint32_t s_session_pause_start = 0;

static uint32_t      s_fail_delay_ms         = IM_WX_FAIL_BASE_MS;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */

static void weixin_poll_task(void *arg);
static void weixin_qr_login_task(void *arg);
static const char *effective_host(void);
/* ---------------------------------------------------------------------------
 * Utility helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Build X-WECHAT-UIN header value: base64(decimal_str(tick)).
 * @param[out] out      output buffer
 * @param[in]  out_size output buffer size (≥ 20 bytes)
 * @return none
 */
static void weixin_make_uin(char *out, size_t out_size)
{
    uint32_t val = tal_system_get_millisecond();
    char     dec[12] = {0};
    size_t   dec_len;
    size_t   b64_len = 0;

    snprintf(dec, sizeof(dec), "%" PRIu32, val);
    dec_len = strlen(dec);
    mbedtls_base64_encode((uint8_t *)out, out_size, &b64_len,
                          (const uint8_t *)dec, dec_len);
    if (b64_len < out_size) {
        out[b64_len] = '\0';
    } else {
        out[out_size - 1] = '\0';
    }
}

/**
 * @brief Ensure TLS certificate for the current effective API host is loaded.
 *
 * Invalidates the cached certificate when effective_host() differs from the
 * host the certificate was originally loaded for, so that a server-provided
 * custom host always gets its own certificate lookup.
 *
 * @return OPRT_OK always (falls back to no-verify on failure)
 */
static OPERATE_RET ensure_wx_cert(void)
{
    const char *host = effective_host();

    /* Cache hit: cert already loaded for the current host */
    if (s_wx_cacert && s_wx_cacert_len > 0 && strcmp(s_wx_cert_host, host) == 0) {
        return OPRT_OK;
    }

    /* Release stale cert when the host has changed */
    if (s_wx_cacert) {
        im_free(s_wx_cacert);
        s_wx_cacert     = NULL;
        s_wx_cacert_len = 0;
    }
    s_wx_cert_host[0] = '\0';

    OPERATE_RET rt = im_tls_query_domain_certs(host, &s_wx_cacert, &s_wx_cacert_len);
    if (rt != OPRT_OK || !s_wx_cacert || s_wx_cacert_len == 0) {
        if (s_wx_cacert) {
            im_free(s_wx_cacert);
        }
        s_wx_cacert        = NULL;
        s_wx_cacert_len    = 0;
        s_wx_tls_no_verify = true;
        IM_LOGD(TAG, "cert unavailable for %s, TLS no-verify mode", host);
        return OPRT_OK;
    }

    im_safe_copy(s_wx_cert_host, sizeof(s_wx_cert_host), host);
    s_wx_tls_no_verify = false;
    return OPRT_OK;
}

/**
 * @brief Percent-encode a string for safe inclusion in a URL query parameter.
 *
 * Only unreserved characters (RFC 3986) are left as-is; all others are
 * encoded as %XX.  The output is always null-terminated.
 *
 * @param[in]  src       source string to encode
 * @param[out] dst       output buffer
 * @param[in]  dst_size  size of dst (including space for '\0')
 * @return number of bytes written (excluding null terminator)
 */
static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t written = 0;

    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0) {
            dst[0] = '\0';
        }
        return 0;
    }

    for (const char *p = src; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';

        if (unreserved) {
            if (written + 2 > dst_size) {
                break;
            }
            dst[written++] = (char)c;
        } else {
            if (written + 4 > dst_size) {
                break;
            }
            dst[written++] = '%';
            dst[written++] = hex[(c >> 4) & 0x0F];
            dst[written++] = hex[c & 0x0F];
        }
    }
    dst[written] = '\0';
    return written;
}

/**
 * @brief Return the effective API hostname (stored or default).
 * @return const char* to hostname (no scheme, no port)
 */
static const char *effective_host(void)
{
    if (s_base_host[0] != '\0') {
        return s_base_host;
    }
    return WX_API_HOST;
}

/* ---------------------------------------------------------------------------
 * HTTP helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Execute a POST JSON API call to the Weixin iLink server.
 *
 * Sets Authorization, AuthorizationType, X-WECHAT-UIN, Content-Type headers.
 *
 * @param[in]  path          URL path (e.g. "/ilink/bot/getupdates")
 * @param[in]  json_body     Request body JSON string
 * @param[out] resp_buf      Response body output buffer
 * @param[in]  resp_buf_size Size of resp_buf
 * @param[out] status_code   HTTP status code (may be NULL)
 * @param[in]  timeout_ms    Request timeout in milliseconds
 * @return OPRT_OK on success
 */
static OPERATE_RET weixin_api_post(const char *path, const char *json_body,
                                   char *resp_buf, size_t resp_buf_size,
                                   uint16_t *status_code, uint32_t timeout_ms)
{
    if (!path || !json_body || !resp_buf || resp_buf_size == 0) {
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = ensure_wx_cert();
    if (rt != OPRT_OK) {
        return rt;
    }

    char auth_buf[280]  = {0};
    char uin_buf[32]    = {0};
    weixin_make_uin(uin_buf, sizeof(uin_buf));
    snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", s_bot_token);

    http_client_header_t headers[] = {
        {"Content-Type",       "application/json"},
        {"AuthorizationType",  "ilink_bot_token"},
        {"Authorization",      auth_buf},
        {"X-WECHAT-UIN",       uin_buf},
    };

    http_client_response_t response = {0};
    http_client_status_t   http_rt  = http_client_request(
        &(const http_client_request_t){
            .cacert        = s_wx_cacert,
            .cacert_len    = s_wx_cacert_len,
            .tls_no_verify = s_wx_tls_no_verify,
            .host          = effective_host(),
            .port          = WX_API_PORT,
            .method        = "POST",
            .path          = path,
            .headers       = headers,
            .headers_count = 4,
            .body          = (const uint8_t *)json_body,
            .body_length   = strlen(json_body),
            .timeout_ms    = timeout_ms,
        },
        &response);

    if (http_rt != HTTP_CLIENT_SUCCESS) {
        IM_LOGE(TAG, "api_post %s failed http_rt=%d", path, http_rt);
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }

    if (status_code) {
        *status_code = response.status_code;
    }

    resp_buf[0] = '\0';
    if (response.body && response.body_length > 0) {
        size_t copy = (response.body_length < resp_buf_size - 1)
                          ? response.body_length
                          : (resp_buf_size - 1);
        memcpy(resp_buf, response.body, copy);
        resp_buf[copy] = '\0';
    }

    http_client_free(&response);
    return OPRT_OK;
}

/**
 * @brief Execute a GET request to the Weixin iLink server (QR endpoints).
 *
 * @param[in]  path          URL path with query string
 * @param[in]  extra_hdr_key   Additional header key (may be NULL)
 * @param[in]  extra_hdr_val   Additional header value (may be NULL)
 * @param[out] resp_buf      Response body output buffer
 * @param[in]  resp_buf_size Size of resp_buf
 * @param[out] status_code   HTTP status code (may be NULL)
 * @param[in]  timeout_ms    Request timeout in milliseconds
 * @return OPRT_OK on success
 */
static OPERATE_RET weixin_api_get(const char *path,
                                  const char *extra_hdr_key, const char *extra_hdr_val,
                                  char *resp_buf, size_t resp_buf_size,
                                  uint16_t *status_code, uint32_t timeout_ms)
{
    if (!path || !resp_buf || resp_buf_size == 0) {
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = ensure_wx_cert();
    if (rt != OPRT_OK) {
        return rt;
    }

    http_client_header_t headers[2] = {0};
    uint8_t              hdr_cnt    = 0;

    if (extra_hdr_key && extra_hdr_val) {
        headers[hdr_cnt].key   = extra_hdr_key;
        headers[hdr_cnt].value = extra_hdr_val;
        hdr_cnt++;
    }

    http_client_response_t response = {0};
    http_client_status_t   http_rt  = http_client_request(
        &(const http_client_request_t){
            .cacert        = s_wx_cacert,
            .cacert_len    = s_wx_cacert_len,
            .tls_no_verify = s_wx_tls_no_verify,
            .host          = effective_host(),
            .port          = WX_API_PORT,
            .method        = "GET",
            .path          = path,
            .headers       = hdr_cnt > 0 ? headers : NULL,
            .headers_count = hdr_cnt,
            .body          = NULL,
            .body_length   = 0,
            .timeout_ms    = timeout_ms,
        },
        &response);

    if (http_rt != HTTP_CLIENT_SUCCESS) {
        IM_LOGE(TAG, "api_get %s failed http_rt=%d", path, http_rt);
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }

    if (status_code) {
        *status_code = response.status_code;
    }

    resp_buf[0] = '\0';
    if (response.body && response.body_length > 0) {
        size_t copy = (response.body_length < resp_buf_size - 1)
                          ? response.body_length
                          : (resp_buf_size - 1);
        memcpy(resp_buf, response.body, copy);
        resp_buf[copy] = '\0';
    }

    http_client_free(&response);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Context token helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Persist context_token to KV and update the in-memory cache.
 * @param[in] token  context_token string
 * @return none
 */
static void save_context_token(const char *token)
{
    if (!token || token[0] == '\0') {
        return;
    }
    im_safe_copy(s_context_token, sizeof(s_context_token), token);
    im_kv_set_string(IM_NVS_WX, IM_NVS_KEY_WX_CTX_TOK, token);
}

/* ---------------------------------------------------------------------------
 * Session pause helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Mark the session as paused for IM_WX_SESSION_PAUSE_MS milliseconds.
 * @return none
 */
static void session_pause(void)
{
    s_session_paused      = true;
    s_session_pause_start = tal_system_get_millisecond();
    IM_LOGW(TAG, "session expired (errcode=%d), pausing %d min",
            WX_SESSION_EXPIRED_CODE, IM_WX_SESSION_PAUSE_MS / 60000);
}

/**
 * @brief Check whether the session is still within its cooldown window.
 * @return true while paused, false when the window has expired
 */
static bool session_is_paused(void)
{
    if (!s_session_paused) {
        return false;
    }
    uint32_t elapsed = tal_system_get_millisecond() - s_session_pause_start;
    if (elapsed >= IM_WX_SESSION_PAUSE_MS) {
        s_session_paused = false;
        IM_LOGI(TAG, "session cooldown expired, resuming");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Message parsing
 * --------------------------------------------------------------------------- */

/**
 * @brief Extract plain-text body from a WeixinMessage item_list array.
 *
 * Priority: TEXT item (type=1) > VOICE item with STT text (type=3).
 *
 * @param[in]  item_list  cJSON array node
 * @param[out] out        output buffer
 * @param[in]  out_size   output buffer size
 * @return none
 */
static void extract_text_body(cJSON *item_list, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!item_list || !cJSON_IsArray(item_list)) {
        return;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, item_list) {
        int type = im_json_int(item, "type", 0);

        if (type == WX_MSG_TYPE_TEXT) {
            cJSON *text_item = cJSON_GetObjectItem(item, "text_item");
            cJSON *text      = text_item ? cJSON_GetObjectItem(text_item, "text") : NULL;
            if (cJSON_IsString(text) && text->valuestring && text->valuestring[0] != '\0') {
                im_safe_copy(out, out_size, text->valuestring);
                return;
            }
        }

        if (type == WX_MSG_TYPE_VOICE) {
            cJSON *voice_item = cJSON_GetObjectItem(item, "voice_item");
            cJSON *stt        = voice_item ? cJSON_GetObjectItem(voice_item, "text") : NULL;
            if (cJSON_IsString(stt) && stt->valuestring && stt->valuestring[0] != '\0') {
                im_safe_copy(out, out_size, stt->valuestring);
                return;
            }
        }
    }
}

/**
 * @brief Process parsed getupdates response: push each valid message to the bus.
 *
 * @param[in] root  Parsed getupdates JSON root object
 * @return none
 */
static void process_updates(cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) {
        return;
    }

    /* Update get_updates_buf if server returned a new one */
    cJSON *new_buf = cJSON_GetObjectItem(root, "get_updates_buf");
    if (cJSON_IsString(new_buf) && new_buf->valuestring && new_buf->valuestring[0] != '\0') {
        im_safe_copy(s_get_updates_buf, sizeof(s_get_updates_buf), new_buf->valuestring);
        im_kv_set_string(IM_NVS_WX, IM_NVS_KEY_WX_UPD_BUF, s_get_updates_buf);
    }

    cJSON *msgs = cJSON_GetObjectItem(root, "msgs");
    if (!cJSON_IsArray(msgs)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *msg = NULL;
    cJSON_ArrayForEach(msg, msgs) {
        cJSON *from_node = cJSON_GetObjectItem(msg, "from_user_id");
        if (!cJSON_IsString(from_node) || !from_node->valuestring ||
            from_node->valuestring[0] == '\0') {
            continue;
        }

        const char *from_user = from_node->valuestring;

        /* allowFrom authorization: fail-closed — reject all messages when no
         * allow_from is configured, and reject messages from non-matching users
         * when one is configured.  This prevents a token-only provisioned device
         * from accepting commands from arbitrary senders. */
        if (s_allow_from[0] == '\0' || strcmp(s_allow_from, from_user) != 0) {
            IM_LOGD(TAG, "drop msg from unauthorised user=%s (allow_from=%s)",
                    from_user, s_allow_from[0] ? s_allow_from : "<unset>");
            continue;
        }

        /* Persist context_token for reply */
        cJSON *ctx_tok_node = cJSON_GetObjectItem(msg, "context_token");
        if (cJSON_IsString(ctx_tok_node) && ctx_tok_node->valuestring &&
            ctx_tok_node->valuestring[0] != '\0') {
            save_context_token(ctx_tok_node->valuestring);
        }

        /* Extract text body */
        char text_body[IM_WX_MAX_MSG_LEN + 1] = {0};
        extract_text_body(cJSON_GetObjectItem(msg, "item_list"), text_body, sizeof(text_body));

        IM_LOGI(TAG, "inbound from=%s body_len=%u", from_user, (unsigned)strlen(text_body));

        /* Push to message bus */
        im_msg_t bus_msg = {0};
        strncpy(bus_msg.channel, IM_CHAN_WEIXIN, sizeof(bus_msg.channel) - 1);
        strncpy(bus_msg.chat_id, from_user, sizeof(bus_msg.chat_id) - 1);
        bus_msg.content = im_strdup(text_body);
        if (!bus_msg.content) {
            IM_LOGE(TAG, "strdup failed for message content");
            continue;
        }

        OPERATE_RET rt = message_bus_push_inbound(&bus_msg);
        if (rt != OPRT_OK) {
            IM_LOGW(TAG, "push inbound failed rt=%d", rt);
            im_free(bus_msg.content);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Long-poll task
 * --------------------------------------------------------------------------- */

/**
 * @brief Long-poll getupdates task. Runs until device resets.
 * @param[in] arg unused
 * @return none
 */
static void weixin_poll_task(void *arg)
{
    (void)arg;
    char *resp = im_malloc(WX_RESP_BUF_SIZE);
    if (!resp) {
        IM_LOGE(TAG, "alloc poll resp buf failed");
        return;
    }

    IM_LOGI(TAG, "poll task started host=%s", effective_host());

    while (1) {
        /* Session cooldown after errcode -14 */
        if (session_is_paused()) {
            tal_system_sleep(5000);
            continue;
        }

        if (s_bot_token[0] == '\0') {
            tal_system_sleep(3000);
            continue;
        }

        /* Build getupdates request body */
        cJSON *body = cJSON_CreateObject();
        if (!body) {
            tal_system_sleep(IM_WX_FAIL_BASE_MS);
            continue;
        }
        cJSON_AddStringToObject(body, "get_updates_buf", s_get_updates_buf);
        cJSON *base_info = cJSON_AddObjectToObject(body, "base_info");
        if (base_info) {
            cJSON_AddStringToObject(base_info, "channel_version", IM_WX_CHANNEL_VERSION);
        }
        char *json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (!json) {
            tal_system_sleep(IM_WX_FAIL_BASE_MS);
            continue;
        }

        memset(resp, 0, WX_RESP_BUF_SIZE);
        uint16_t    status = 0;
        OPERATE_RET rt     = weixin_api_post("/ilink/bot/getupdates", json,
                                             resp, WX_RESP_BUF_SIZE,
                                             &status, WX_LONG_POLL_TIMEOUT_MS);
        cJSON_free(json);

        if (rt != OPRT_OK || status != 200) {
            IM_LOGD(TAG, "getupdates failed rt=%d http=%u retry=%ums",
                    rt, status, s_fail_delay_ms);
            tal_system_sleep(s_fail_delay_ms);
            if (s_fail_delay_ms < IM_WX_FAIL_MAX_MS) {
                uint32_t next = s_fail_delay_ms << 1;
                s_fail_delay_ms = (next > IM_WX_FAIL_MAX_MS) ? IM_WX_FAIL_MAX_MS : next;
            }
            continue;
        }

        /* Parse once and reuse the same JSON tree for status checks and updates. */
        cJSON *root    = cJSON_Parse(resp);
        int    api_ret = root ? im_json_int(root, "ret", 0)     : -1;
        int    errcode = root ? im_json_int(root, "errcode", 0) : -1;

        if (api_ret != 0 || errcode != 0) {
            if (errcode == WX_SESSION_EXPIRED_CODE || api_ret == WX_SESSION_EXPIRED_CODE) {
                session_pause();
                s_fail_delay_ms = IM_WX_FAIL_BASE_MS;
            } else {
                IM_LOGW(TAG, "getupdates api error ret=%d errcode=%d", api_ret, errcode);
                tal_system_sleep(s_fail_delay_ms);
                if (s_fail_delay_ms < IM_WX_FAIL_MAX_MS) {
                    uint32_t next = s_fail_delay_ms << 1;
                    s_fail_delay_ms = (next > IM_WX_FAIL_MAX_MS) ? IM_WX_FAIL_MAX_MS : next;
                }
            }
            if (root) {
                cJSON_Delete(root);
            }
            continue;
        }

        s_fail_delay_ms = IM_WX_FAIL_BASE_MS;
        process_updates(root);
        cJSON_Delete(root);
    }
}

/* ---------------------------------------------------------------------------
 * QR login task
 * --------------------------------------------------------------------------- */

/**
 * @brief QR login task. Prints scan URL, waits for confirmation, then
 *        saves credentials and spawns the poll task.
 * @param[in] arg unused
 * @return none
 */
static void weixin_qr_login_task(void *arg)
{
    (void)arg;

    char *resp        = im_malloc(WX_SMALL_BUF_SIZE);
    char *qr_url_buf  = im_malloc(WX_QR_URL_SIZE);
    char  qrcode[256] = {0};

    if (!resp || !qr_url_buf) {
        IM_LOGE(TAG, "QR login: alloc failed");
        goto cleanup;
    }

    IM_LOGI(TAG, "QR login task started");

    /* ---- Step 1: Get QR code ---- */
    {
        char path[64] = {0};
        snprintf(path, sizeof(path), "/ilink/bot/get_bot_qrcode?bot_type=%s",
                 IM_WX_BOT_TYPE);

        uint16_t status = 0;
        OPERATE_RET rt = weixin_api_get(path, NULL, NULL,
                                        resp, WX_SMALL_BUF_SIZE,
                                        &status, WX_API_TIMEOUT_MS);
        if (rt != OPRT_OK || status != 200) {
            IM_LOGE(TAG, "get_bot_qrcode failed rt=%d http=%u", rt, status);
            goto cleanup;
        }

        cJSON *root = cJSON_Parse(resp);
        if (!root) {
            IM_LOGE(TAG, "get_bot_qrcode: JSON parse failed");
            goto cleanup;
        }

        cJSON *qrcode_node = cJSON_GetObjectItem(root, "qrcode");
        cJSON *url_node    = cJSON_GetObjectItem(root, "qrcode_img_content");

        if (!cJSON_IsString(qrcode_node) || !cJSON_IsString(url_node)) {
            IM_LOGE(TAG, "get_bot_qrcode: missing qrcode or qrcode_img_content");
            cJSON_Delete(root);
            goto cleanup;
        }

        im_safe_copy(qrcode, sizeof(qrcode), qrcode_node->valuestring);
        im_safe_copy(qr_url_buf, WX_QR_URL_SIZE, url_node->valuestring);
        cJSON_Delete(root);
    }

    /* ---- Step 2: Print URL prominently ---- */
    PR_INFO("=========================================================");
    PR_INFO("  Weixin QR Login");
    PR_INFO("  Open this URL in your PC browser, then scan with WeChat:");
    PR_INFO("  %s", qr_url_buf);
    PR_INFO("=========================================================");

    /* ---- Step 3: Long-poll for scan status ---- */
    {
        uint32_t login_deadline = tal_system_get_millisecond() + IM_WX_LOGIN_TIMEOUT_MS;
        int      refresh_count  = 0;
        bool     scanned        = false;

        /* Unsigned wrap-safe timeout check: still true until now reaches login_deadline. */
        while ((uint32_t)(tal_system_get_millisecond() - login_deadline + IM_WX_LOGIN_TIMEOUT_MS)
               < IM_WX_LOGIN_TIMEOUT_MS) {

            /* Encoded qrcode can be up to 3× the raw length; allocate path on heap */
            size_t qrcode_enc_size = strlen(qrcode) * 3 + 1;
            char *qrcode_enc = im_malloc(qrcode_enc_size);
            if (!qrcode_enc) {
                IM_LOGE(TAG, "get_qrcode_status: alloc enc buf failed");
                tal_system_sleep(2000);
                continue;
            }
            url_encode(qrcode, qrcode_enc, qrcode_enc_size);

            /* "/ilink/bot/get_qrcode_status?qrcode=" = 37 chars */
            size_t path_size = 37 + qrcode_enc_size + 1;
            char *path = im_malloc(path_size);
            if (!path) {
                IM_LOGE(TAG, "get_qrcode_status: alloc path buf failed");
                im_free(qrcode_enc);
                tal_system_sleep(2000);
                continue;
            }
            snprintf(path, path_size, "/ilink/bot/get_qrcode_status?qrcode=%s", qrcode_enc);
            im_free(qrcode_enc);

            uint16_t status = 0;
            OPERATE_RET rt = weixin_api_get(path,
                                            "iLink-App-ClientVersion", "1",
                                            resp, WX_SMALL_BUF_SIZE,
                                            &status, WX_QR_POLL_TIMEOUT_MS);
            im_free(path);

            if (rt != OPRT_OK || status != 200) {
                IM_LOGW(TAG, "get_qrcode_status failed rt=%d http=%u, retry", rt, status);
                tal_system_sleep(2000);
                continue;
            }

            cJSON *root = cJSON_Parse(resp);
            if (!root) {
                tal_system_sleep(1000);
                continue;
            }

            const char *scan_status = im_json_str(root, "status", "wait");

            if (strcmp(scan_status, "wait") == 0) {
                /* Server held the poll; normal — no sleep needed */

            } else if (strcmp(scan_status, "scaned") == 0) {
                if (!scanned) {
                    scanned = true;
                    PR_INFO("[weixin] QR scanned — confirm in your WeChat app");
                }

            } else if (strcmp(scan_status, "expired") == 0) {
                refresh_count++;
                if (refresh_count > IM_WX_QR_MAX_REFRESH) {
                    IM_LOGE(TAG, "QR expired %d times, giving up", IM_WX_QR_MAX_REFRESH);
                    cJSON_Delete(root);
                    goto cleanup;
                }

                PR_INFO("[weixin] QR expired, refreshing (%d/%d)...",
                        refresh_count, IM_WX_QR_MAX_REFRESH);
                cJSON_Delete(root);

                /* Fetch new QR code */
                char qr_path[64] = {0};
                snprintf(qr_path, sizeof(qr_path),
                         "/ilink/bot/get_bot_qrcode?bot_type=%s", IM_WX_BOT_TYPE);

                uint16_t    qr_st = 0;
                OPERATE_RET qr_rt = weixin_api_get(qr_path, NULL, NULL,
                                                   resp, WX_SMALL_BUF_SIZE,
                                                   &qr_st, WX_API_TIMEOUT_MS);
                if (qr_rt != OPRT_OK || qr_st != 200) {
                    IM_LOGE(TAG, "QR refresh failed rt=%d http=%u", qr_rt, qr_st);
                    goto cleanup;
                }

                cJSON *qr2 = cJSON_Parse(resp);
                if (!qr2) {
                    goto cleanup;
                }
                cJSON *qrcode2_node = cJSON_GetObjectItem(qr2, "qrcode");
                cJSON *url2_node    = cJSON_GetObjectItem(qr2, "qrcode_img_content");
                if (cJSON_IsString(qrcode2_node) && cJSON_IsString(url2_node)) {
                    im_safe_copy(qrcode, sizeof(qrcode), qrcode2_node->valuestring);
                    im_safe_copy(qr_url_buf, WX_QR_URL_SIZE, url2_node->valuestring);
                    scanned = false;
                    PR_INFO("[weixin] New QR URL: %s", qr_url_buf);
                }
                cJSON_Delete(qr2);
                continue;

            } else if (strcmp(scan_status, "confirmed") == 0) {
                const char *token   = im_json_str(root, "bot_token", "");
                const char *bot_id  = im_json_str(root, "ilink_bot_id", "");
                const char *user_id = im_json_str(root, "ilink_user_id", "");
                const char *baseurl = im_json_str(root, "baseurl", "");

                if (token[0] == '\0' || bot_id[0] == '\0') {
                    IM_LOGE(TAG, "confirmed but missing bot_token or ilink_bot_id");
                    cJSON_Delete(root);
                    goto cleanup;
                }

                PR_INFO("[weixin] ✅ Login success! bot_id=%s user_id=%s", bot_id, user_id);

                /* Persist credentials */
                im_safe_copy(s_bot_token, sizeof(s_bot_token), token);
                im_kv_set_string(IM_NVS_WX, IM_NVS_KEY_WX_TOKEN, token);

                if (baseurl[0] != '\0') {
                    /* Extract only the hostname from baseurl (strip https:// if present) */
                    const char *host_start = baseurl;
                    if (strncmp(host_start, "https://", 8) == 0) {
                        host_start += 8;
                    } else if (strncmp(host_start, "http://", 7) == 0) {
                        host_start += 7;
                    }
                    /* Strip path component */
                    char host_only[128] = {0};
                    const char *slash = strchr(host_start, '/');
                    if (slash) {
                        size_t hlen = (size_t)(slash - host_start);
                        if (hlen < sizeof(host_only)) {
                            memcpy(host_only, host_start, hlen);
                            host_only[hlen] = '\0';
                        }
                    } else {
                        im_safe_copy(host_only, sizeof(host_only), host_start);
                    }
                    if (host_only[0] != '\0') {
                        im_safe_copy(s_base_host, sizeof(s_base_host), host_only);
                        im_kv_set_string(IM_NVS_WX, IM_NVS_KEY_WX_HOST, host_only);
                    }
                }

                if (user_id[0] != '\0') {
                    im_safe_copy(s_allow_from, sizeof(s_allow_from), user_id);
                    im_kv_set_string(IM_NVS_WX, IM_NVS_KEY_WX_ALLOW, user_id);
                }

                cJSON_Delete(root);

                /* Spawn poll task */
                THREAD_CFG_T cfg = {0};
                cfg.stackDepth   = IM_WX_POLL_STACK;
                cfg.priority     = THREAD_PRIO_1;
                cfg.thrdname     = "im_wx_poll";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
                cfg.psram_mode = 1;
#endif
                tal_thread_create_and_start(&s_poll_thread, NULL, NULL,
                                            weixin_poll_task, NULL, &cfg);
                goto cleanup;
            }

            cJSON_Delete(root);
            tal_system_sleep(1000);
        }

        PR_WARN("[weixin] QR login timed out (%d ms)", IM_WX_LOGIN_TIMEOUT_MS);
    }

cleanup:
    im_free(resp);
    im_free(qr_url_buf);
    s_qr_thread = NULL;
    IM_LOGI(TAG, "QR login task exited");
}

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialize Weixin bot (load credentials from KV storage).
 * @return OPRT_OK on success
 */
OPERATE_RET weixin_bot_init(void)
{
    /* Load token */
    if (IM_SECRET_WX_TOKEN[0] != '\0') {
        im_safe_copy(s_bot_token, sizeof(s_bot_token), IM_SECRET_WX_TOKEN);
    }
    char tmp[256] = {0};
    if (im_kv_get_string(IM_NVS_WX, IM_NVS_KEY_WX_TOKEN, tmp, sizeof(tmp)) == OPRT_OK &&
        tmp[0] != '\0') {
        im_safe_copy(s_bot_token, sizeof(s_bot_token), tmp);
    }

    /* Load custom API host */
    memset(tmp, 0, sizeof(tmp));
    if (im_kv_get_string(IM_NVS_WX, IM_NVS_KEY_WX_HOST, tmp, sizeof(tmp)) == OPRT_OK &&
        tmp[0] != '\0') {
        im_safe_copy(s_base_host, sizeof(s_base_host), tmp);
    }

    /* Load allow_from */
    if (IM_SECRET_WX_ALLOW_FROM[0] != '\0') {
        im_safe_copy(s_allow_from, sizeof(s_allow_from), IM_SECRET_WX_ALLOW_FROM);
    }
    memset(tmp, 0, sizeof(tmp));
    if (im_kv_get_string(IM_NVS_WX, IM_NVS_KEY_WX_ALLOW, tmp, sizeof(tmp)) == OPRT_OK &&
        tmp[0] != '\0') {
        im_safe_copy(s_allow_from, sizeof(s_allow_from), tmp);
    }

    /* Load get_updates_buf */
    memset(tmp, 0, sizeof(tmp));
    if (im_kv_get_string(IM_NVS_WX, IM_NVS_KEY_WX_UPD_BUF,
                         s_get_updates_buf, sizeof(s_get_updates_buf)) == OPRT_OK) {
        IM_LOGI(TAG, "restored get_updates_buf len=%u",
                (unsigned)strlen(s_get_updates_buf));
    }

    /* Load context_token */
    if (im_kv_get_string(IM_NVS_WX, IM_NVS_KEY_WX_CTX_TOK,
                         s_context_token, sizeof(s_context_token)) == OPRT_OK &&
        s_context_token[0] != '\0') {
        IM_LOGI(TAG, "restored context_token");
    }

    IM_LOGI(TAG, "init done token=%s allow_from=%s",
            s_bot_token[0] ? "set" : "empty",
            s_allow_from[0] ? s_allow_from : "empty");
    return OPRT_OK;
}

/**
 * @brief Start Weixin bot: poll if token present, QR login otherwise.
 * @return OPRT_OK on success
 */
OPERATE_RET weixin_bot_start(void)
{
    if (s_poll_thread) {
        return OPRT_OK;
    }

    THREAD_CFG_T cfg = {0};
    cfg.priority     = THREAD_PRIO_1;

    if (s_bot_token[0] != '\0') {
        /* Token available — start poll immediately */
        cfg.stackDepth = IM_WX_POLL_STACK;
        cfg.thrdname   = "im_wx_poll";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
        cfg.psram_mode = 1;
#endif
        OPERATE_RET rt = tal_thread_create_and_start(&s_poll_thread, NULL, NULL,
                                                     weixin_poll_task, NULL, &cfg);
        if (rt != OPRT_OK) {
            IM_LOGE(TAG, "create poll thread failed rt=%d", rt);
            return rt;
        }
    } else {
        /* No token — start QR login task */
        if (s_qr_thread) {
            return OPRT_OK;
        }
        cfg.stackDepth = IM_WX_QR_STACK;
        cfg.thrdname   = "im_wx_qr";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
        cfg.psram_mode = 1;
#endif
        OPERATE_RET rt = tal_thread_create_and_start(&s_qr_thread, NULL, NULL,
                                                     weixin_qr_login_task, NULL, &cfg);
        if (rt != OPRT_OK) {
            IM_LOGE(TAG, "create QR login thread failed rt=%d", rt);
            return rt;
        }
        PR_INFO("[weixin] No token — QR login task started. Check serial log for scan URL.");
    }

    return OPRT_OK;
}

/**
 * @brief Send a text message to a Weixin user via sendmessage API.
 * @param[in] user_id  Target iLink user ID
 * @param[in] text     UTF-8 message text
 * @return OPRT_OK on success
 */
OPERATE_RET weixin_send_message(const char *user_id, const char *text)
{
    if (!user_id || !text) {
        return OPRT_INVALID_PARM;
    }
    if (s_bot_token[0] == '\0') {
        IM_LOGW(TAG, "send_message: no token");
        return OPRT_NOT_FOUND;
    }

    /* Generate client_id */
    char     client_id[64] = {0};
    uint64_t now_ms        = (uint64_t)tal_system_get_millisecond();

    snprintf(client_id, sizeof(client_id), "wx-%016" PRIx64 "-%04x",
             now_ms,
             (unsigned int)(tal_system_get_random(0xFFFF)));

    /* Build item_list */
    cJSON *root    = cJSON_CreateObject();
    cJSON *msg_obj = cJSON_AddObjectToObject(root, "msg");
    cJSON_AddStringToObject(msg_obj, "from_user_id",  "");
    cJSON_AddStringToObject(msg_obj, "to_user_id",    user_id);
    cJSON_AddStringToObject(msg_obj, "client_id",     client_id);
    cJSON_AddNumberToObject(msg_obj, "message_type",  WX_MSG_TYPE_BOT);
    cJSON_AddNumberToObject(msg_obj, "message_state", WX_MSG_STATE_FINISH);
    if (s_context_token[0] != '\0') {
        cJSON_AddStringToObject(msg_obj, "context_token", s_context_token);
    }
    cJSON *item_list = cJSON_AddArrayToObject(msg_obj, "item_list");
    cJSON *item      = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "type", WX_MSG_TYPE_TEXT);
    cJSON *text_item = cJSON_AddObjectToObject(item, "text_item");
    cJSON_AddStringToObject(text_item, "text", text);
    cJSON_AddItemToArray(item_list, item);

    cJSON *base_info = cJSON_AddObjectToObject(root, "base_info");
    cJSON_AddStringToObject(base_info, "channel_version", IM_WX_CHANNEL_VERSION);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return OPRT_MALLOC_FAILED;
    }

    char        resp[WX_SMALL_BUF_SIZE] = {0};
    uint16_t    status                  = 0;
    OPERATE_RET rt                      = weixin_api_post("/ilink/bot/sendmessage",
                                                          json, resp, sizeof(resp),
                                                          &status, WX_API_TIMEOUT_MS);
    cJSON_free(json);

    if (rt != OPRT_OK || status != 200) {
        IM_LOGE(TAG, "sendmessage failed rt=%d http=%u", rt, status);
        return OPRT_COM_ERROR;
    }

    IM_LOGD(TAG, "sendmessage OK to=%s", user_id);
    return OPRT_OK;
}

/**
 * @brief Persist a new bot_token and reset poll state.
 * @param[in] token  New bot_token string
 * @return OPRT_OK on success
 */
OPERATE_RET weixin_set_token(const char *token)
{
    if (!token) {
        return OPRT_INVALID_PARM;
    }
    im_safe_copy(s_bot_token, sizeof(s_bot_token), token);
    memset(s_get_updates_buf, 0, sizeof(s_get_updates_buf));
    memset(s_context_token,   0, sizeof(s_context_token));
    (void)im_kv_del(IM_NVS_WX, IM_NVS_KEY_WX_UPD_BUF);
    (void)im_kv_del(IM_NVS_WX, IM_NVS_KEY_WX_CTX_TOK);
    return im_kv_set_string(IM_NVS_WX, IM_NVS_KEY_WX_TOKEN, token);
}

/**
 * @brief Persist the authorised sender user_id (allow_from).
 * @param[in] user_id  Weixin iLink user ID to authorise
 * @return OPRT_OK on success
 */
OPERATE_RET weixin_set_allow_from(const char *user_id)
{
    if (!user_id) {
        return OPRT_INVALID_PARM;
    }
    im_safe_copy(s_allow_from, sizeof(s_allow_from), user_id);
    return im_kv_set_string(IM_NVS_WX, IM_NVS_KEY_WX_ALLOW, user_id);
}

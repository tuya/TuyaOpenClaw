/**
 * @file acp_client.c
 * @brief ACP (Agent Client Protocol) WebSocket client implementation.
 *
 * Implements a WebSocket client that speaks the OpenClaw ACP protocol
 * (@agentclientprotocol/sdk) to connect directly to an openclaw gateway
 * over a local-area-network WebSocket connection:
 *
 *   Phase 1 – TCP connect to OPENCLAW_GATEWAY_HOST:OPENCLAW_GATEWAY_PORT
 *   Phase 2 – RFC 6455 HTTP upgrade handshake
 *   Phase 3 – ACP "connect" request with client descriptor and auth token;
 *              wait for "hello-ok" event to extract sessionKey
 *   Phase 4 – Steady-state: send chat.send requests, then on demand receive
 *              chat events (delta / final) for one pending request window
 *
 * ACP frame types (JSON-over-WebSocket RPC):
 *   Request  : {"type":"req",   "id":"N",  "method":"...", "params":{...}}
 *   Response : {"type":"res",   "id":"N",  "ok":true/false, "payload":{...}}
 *   Event    : {"type":"event", "event":"...", "payload":{...}, "seq":N}
 *
 * Per RFC 6455 §5.1, frames sent from a client to a server MUST be masked.
 * This file uses a lightweight 4-byte masking key derived from the system
 * tick counter and a monotonic counter; cryptographic strength is not
 * required for the WS masking key.
 *
 * @version 2.0
 * @date 2026-03-19
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "acp_client.h"
#include "tool_files.h"
#include "tuya_app_config.h"
#include "app_base_config.h"
#include "app_im.h"
#include "sys_bus.h"

#include "cJSON.h"
#include "mix_method.h"
#include "tal_hash.h"
#include "tal_log.h"
#include "tal_api.h"
#include "tal_semaphore.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

#define ACP_WS_OPCODE_CONT    0x00
#define ACP_WS_OPCODE_TEXT    0x01
#define ACP_WS_OPCODE_BINARY  0x02
#define ACP_WS_OPCODE_CLOSE   0x08
#define ACP_WS_OPCODE_PING    0x09
#define ACP_WS_OPCODE_PONG    0x0A

/* ACP protocol version negotiation range – gateway requires exactly v3 */
#define ACP_PROTO_MIN         3
#define ACP_PROTO_MAX         3

/* Fixed request ID for the ACP connect handshake */
#define ACP_CONNECT_REQ_ID    "1"

/* Maximum ms to wait for a single recv before looping */
#define ACP_SELECT_TIMEOUT_MS 200
/* Maximum ms to wait for a synchronous connect/upgrade response */
#define ACP_SYNC_RECV_TIMEOUT_MS  10000
/* Maximum time to wait for one async chat.send final reply */
#define ACP_ASYNC_REPLY_TIMEOUT_MS  (180 * 1000)

#ifndef ACP_CLIENT_WS_MSG_BUF_SIZE
#define ACP_CLIENT_WS_MSG_BUF_SIZE (32 * 1024)
#endif

#define ACP_LOG_CHUNK_LEN     192

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */

typedef enum {
    ACP_STATE_DISCONNECTED = 0,
    ACP_STATE_CONNECTED,
} acp_state_e;

typedef struct {
    THREAD_HANDLE  thread;
    MUTEX_HANDLE   tx_mutex;

    int            fd;
    acp_state_e    state;

    /* sessionKey obtained from hello-ok event; used in chat.send params */
    char           session_key[128];
    uint32_t       req_id_counter;

    /* PSRAM-allocated at init time. Previously inline arrays totaling ~48 KiB
     * of internal SRAM .bss — moved out so BIND-stage softAP has heap. */
    uint8_t       *rx_buf;        /* ACP_CLIENT_RX_BUF_SIZE bytes */
    size_t         rx_len;

    char          *ws_msg_buf;    /* ACP_CLIENT_WS_MSG_BUF_SIZE bytes */
    size_t         ws_msg_len;
    bool         ws_msg_active;

    /* Accumulates streaming chat content until done=true / state=final */
    char          *reply_buf;     /* ACP_CLIENT_REPLY_BUF_SIZE bytes */
    size_t         reply_len;

    acp_reply_cb_t reply_cb;
    void        *reply_cb_data;

    MUTEX_HANDLE   state_mutex;

    SEM_HANDLE     recv_sem;
    bool         recv_requested;
    uint32_t       recv_started_ms;
    char           pending_channel[16];
    char           pending_chat_id[96];

    bool         stop_requested;
} acp_ctx_t;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */

static acp_ctx_t s_ctx;

/* KV-backed config (loaded once at init, used throughout) */
static char s_gw_host[64]    = {0};
static char s_gw_token[128]  = {0};
static char s_device_id[64]  = {0};
static unsigned s_gw_port    = 0;

static void acp_load_config(void)
{
    app_cfg_get_gw_host(s_gw_host, sizeof(s_gw_host));
    s_gw_port = app_cfg_get_gw_port_num();
    app_cfg_get_gw_token(s_gw_token, sizeof(s_gw_token));
    app_cfg_get_device_id(s_device_id, sizeof(s_device_id));
}

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */

static void acp_client_task(void *arg);

static OPERATE_RET __ws_decode_frame(const uint8_t *rx_buf, size_t rx_len,
                                     size_t buf_cap,
                                     bool *fin,
                                     uint8_t *opcode,
                                     const uint8_t **payload,
                                     size_t *payload_len,
                                     size_t *consumed);
static OPERATE_RET __acp_queue_reply_to_bus(const char *reply);
static OPERATE_RET __acp_queue_timeout_to_bus(void);
static void __acp_log_full_text(const char *tag, const char *text, size_t text_len);
static OPERATE_RET __acp_feed_text_frame(uint8_t opcode,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         bool fin,
                                         const char **message_out,
                                         size_t *message_len_out);

/* ---------------------------------------------------------------------------
 * Internal helpers – raw TCP I/O
 * --------------------------------------------------------------------------- */

/**
 * @brief Send all bytes in buf to fd, retrying on EAGAIN.
 * @param[in] fd   Socket file descriptor.
 * @param[in] buf  Data buffer.
 * @param[in] len  Number of bytes to send.
 * @return OPRT_OK on success, OPRT_SEND_ERR on failure.
 */
static OPERATE_RET __send_all(int fd, const uint8_t *buf, size_t len)
{
    if (fd < 0 || (!buf && len > 0)) {
        return OPRT_INVALID_PARM;
    }
    size_t sent = 0;
    while (sent < len) {
        int n = tal_net_send(fd, buf + sent, (uint32_t)(len - sent));
        if (n == OPRT_RESOURCE_NOT_READY) {
            tal_system_sleep(5);
            continue;
        }
        if (n <= 0) {
            return OPRT_SEND_ERR;
        }
        sent += (size_t)n;
    }
    return OPRT_OK;
}

/**
 * @brief Recv bytes from fd into buf, retrying until len bytes or timeout.
 * @param[in]  fd          Socket file descriptor.
 * @param[out] buf         Output buffer.
 * @param[in]  len         Maximum bytes to receive.
 * @param[in]  timeout_ms  Maximum wait in milliseconds.
 * @return Number of bytes received, or 0 on timeout, negative on error.
 */
static int __recv_timeout(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (fd < 0 || !buf || len == 0) {
        return -1;
    }
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        TUYA_FD_SET_T rfds;
        TAL_FD_ZERO(&rfds);
        TAL_FD_SET(fd, &rfds);
        int ready = tal_net_select(fd + 1, &rfds, NULL, NULL, 50);
        if (ready > 0 && TAL_FD_ISSET(fd, &rfds)) {
            int n = tal_net_recv(fd, buf, (uint32_t)len);
            if (n > 0) {
                return n;
            }
            if (n == OPRT_RESOURCE_NOT_READY) {
                /* no data this poll cycle */
            } else {
                return -1;
            }
        }
        elapsed += 50;
    }
    return 0; /* timeout */
}

/* ---------------------------------------------------------------------------
 * Internal helpers – WebSocket masked frame send
 * --------------------------------------------------------------------------- */

/**
 * @brief Generate a 4-byte WS masking key.
 *
 * The masking key is used to comply with RFC 6455 §5.3.  It does not need
 * to be cryptographically strong; we use the system tick XOR'd with a
 * monotonic counter to avoid repeating a fixed key.
 *
 * @param[out] mask_key  4-byte output buffer.
 * @return none
 */
static void __gen_mask_key(uint8_t mask_key[4])
{
    static uint32_t s_counter = 0;
    uint32_t tick = tal_system_get_millisecond();
    uint32_t val  = tick ^ (++s_counter * 0x9e3779b9u);
    mask_key[0] = (uint8_t)((val >> 24) & 0xFF);
    mask_key[1] = (uint8_t)((val >> 16) & 0xFF);
    mask_key[2] = (uint8_t)((val >>  8) & 0xFF);
    mask_key[3] = (uint8_t)( val        & 0xFF);
}

/**
 * @brief Build and send a masked RFC 6455 WebSocket text frame.
 *
 * Per RFC 6455 §5.1, all frames sent from a client to a server MUST be masked.
 * The masked payload is applied in-place on a heap-allocated copy so the
 * caller's buffer is not modified.
 *
 * @param[in] fd          Socket file descriptor.
 * @param[in] opcode      WS opcode (use ACP_WS_OPCODE_TEXT for JSON messages).
 * @param[in] payload     Payload bytes.
 * @param[in] payload_len Payload length in bytes.
 * @return OPRT_OK on success.
 */
static OPERATE_RET __ws_send_masked(int fd, uint8_t opcode,
                                    const uint8_t *payload, size_t payload_len)
{
    /* Header: up to 2 + 8 length bytes + 4 mask bytes = 14 bytes */
    uint8_t header[14] = {0};
    size_t  hdr_len    = 0;
    uint8_t mask_key[4];

    __gen_mask_key(mask_key);

    header[0] = (uint8_t)(0x80 | (opcode & 0x0F)); /* FIN=1 */

    /* Byte 1: MASK bit set + payload length */
    if (payload_len <= 125) {
        header[1] = (uint8_t)(0x80 | payload_len);
        hdr_len   = 2;
    } else if (payload_len <= 0xFFFF) {
        header[1] = (uint8_t)(0x80 | 126);
        header[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        header[3] = (uint8_t)( payload_len       & 0xFF);
        hdr_len   = 4;
    } else {
        header[1] = (uint8_t)(0x80 | 127);
        uint64_t plen64 = (uint64_t)payload_len;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)((plen64 >> (56 - i * 8)) & 0xFF);
        }
        hdr_len = 10;
    }

    /* Append 4-byte masking key */
    memcpy(header + hdr_len, mask_key, 4);
    hdr_len += 4;

    OPERATE_RET rt = __send_all(fd, header, hdr_len);
    if (rt != OPRT_OK) {
        return rt;
    }

    if (payload_len == 0) {
        return OPRT_OK;
    }

    /* Allocate a copy to apply the mask without modifying caller's buffer */
    uint8_t *masked = (uint8_t *)claw_malloc(payload_len);
    if (!masked) {
        return OPRT_MALLOC_FAILED;
    }
    for (size_t i = 0; i < payload_len; i++) {
        masked[i] = (uint8_t)(payload[i] ^ mask_key[i % 4]);
    }

    rt = __send_all(fd, masked, payload_len);
    claw_free(masked);
    return rt;
}

/**
 * @brief Send a masked WebSocket text frame containing a JSON string.
 * @param[in] fd   Socket file descriptor.
 * @param[in] json NUL-terminated JSON string.
 * @return OPRT_OK on success.
 */
static OPERATE_RET __ws_send_json(int fd, const char *json)
{
    if (!json || json[0] == '\0') {
        return OPRT_INVALID_PARM;
    }
    return __ws_send_masked(fd, ACP_WS_OPCODE_TEXT,
                            (const uint8_t *)json, strlen(json));
}

/* ---------------------------------------------------------------------------
 * Internal helpers – WebSocket client handshake
 * --------------------------------------------------------------------------- */

/**
 * @brief Build the Base64-encoded Sec-WebSocket-Key from 16 random bytes.
 * @param[out] key_out  Output buffer (at least 32 bytes).
 * @param[in]  out_size Size of key_out.
 * @return OPRT_OK on success.
 */
static OPERATE_RET __ws_build_client_key(char *key_out, size_t out_size)
{
    static uint32_t s_seed = 0;
    uint8_t raw[16] = {0};
    uint32_t tick   = tal_system_get_tick_count();
    for (int i = 0; i < 4; i++) {
        uint32_t v = tick ^ (++s_seed * 0x6c62272eu);
        raw[i * 4 + 0] = (uint8_t)((v >> 24) & 0xFF);
        raw[i * 4 + 1] = (uint8_t)((v >> 16) & 0xFF);
        raw[i * 4 + 2] = (uint8_t)((v >>  8) & 0xFF);
        raw[i * 4 + 3] = (uint8_t)( v        & 0xFF);
        tick ^= v;
    }
    if (!tuya_base64_encode(raw, key_out, sizeof(raw))) {
        return OPRT_COM_ERROR;
    }
    (void)out_size;
    return OPRT_OK;
}

/**
 * @brief Perform the RFC 6455 HTTP upgrade handshake.
 *
 * Sends the upgrade request and validates the "101 Switching Protocols"
 * response from the server.
 *
 * @param[in] fd    Connected TCP socket.
 * @param[in] host  Host header value.
 * @param[in] port  Port (for Host header).
 * @return OPRT_OK if the server accepted the upgrade.
 */
static OPERATE_RET __ws_upgrade(int fd, const char *host, uint16_t port)
{
    char client_key[32] = {0};
    OPERATE_RET rt = __ws_build_client_key(client_key, sizeof(client_key));
    if (rt != OPRT_OK) {
        return rt;
    }

    char *req = (char *)claw_malloc(512);
    if (!req) {
        return OPRT_MALLOC_FAILED;
    }
    memset(req, 0, 512);
    int  n = snprintf(req, 512,
        "GET / HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Origin: http://%s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        host, (unsigned)port, host, (unsigned)port, client_key);
    if (n <= 0 || (size_t)n >= 512) {
        claw_free(req);
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    rt = __send_all(fd, (const uint8_t *)req, (size_t)n);
    claw_free(req);
    if (rt != OPRT_OK) {
        PR_ERR("acp upgrade send failed rt=%d", rt);
        return rt;
    }

    /* Read the server HTTP response (look for "101") */
    uint8_t *resp = (uint8_t *)claw_malloc(512);
    if (!resp) {
        return OPRT_MALLOC_FAILED;
    }
    memset(resp, 0, 512);
    int   got = __recv_timeout(fd, resp, 512 - 1, ACP_SYNC_RECV_TIMEOUT_MS);
    if (got <= 0) {
        PR_ERR("acp upgrade recv timeout/err got=%d", got);
        claw_free(resp);
        return OPRT_RECV_ERR;
    }
    resp[got] = '\0';

    if (!strstr((const char *)resp, "101")) {
        PR_ERR("acp upgrade rejected: %.80s", (const char *)resp);
        claw_free(resp);
        return OPRT_COM_ERROR;
    }

    claw_free(resp);
    PR_INFO("acp ws upgrade ok host=%s:%u", host, (unsigned)port);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Internal helpers – ACP protocol (connect + hello-ok)
 * --------------------------------------------------------------------------- */

/**
 * @brief Extract the mainSessionKey from a parsed ACP hello-ok payload.
 *
 * The hello-ok data lives inside res.payload (v3 protocol).
 * Tries res.payload.snapshot.sessionDefaults.mainSessionKey first,
 * falls back to "agent:main" if not found.
 *
 * @param[in]  hello_payload  cJSON object for the hello-ok payload.
 * @param[out] session_key    Output buffer.
 * @param[in]  sk_size        Buffer size.
 * @return none
 */
static void __extract_session_key(cJSON *hello_payload, char *session_key, size_t sk_size)
{
    cJSON *snapshot = cJSON_GetObjectItem(hello_payload, "snapshot");
    cJSON *sess_def = cJSON_GetObjectItem(snapshot, "sessionDefaults");
    cJSON *main_key = cJSON_GetObjectItem(sess_def, "mainSessionKey");

    if (cJSON_IsString(main_key) && main_key->valuestring &&
        main_key->valuestring[0] != '\0') {
        snprintf(session_key, sk_size, "%s", main_key->valuestring);
        PR_INFO("acp sessionKey=%s", session_key);
    } else {
        PR_WARN("acp hello-ok missing mainSessionKey, using default");
        snprintf(session_key, sk_size, "agent:main");
    }
}

/**
 * @brief Send the ACP "connect" request and wait for the hello-ok response.
 *
 * ACP v3 protocol flow:
 *   1. Gateway immediately sends a "connect.challenge" event (token auth:
 *      no client response needed – token is in connect req params.auth).
 *   2. Client sends the connect request.
 *   3. Gateway replies with a single "res" frame:
 *        {"type":"res","id":"1","ok":true,"payload":{"type":"hello-ok",
 *         "snapshot":{"sessionDefaults":{"mainSessionKey":"agent:main",...}}}}
 *      The hello-ok data is embedded in res.payload, NOT a separate event.
 *
 * Because the hello-ok payload is large (sessions list, capabilities, etc.)
 * it typically spans multiple TCP segments.  This function accumulates raw
 * bytes and uses __ws_decode_frame to wait for a complete WS frame before
 * parsing.
 *
 * @param[in]  fd          Connected, upgraded WebSocket socket.
 * @param[out] session_key Output buffer for the session key.
 * @param[in]  sk_size     Size of session_key buffer.
 * @return OPRT_OK on success.
 */
static OPERATE_RET __acp_connect(int fd, char *session_key, size_t sk_size)
{
    /* Build connect request */
    cJSON *root   = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *client = cJSON_CreateObject();
    cJSON *auth   = cJSON_CreateObject();
    if (!root || !params || !client || !auth) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        cJSON_Delete(client);
        cJSON_Delete(auth);
        return OPRT_MALLOC_FAILED;
    }

    cJSON_AddStringToObject(root,   "type",   "req");
    cJSON_AddStringToObject(root,   "id",     ACP_CONNECT_REQ_ID);
    cJSON_AddStringToObject(root,   "method", "connect");

    cJSON_AddNumberToObject(params, "minProtocol", ACP_PROTO_MIN);
    cJSON_AddNumberToObject(params, "maxProtocol", ACP_PROTO_MAX);

    cJSON_AddStringToObject(client, "id",           "openclaw-control-ui");
    cJSON_AddStringToObject(client, "mode",         "ui");
    cJSON_AddStringToObject(client, "version",      "1.0.0");
    cJSON_AddStringToObject(client, "platform",     "embedded");
    cJSON_AddStringToObject(client, "deviceFamily", "DuckyClaw");
    cJSON_AddStringToObject(client, "displayName",  s_device_id);
    cJSON_AddItemToObject(params, "client", client);

    cJSON_AddStringToObject(auth, "token", s_gw_token);
    cJSON_AddItemToObject(params, "auth", auth);

    cJSON_AddStringToObject(params, "role", "operator");
    cJSON *scopes = cJSON_CreateArray();
    if (scopes) {
        cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.admin"));
        cJSON_AddItemToObject(params, "scopes", scopes);
    }

    cJSON_AddItemToObject(root, "params", params);

    char *req_payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req_payload) {
        return OPRT_MALLOC_FAILED;
    }

    PR_DEBUG("acp connect req: %.128s", req_payload);
    OPERATE_RET rt = __ws_send_json(fd, req_payload);
    cJSON_free(req_payload);
    if (rt != OPRT_OK) {
        PR_ERR("acp connect send failed rt=%d", rt);
        return rt;
    }

    /*
     * Accumulate incoming bytes until __ws_decode_frame signals a complete
     * WS frame is available, then parse the JSON payload.
     * Use a heap buffer (16 KB) to handle the large hello-ok response.
     */
    const size_t RX_CAP = 32 * 1024;
    uint8_t *rx_buf = (uint8_t *)claw_malloc(RX_CAP);
    if (!rx_buf) {
        PR_ERR("acp connect: alloc rx_buf failed");
        return OPRT_MALLOC_FAILED;
    }
    size_t   rx_len = 0;
    uint32_t elapsed = 0;
    rt = OPRT_RECV_ERR; /* assume timeout unless we succeed */

    while (elapsed < ACP_SYNC_RECV_TIMEOUT_MS) {
        /* Receive available bytes into the accumulation buffer */
        if (rx_len < RX_CAP) {
            int got = __recv_timeout(fd, rx_buf + rx_len, RX_CAP - rx_len, 200);
            if (got < 0) {
                PR_ERR("acp connect recv error");
                rt = OPRT_RECV_ERR;
                break;
            }
            if (got > 0) {
                rx_len += (size_t)got;
            }
        }
        elapsed += 200;

        /* Try to decode complete WS frames from the accumulated data */
        while (rx_len > 0) {
            bool         fin         = FALSE;
            uint8_t        opcode      = 0;
            const uint8_t *ws_payload  = NULL;
            size_t         ws_plen     = 0;
            size_t         consumed    = 0;

            OPERATE_RET dec = __ws_decode_frame(rx_buf, rx_len, RX_CAP,
                                                &fin, &opcode, &ws_payload,
                                                &ws_plen, &consumed);
            if (dec == OPRT_RESOURCE_NOT_READY) {
                break; /* need more data */
            }
            if (dec != OPRT_OK || consumed == 0) {
                PR_ERR("acp connect frame decode err rt=%d", dec);
                rt = OPRT_COM_ERROR;
                goto done;
            }

            /*
             * Copy payload BEFORE consuming (memmove invalidates ws_payload).
             * Only allocate for text frames that need JSON parsing.
             */
            char *tmp = NULL;
            if (opcode == ACP_WS_OPCODE_TEXT && ws_payload && ws_plen > 0) {
                tmp = (char *)claw_malloc(ws_plen + 1);
                if (!tmp) {
                    rt = OPRT_MALLOC_FAILED;
                    goto done;
                }
                memcpy(tmp, ws_payload, ws_plen);
                tmp[ws_plen] = '\0';
            }

            /* Consume the frame bytes (invalidates ws_payload) */
            if (consumed < rx_len) {
                memmove(rx_buf, rx_buf + consumed, rx_len - consumed);
            }
            rx_len -= consumed;

            /* Skip non-text frames */
            if (!tmp) {
                continue;
            }

            PR_DEBUG("acp connect frame: %.128s", tmp);

            cJSON *resp = cJSON_Parse(tmp);
            claw_free(tmp);

            if (!resp) {
                continue; /* malformed frame, keep waiting */
            }

            cJSON *type_field = cJSON_GetObjectItem(resp, "type");
            if (!cJSON_IsString(type_field)) {
                cJSON_Delete(resp);
                continue;
            }

            /* event frame (connect.challenge before req, or other events) */
            if (strcmp(type_field->valuestring, "event") == 0) {
                cJSON *event_name = cJSON_GetObjectItem(resp, "event");
                if (cJSON_IsString(event_name) &&
                    strcmp(event_name->valuestring, "connect.challenge") == 0) {
                    cJSON *nonce = cJSON_GetObjectItem(
                        cJSON_GetObjectItem(resp, "payload"), "nonce");
                    PR_DEBUG("acp connect.challenge nonce=%s (token auth: no response needed)",
                             cJSON_IsString(nonce) ? nonce->valuestring : "?");
                }
                cJSON_Delete(resp);
                continue;
            }

            /* res frame – the only expected response to our connect req */
            if (strcmp(type_field->valuestring, "res") == 0) {
                cJSON *ok_field = cJSON_GetObjectItem(resp, "ok");
                if (!cJSON_IsBool(ok_field) || !cJSON_IsTrue(ok_field)) {
                    cJSON *err_obj = cJSON_GetObjectItem(resp, "error");
                    cJSON *err_code = err_obj ? cJSON_GetObjectItem(err_obj, "code") : NULL;
                    cJSON *err_msg  = err_obj ? cJSON_GetObjectItem(err_obj, "message") : NULL;
                    PR_ERR("acp connect res ok=false code=%s msg=%.200s",
                           cJSON_IsString(err_code) ? err_code->valuestring : "?",
                           cJSON_IsString(err_msg)  ? err_msg->valuestring  : "?");
                    cJSON_Delete(resp);
                    rt = OPRT_COM_ERROR;
                    goto done;
                }

                /*
                 * ACP v3: hello-ok data is in res.payload (not a separate
                 * event).  Extract sessionKey from
                 * res.payload.snapshot.sessionDefaults.mainSessionKey.
                 */
                cJSON *res_payload = cJSON_GetObjectItem(resp, "payload");
                __extract_session_key(res_payload, session_key, sk_size);
                cJSON_Delete(resp);
                rt = OPRT_OK;
                goto done;
            }

            cJSON_Delete(resp);
        }
    }

done:
    claw_free(rx_buf);
    if (rt == OPRT_RECV_ERR) {
        PR_ERR("acp connect timeout waiting for hello-ok");
    }
    return rt;
}

/* ---------------------------------------------------------------------------
 * Internal helpers – WS frame decode in the RX loop
 * --------------------------------------------------------------------------- */

/**
 * @brief Decode one WS frame from the head of rx_buf.
 *
 * Unmasked frames (server→client) are returned as-is.  The payload is
 * a pointer into rx_buf and is valid until the next call to this function.
 *
 * @param[in]  rx_buf      Input buffer.
 * @param[in]  rx_len      Bytes available in rx_buf.
 * @param[in]  buf_cap     Total capacity of rx_buf in bytes. Frames whose
 *                         payload exceeds this limit are dropped.
 * @param[out] opcode      WS opcode byte.
 * @param[out] payload     Pointer to (unmasked) payload in rx_buf.
 * @param[out] payload_len Payload length in bytes.
 * @param[out] consumed    Total frame bytes consumed (header + payload).
 * @return OPRT_OK          Frame decoded successfully.
 * @return OPRT_RESOURCE_NOT_READY  Not enough data yet (partial frame).
 * @return OPRT_COM_ERROR   Frame too large to fit in buf_cap, caller should
 *                          reconnect.
 */
static OPERATE_RET __ws_decode_frame(const uint8_t *rx_buf, size_t rx_len,
                                     size_t buf_cap,
                                     bool *fin,
                                     uint8_t *opcode,
                                     const uint8_t **payload,
                                     size_t *payload_len,
                                     size_t *consumed)
{
    if (!rx_buf || !fin || !opcode || !payload || !payload_len || !consumed) {
        return OPRT_INVALID_PARM;
    }
    if (rx_len < 2) {
        return OPRT_RESOURCE_NOT_READY;
    }

    *fin = ((rx_buf[0] & 0x80) != 0) ? TRUE : FALSE;
    uint8_t  op     = (uint8_t)(rx_buf[0] & 0x0F);
    bool   masked = (rx_buf[1] & 0x80) != 0;
    uint64_t plen   = (uint64_t)(rx_buf[1] & 0x7F);
    size_t   off    = 2;

    if (plen == 126) {
        if (rx_len < off + 2) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = (uint64_t)((rx_buf[off] << 8) | rx_buf[off + 1]);
        off += 2;
    } else if (plen == 127) {
        if (rx_len < off + 8) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | rx_buf[off + i];
        }
        off += 8;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (rx_len < off + 4) {
            return OPRT_RESOURCE_NOT_READY;
        }
        memcpy(mask, rx_buf + off, 4);
        off += 4;
    }

    if (plen > (uint64_t)(buf_cap - off)) {
        PR_WARN("acp rx frame too large plen=%llu buf_cap=%u, dropping",
                (unsigned long long)plen, (unsigned)buf_cap);
        /* Guard: only consume if the full frame data has already arrived.
         * Setting consumed = off + plen when plen >> rx_len causes a size_t
         * underflow in the caller (rx_len -= consumed wraps to ~4 GB), which
         * then triggers a memmove of gigabytes and crashes with BusFault. */
        size_t full_frame = off + (size_t)plen;
        if (rx_len < full_frame) {
            /* Frame not fully received yet; signal caller to reconnect rather
             * than wait indefinitely for an oversized frame. */
            return OPRT_COM_ERROR;
        }
        *consumed    = full_frame;
        *payload_len = 0;
        *opcode      = op;
        *payload     = NULL;
        return OPRT_OK;
    }

    size_t frame_len = off + (size_t)plen;
    if (rx_len < frame_len) {
        return OPRT_RESOURCE_NOT_READY;
    }

    /* Unmask in-place (server should never send masked, but handle it anyway) */
    if (masked && plen > 0) {
        uint8_t *data = (uint8_t *)(rx_buf + off);
        for (size_t i = 0; i < (size_t)plen; i++) {
            data[i] ^= mask[i % 4];
        }
    }

    *opcode      = op;
    *payload     = rx_buf + off;
    *payload_len = (size_t)plen;
    *consumed    = frame_len;
    return OPRT_OK;
}

/**
 * @brief Consume (remove) the first `consumed` bytes from rx_buf.
 * @param[in] consumed  Number of bytes to remove.
 * @return none
 */
static void __rx_consume(size_t consumed)
{
    if (consumed == 0 || consumed > s_ctx.rx_len) {
        return;
    }
    if (consumed < s_ctx.rx_len) {
        memmove(s_ctx.rx_buf, s_ctx.rx_buf + consumed,
                s_ctx.rx_len - consumed);
    }
    s_ctx.rx_len -= consumed;
}

/* ---------------------------------------------------------------------------
 * Internal helpers – ACP frame dispatch
 * --------------------------------------------------------------------------- */

/**
 * @brief Send a pong response to a gateway tick heartbeat.
 *
 * The openclaw gateway sends a "tick" event every ~30 seconds.  Responding
 * with a "pong" req keeps the connection alive and prevents the gateway
 * from closing the session.
 *
 * @param[in] seq  Sequence number from the incoming tick event.
 * @return none
 */
void __acp_send_pong(uint32_t seq)
{
    char buf[128] = {0};
    snprintf(buf, sizeof(buf),
             "{\"type\":\"req\",\"id\":\"%u\",\"method\":\"pong\","
             "\"params\":{\"seq\":%u}}",
             (unsigned)(++s_ctx.req_id_counter), (unsigned)seq);
    (void)__ws_send_json(s_ctx.fd, buf);
    PR_DEBUG("acp pong sent seq=%u", (unsigned)seq);
}

static void __acp_snapshot_pending_route(void)
{
    const char *channel = app_im_get_channel();
    const char *chat_id = app_im_get_chat_id();

    tal_mutex_lock(s_ctx.state_mutex);

    s_ctx.pending_channel[0] = '\0';
    s_ctx.pending_chat_id[0] = '\0';

    if (channel && channel[0] != '\0') {
        strncpy(s_ctx.pending_channel, channel, sizeof(s_ctx.pending_channel) - 1);
        s_ctx.pending_channel[sizeof(s_ctx.pending_channel) - 1] = '\0';
    }
    if (chat_id && chat_id[0] != '\0') {
        strncpy(s_ctx.pending_chat_id, chat_id, sizeof(s_ctx.pending_chat_id) - 1);
        s_ctx.pending_chat_id[sizeof(s_ctx.pending_chat_id) - 1] = '\0';
    }

    tal_mutex_unlock(s_ctx.state_mutex);
}

static void __acp_clear_pending_locked(void)
{
    s_ctx.recv_requested = FALSE;
    s_ctx.recv_started_ms = 0;
    s_ctx.pending_channel[0] = '\0';
    s_ctx.pending_chat_id[0] = '\0';
}

static void __acp_take_pending_route(char *channel, size_t channel_size,
                                     char *chat_id, size_t chat_id_size)
{
    if (!channel || channel_size == 0 || !chat_id || chat_id_size == 0) {
        return;
    }

    channel[0] = '\0';
    chat_id[0] = '\0';

    tal_mutex_lock(s_ctx.state_mutex);
    strncpy(channel, s_ctx.pending_channel, channel_size - 1);
    channel[channel_size - 1] = '\0';
    strncpy(chat_id, s_ctx.pending_chat_id, chat_id_size - 1);
    chat_id[chat_id_size - 1] = '\0';
    __acp_clear_pending_locked();
    tal_mutex_unlock(s_ctx.state_mutex);
}

static OPERATE_RET __acp_push_notice_to_bus(const char *content, const char *tag)
{
    char channel[sizeof(s_ctx.pending_channel)] = {0};
    char chat_id[sizeof(s_ctx.pending_chat_id)] = {0};
    size_t content_len;
    char *owned_content;
    sys_msg_t in = {0};

    if (!content || content[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    __acp_take_pending_route(channel, sizeof(channel), chat_id, sizeof(chat_id));

    if (channel[0] == '\0' || chat_id[0] == '\0') {
        PR_WARN("acp pending route missing for %s channel=%s chat_id=%s, fallback to system inbound",
                tag ? tag : "notice",
                channel[0] ? channel : "(empty)",
                chat_id[0] ? chat_id : "(empty)");
        strncpy(channel, "system", sizeof(channel) - 1);
        channel[sizeof(channel) - 1] = '\0';
        chat_id[0] = '\0';
    }

    content_len = strlen(content);
    owned_content = (char *)claw_malloc(content_len + 1);
    if (!owned_content) {
        return OPRT_MALLOC_FAILED;
    }

    memcpy(owned_content, content, content_len + 1);

    strncpy(in.channel, channel, sizeof(in.channel) - 1);
    in.channel[sizeof(in.channel) - 1] = '\0';
    strncpy(in.chat_id, chat_id, sizeof(in.chat_id) - 1);
    in.chat_id[sizeof(in.chat_id) - 1] = '\0';
    in.content = owned_content;

    OPERATE_RET rt = sys_bus_push_inbound(&in);
    if (rt != OPRT_OK) {
        PR_ERR("acp %s inject failed channel=%s chat_id=%s rt=%d",
               tag ? tag : "notice", channel, chat_id, rt);
        claw_free(owned_content);
        return rt;
    }

    PR_INFO("acp %s queued to sys_bus channel=%s chat_id=%s",
            tag ? tag : "notice", channel, chat_id);
    return OPRT_OK;
}

static void __acp_log_full_text(const char *tag, const char *text, size_t text_len)
{
    if (!tag || !text) {
        return;
    }

    PR_INFO("%s len=%u", tag, (unsigned)text_len);
    for (size_t off = 0; off < text_len; off += ACP_LOG_CHUNK_LEN) {
        size_t chunk_len = text_len - off;
        if (chunk_len > ACP_LOG_CHUNK_LEN) {
            chunk_len = ACP_LOG_CHUNK_LEN;
        }

        char chunk[ACP_LOG_CHUNK_LEN + 1];
        memcpy(chunk, text + off, chunk_len);
        chunk[chunk_len] = '\0';

        PR_INFO("%s[%u:%u] %s",
                tag,
                (unsigned)off,
                (unsigned)(off + chunk_len),
                chunk);
    }
}

static OPERATE_RET __acp_feed_text_frame(uint8_t opcode,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         bool fin,
                                         const char **message_out,
                                         size_t *message_len_out)
{
    if (!message_out || !message_len_out) {
        return OPRT_INVALID_PARM;
    }

    *message_out     = NULL;
    *message_len_out = 0;

    if (opcode == ACP_WS_OPCODE_TEXT) {
        s_ctx.ws_msg_active = TRUE;
        s_ctx.ws_msg_len    = 0;
    } else if (opcode == ACP_WS_OPCODE_CONT) {
        if (!s_ctx.ws_msg_active) {
            PR_WARN("acp continuation frame received without an active message");
            return OPRT_COM_ERROR;
        }
    } else {
        return OPRT_INVALID_PARM;
    }

    if (payload_len > 0) {
        if (!payload) {
            return OPRT_INVALID_PARM;
        }
        if (s_ctx.ws_msg_len + payload_len >= ((size_t)ACP_CLIENT_WS_MSG_BUF_SIZE)) {
            PR_WARN("acp ws message too large msg_len=%u frag=%u cap=%u",
                    (unsigned)s_ctx.ws_msg_len,
                    (unsigned)payload_len,
                    (unsigned)((size_t)ACP_CLIENT_WS_MSG_BUF_SIZE));
            s_ctx.ws_msg_active = FALSE;
            s_ctx.ws_msg_len    = 0;
            return OPRT_BUFFER_NOT_ENOUGH;
        }

        memcpy(s_ctx.ws_msg_buf + s_ctx.ws_msg_len, payload, payload_len);
        s_ctx.ws_msg_len += payload_len;
    }

    if (!fin) {
        return OPRT_RESOURCE_NOT_READY;
    }

    s_ctx.ws_msg_buf[s_ctx.ws_msg_len] = '\0';
    s_ctx.ws_msg_active = FALSE;

    *message_out     = s_ctx.ws_msg_buf;
    *message_len_out = s_ctx.ws_msg_len;
    return OPRT_OK;
}

static OPERATE_RET __acp_queue_reply_to_bus(const char *reply)
{
    static const char prefix[] =
        "System notification: This is the execution result of the previous task. Please organize it and directly reply to the user.\n";
    char *content;
    size_t prefix_len;
    size_t reply_len;

    if (!reply || reply[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    prefix_len = strlen(prefix);
    reply_len  = strlen(reply);
    content = (char *)claw_malloc(prefix_len + reply_len + 1);
    if (!content) {
        tal_mutex_lock(s_ctx.state_mutex);
        __acp_clear_pending_locked();
        tal_mutex_unlock(s_ctx.state_mutex);
        return OPRT_MALLOC_FAILED;
    }

    memcpy(content, prefix, prefix_len);
    memcpy(content + prefix_len, reply, reply_len);
    content[prefix_len + reply_len] = '\0';

    OPERATE_RET rt = __acp_push_notice_to_bus(content, "reply");
    claw_free(content);
    return rt;
}

static OPERATE_RET __acp_queue_timeout_to_bus(void)
{
    static const char timeout_notice[] =
        "System notification: AI assistant did not return a final result within 180 seconds. "
        "Please tell the user that the previous ACP task timed out or the reply was lost, "
        "and suggest retrying if needed.";

    return __acp_push_notice_to_bus(timeout_notice, "timeout");
}

static bool __acp_copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t copy_len;

    if (!dst || dst_size == 0 || !src || src[0] == '\0') {
        return FALSE;
    }

    copy_len = strlen(src);
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }

    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
    return TRUE;
}

static bool __acp_find_text_recursive(cJSON *node, char *dst, size_t dst_size)
{
    if (!node || !dst || dst_size == 0) {
        return FALSE;
    }

    if (cJSON_IsObject(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (child->string &&
                strcmp(child->string, "text") == 0 &&
                cJSON_IsString(child) &&
                child->valuestring &&
                child->valuestring[0] != '\0') {
                return __acp_copy_text(dst, dst_size, child->valuestring);
            }
            if (__acp_find_text_recursive(child, dst, dst_size)) {
                return TRUE;
            }
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (__acp_find_text_recursive(child, dst, dst_size)) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static bool __acp_extract_text_from_message(cJSON *message, char *dst, size_t dst_size)
{
    cJSON *content_arr;
    cJSON *part = NULL;

    if (!cJSON_IsObject(message) || !dst || dst_size == 0) {
        return FALSE;
    }

    content_arr = cJSON_GetObjectItem(message, "content");
    if (!cJSON_IsArray(content_arr)) {
        return FALSE;
    }

    cJSON_ArrayForEach(part, content_arr) {
        cJSON *part_type = cJSON_GetObjectItem(part, "type");
        cJSON *part_text = cJSON_GetObjectItem(part, "text");
        if (cJSON_IsString(part_type) &&
            strcmp(part_type->valuestring, "text") == 0 &&
            cJSON_IsString(part_text) &&
            part_text->valuestring &&
            part_text->valuestring[0] != '\0') {
            return __acp_copy_text(dst, dst_size, part_text->valuestring);
        }
    }

    return FALSE;
}

static bool __acp_is_true_value(cJSON *node)
{
    if (!node) {
        return FALSE;
    }
    if (cJSON_IsBool(node)) {
        return cJSON_IsTrue(node) ? TRUE : FALSE;
    }
    if (cJSON_IsNumber(node)) {
        return (node->valuedouble != 0) ? TRUE : FALSE;
    }
    return FALSE;
}

static bool __acp_event_is_final(cJSON *root, const char *event_name)
{
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON *data    = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "data") : NULL;
    cJSON *state   = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "state") : NULL;
    cJSON *done    = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "done") : NULL;
    cJSON *final   = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "final") : NULL;
    cJSON *finish  = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "finish") : NULL;
    cJSON *data_done = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "done") : NULL;
    cJSON *data_final = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "final") : NULL;
    cJSON *data_finish = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "finish") : NULL;

    if (cJSON_IsString(state) && state->valuestring) {
        if (strcmp(state->valuestring, "final") == 0 ||
            strcmp(state->valuestring, "done") == 0 ||
            strcmp(state->valuestring, "complete") == 0) {
            return TRUE;
        }
    }

    if (__acp_is_true_value(done) ||
        __acp_is_true_value(final) ||
        __acp_is_true_value(finish) ||
        __acp_is_true_value(data_done) ||
        __acp_is_true_value(data_final) ||
        __acp_is_true_value(data_finish)) {
        return TRUE;
    }

    if (event_name &&
        (strstr(event_name, "final") != NULL ||
         strstr(event_name, "done") != NULL ||
         strstr(event_name, "complete") != NULL)) {
        return TRUE;
    }

    return FALSE;
}

static bool __acp_extract_event_text(cJSON *root, char *dst, size_t dst_size)
{
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON *message = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "message") : NULL;
    cJSON *data    = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "data") : NULL;
    cJSON *root_data = cJSON_GetObjectItem(root, "data");

    if (__acp_extract_text_from_message(message, dst, dst_size)) {
        return TRUE;
    }
    if (__acp_find_text_recursive(data, dst, dst_size)) {
        return TRUE;
    }
    if (__acp_find_text_recursive(payload, dst, dst_size)) {
        return TRUE;
    }
    if (__acp_find_text_recursive(root_data, dst, dst_size)) {
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Process one decoded ACP JSON text message.
 *
 * Handles:
 *   - type "event": extract the final ACP text reply and route it
 *   - type "event", event "tick": log keepalive ticks
 *   - type "res": log unexpected responses
 *
 * @param[in] text      NUL-terminated JSON payload.
 * @param[in] text_len  Length of text (informational).
 * @return none
 */
static void __acp_dispatch(const char *text, size_t text_len)
{
    (void)text_len;
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        PR_WARN("acp dispatch json parse failed");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "event") == 0) {
        cJSON *event = cJSON_GetObjectItem(root, "event");
        char  *event_text = (char *)claw_malloc(ACP_CLIENT_REPLY_BUF_SIZE);
        if (!event_text) {
            cJSON_Delete(root);
            return;
        }
        memset(event_text, 0, ACP_CLIENT_REPLY_BUF_SIZE);
        bool is_final = FALSE;

        if (!cJSON_IsString(event) || !event->valuestring) {
            claw_free(event_text);
            cJSON_Delete(root);
            return;
        }

        if (strcmp(event->valuestring, "tick") != 0 &&
            __acp_extract_event_text(root, event_text, ACP_CLIENT_REPLY_BUF_SIZE)) {
            size_t text_size = strlen(event_text);
            memcpy(s_ctx.reply_buf, event_text, text_size);
            s_ctx.reply_buf[text_size] = '\0';
            s_ctx.reply_len = text_size;
        }

        is_final = __acp_event_is_final(root, event->valuestring);
        if (is_final && s_ctx.reply_len > 0) {
            PR_NOTICE("acp reply final len=%u event=%s",
                    (unsigned)s_ctx.reply_len,
                    event->valuestring);
            __acp_log_full_text("acp reply final", s_ctx.reply_buf, s_ctx.reply_len);
            OPERATE_RET queue_rt = OPRT_OK;

            tal_mutex_lock(s_ctx.state_mutex);
            bool recv_requested = s_ctx.recv_requested;
            tal_mutex_unlock(s_ctx.state_mutex);

            if (recv_requested) {
                queue_rt = __acp_queue_reply_to_bus(s_ctx.reply_buf);
            } else {
                PR_WARN("acp final reply arrived without a pending request window, dropping");
            }

            if (s_ctx.reply_cb && queue_rt != OPRT_OK) {
                s_ctx.reply_cb(s_ctx.reply_buf, s_ctx.reply_cb_data);
            }
            s_ctx.reply_len    = 0;
            s_ctx.reply_buf[0] = '\0';
        }

        /* ---- tick heartbeat: no response needed, just log to confirm alive ---- */
        if (strcmp(event->valuestring, "tick") == 0) {
            cJSON *evt_payload = cJSON_GetObjectItem(root, "payload");
            cJSON *seq_field   = cJSON_GetObjectItem(evt_payload, "seq");
            uint32_t seq = cJSON_IsNumber(seq_field) ? (uint32_t)seq_field->valuedouble : 0;
            PR_DEBUG("acp tick seq=%u (keepalive, no response needed)", (unsigned)seq);
        }

        claw_free(event_text);

    } else if (strcmp(type->valuestring, "res") == 0) {
        /* connect response is handled synchronously in __acp_connect;
         * log unexpected res frames here for debug. */
        cJSON *id      = cJSON_GetObjectItem(root, "id");
        cJSON *ok      = cJSON_GetObjectItem(root, "ok");
        cJSON *error   = cJSON_GetObjectItem(root, "error");
        cJSON *errcode = error ? cJSON_GetObjectItem(error, "code")    : NULL;
        cJSON *errmsg  = error ? cJSON_GetObjectItem(error, "message") : NULL;
        if (cJSON_IsBool(ok) && cJSON_IsTrue(ok)) {
            PR_DEBUG("acp res id=%s ok=true",
                     cJSON_IsString(id) ? id->valuestring : "?");
        } else {
            PR_WARN("acp res id=%s ok=false code=%s msg=%s",
                    cJSON_IsString(id)      ? id->valuestring      : "?",
                    cJSON_IsString(errcode) ? errcode->valuestring : "?",
                    cJSON_IsString(errmsg)  ? errmsg->valuestring  : "?");
        }
    }

    cJSON_Delete(root);
}

/* ---------------------------------------------------------------------------
 * Internal helpers – connection lifecycle
 * --------------------------------------------------------------------------- */

/**
 * @brief Open a TCP connection, perform WS upgrade, and ACP handshake.
 * @return OPRT_OK on success; the socket fd is stored in s_ctx.fd.
 */
static OPERATE_RET __connect_and_handshake(void)
{
    TUYA_IP_ADDR_T ip_addr = 0;

    /* Resolve host – try str2addr first (works for dotted-decimal IPs) */
    ip_addr = tal_net_str2addr(s_gw_host);
    if (ip_addr == 0) {
        OPERATE_RET rt = tal_net_gethostbyname(s_gw_host, &ip_addr);
        if (rt != OPRT_OK || ip_addr == 0) {
            PR_ERR("acp dns resolve failed host=%s", s_gw_host);
            return OPRT_NETWORK_ERROR;
        }
    }

    int fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0) {
        PR_ERR("acp socket create failed");
        return OPRT_NETWORK_ERROR;
    }

    OPERATE_RET rt = tal_net_connect(fd, ip_addr, (uint16_t)s_gw_port);
    if (rt != OPRT_OK) {
        PR_ERR("acp tcp connect failed rt=%d host=%s port=%u",
               rt, s_gw_host, s_gw_port);
        tal_net_close(fd);
        return rt;
    }
    PR_NOTICE("acp tcp connected fd=%d host=%s:%u",
            fd, s_gw_host, s_gw_port);

    rt = __ws_upgrade(fd, s_gw_host, (uint16_t)s_gw_port);
    if (rt != OPRT_OK) {
        tal_net_close(fd);
        return rt;
    }

    rt = __acp_connect(fd, s_ctx.session_key, sizeof(s_ctx.session_key));
    if (rt != OPRT_OK) {
        tal_net_close(fd);
        return rt;
    }

    s_ctx.fd    = fd;
    s_ctx.state = ACP_STATE_CONNECTED;
    PR_INFO("acp client ready fd=%d session=%s", fd, s_ctx.session_key);
    return OPRT_OK;
}

/**
 * @brief Close the socket and reset connection state.
 * @return none
 */
static void __disconnect(void)
{
    if (s_ctx.fd >= 0) {
        tal_net_close(s_ctx.fd);
        s_ctx.fd = -1;
    }
    s_ctx.state          = ACP_STATE_DISCONNECTED;
    s_ctx.session_key[0] = '\0';
    s_ctx.rx_len         = 0;
    s_ctx.reply_len      = 0;
    s_ctx.reply_buf[0]   = '\0';
    s_ctx.ws_msg_len     = 0;
    s_ctx.ws_msg_active  = FALSE;
}

/* ---------------------------------------------------------------------------
 * Background task
 * --------------------------------------------------------------------------- */

/**
 * @brief Main ACP client task: connect, recv loop, reconnect on error.
 * @param[in] arg  Unused.
 * @return none
 */
static void acp_client_task(void *arg)
{
    (void)arg;
    static const char timeout_fallback[] =
        "AI assistant task timed out after 180 seconds. Please retry if needed.";

    PR_INFO("acp client task started");

    while (!s_ctx.stop_requested) {
        tal_mutex_lock(s_ctx.state_mutex);
        bool recv_requested = s_ctx.recv_requested;
        uint32_t recv_started_ms = s_ctx.recv_started_ms;
        tal_mutex_unlock(s_ctx.state_mutex);

        if (recv_requested &&
            recv_started_ms != 0 &&
            (uint32_t)(tal_system_get_millisecond() - recv_started_ms) >= ACP_ASYNC_REPLY_TIMEOUT_MS) {
            PR_ERR("acp pending reply timeout after %u ms",
                   (unsigned)ACP_ASYNC_REPLY_TIMEOUT_MS);

            OPERATE_RET queue_rt = __acp_queue_timeout_to_bus();
            if (queue_rt != OPRT_OK && s_ctx.reply_cb) {
                s_ctx.reply_cb(timeout_fallback, s_ctx.reply_cb_data);
            }
            continue;
        }

        /* ---- Connection phase ---- */
        if (s_ctx.state == ACP_STATE_DISCONNECTED) {
            OPERATE_RET rt = __connect_and_handshake();
            if (rt != OPRT_OK) {
                PR_WARN("acp connect failed, retry in %dms", ACP_CLIENT_RECONNECT_MS);
                tal_system_sleep(ACP_CLIENT_RECONNECT_MS);
                continue;
            }
        }

        tal_mutex_lock(s_ctx.state_mutex);
        recv_requested = s_ctx.recv_requested;
        tal_mutex_unlock(s_ctx.state_mutex);

        if (!recv_requested) {
            (void)tal_semaphore_wait(s_ctx.recv_sem, ACP_SELECT_TIMEOUT_MS);
            continue;
        }

        /* ---- Receive phase ---- */
        TUYA_FD_SET_T rfds;
        TAL_FD_ZERO(&rfds);
        TAL_FD_SET(s_ctx.fd, &rfds);

        int ready = tal_net_select(s_ctx.fd + 1, &rfds, NULL, NULL,
                                     ACP_SELECT_TIMEOUT_MS);
        if (ready <= 0) {
            continue;
        }

        if (!TAL_FD_ISSET(s_ctx.fd, &rfds)) {
            continue;
        }

        if (s_ctx.rx_len >= ((size_t)ACP_CLIENT_RX_BUF_SIZE)) {
            PR_WARN("acp rx buffer full, disconnecting");
            __disconnect();
            continue;
        }

        int n = tal_net_recv(s_ctx.fd,
                               s_ctx.rx_buf + s_ctx.rx_len,
                               (uint32_t)(((size_t)ACP_CLIENT_RX_BUF_SIZE) - s_ctx.rx_len));
        if (n == OPRT_RESOURCE_NOT_READY) {
            continue;
        }
        if (n <= 0) {
            PR_WARN("acp connection closed by server, reconnecting");
            __disconnect();
            continue;
        }
        s_ctx.rx_len += (size_t)n;

        /* ---- Frame decode loop ---- */
        while (s_ctx.rx_len > 0) {
            bool        fin         = FALSE;
            uint8_t       opcode      = 0;
            const uint8_t *payload    = NULL;
            size_t         payload_len = 0;
            size_t         consumed   = 0;

            OPERATE_RET rt = __ws_decode_frame(s_ctx.rx_buf, s_ctx.rx_len,
                                               ACP_CLIENT_RX_BUF_SIZE,
                                               &fin, &opcode, &payload,
                                               &payload_len, &consumed);
            if (rt == OPRT_RESOURCE_NOT_READY) {
                break; /* wait for more data */
            }
            if (rt != OPRT_OK) {
                PR_WARN("acp frame decode err rt=%d", rt);
                __disconnect();
                break;
            }

            switch (opcode) {
            case ACP_WS_OPCODE_TEXT:
            case ACP_WS_OPCODE_CONT: {
                const char *full_message = NULL;
                size_t      full_len     = 0;

                rt = __acp_feed_text_frame(opcode, payload, payload_len, fin,
                                           &full_message, &full_len);
                if (rt == OPRT_OK && full_message) {
                    __acp_dispatch(full_message, full_len);
                } else if (rt != OPRT_RESOURCE_NOT_READY) {
                    PR_WARN("acp ws text frame handling failed rt=%d opcode=0x%02x fin=%d",
                            rt, opcode, fin);
                    __disconnect();
                }
                break;
            }

            case ACP_WS_OPCODE_BINARY:
                PR_WARN("acp binary frame ignored len=%u fin=%d",
                        (unsigned)payload_len, fin);
                break;

            case ACP_WS_OPCODE_PING:
                /* Respond with a PONG to keep the connection alive */
                (void)__ws_send_masked(s_ctx.fd, ACP_WS_OPCODE_PONG,
                                       payload, payload_len);
                break;

            case ACP_WS_OPCODE_PONG:
                PR_DEBUG("acp recv pong len=%u", (unsigned)payload_len);
                break;

            case ACP_WS_OPCODE_CLOSE:
                PR_INFO("acp server sent close frame");
                __disconnect();
                break;

            default:
                PR_DEBUG("acp unhandled opcode=0x%02x", opcode);
                break;
            }

            __rx_consume(consumed);
            if (s_ctx.state == ACP_STATE_DISCONNECTED) {
                break;
            }
        }
    }

    PR_INFO("acp client task stopped");
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * @brief Register a callback for incoming OpenClaw agent replies.
 * @param[in] cb         Callback; NULL clears.
 * @param[in] user_data  Forwarded pointer.
 * @return none
 */
void acp_client_set_reply_cb(acp_reply_cb_t cb, void *user_data)
{
    s_ctx.reply_cb      = cb;
    s_ctx.reply_cb_data = user_data;
}

OPERATE_RET __acp_client_init_evt_cb(void *data)
{
    if (s_ctx.thread) {
        return OPRT_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.fd    = -1;
    s_ctx.state = ACP_STATE_DISCONNECTED;

    acp_load_config();

    if (s_gw_token[0] == '\0' || s_device_id[0] == '\0' || s_gw_host[0] == '\0') {
        PR_WARN("acp_client: gateway config incomplete (token/device_id/host empty), skip initialization");
        return OPRT_OK;
    }

    /* Allocate the three big I/O buffers in PSRAM (claw_malloc → tal_psram_malloc
     * when ENABLE_EXT_RAM is on). Together these were ~48 KiB of internal SRAM
     * .bss in the previous static layout — moving them out frees that SRAM for
     * the WiFi softAP / BLE controller during the BIND phase. */
    s_ctx.rx_buf      = (uint8_t *)claw_malloc(ACP_CLIENT_RX_BUF_SIZE);
    s_ctx.ws_msg_buf  = (char *)   claw_malloc(ACP_CLIENT_WS_MSG_BUF_SIZE);
    s_ctx.reply_buf   = (char *)   claw_malloc(ACP_CLIENT_REPLY_BUF_SIZE);
    if (!s_ctx.rx_buf || !s_ctx.ws_msg_buf || !s_ctx.reply_buf) {
        PR_ERR("acp client buf alloc failed rx=%p ws=%p reply=%p sizes=%d/%d/%d",
               s_ctx.rx_buf, s_ctx.ws_msg_buf, s_ctx.reply_buf,
               (int)ACP_CLIENT_RX_BUF_SIZE,
               (int)ACP_CLIENT_WS_MSG_BUF_SIZE,
               (int)ACP_CLIENT_REPLY_BUF_SIZE);
        if (s_ctx.rx_buf)     { claw_free(s_ctx.rx_buf);     s_ctx.rx_buf     = NULL; }
        if (s_ctx.ws_msg_buf) { claw_free(s_ctx.ws_msg_buf); s_ctx.ws_msg_buf = NULL; }
        if (s_ctx.reply_buf)  { claw_free(s_ctx.reply_buf);  s_ctx.reply_buf  = NULL; }
        return OPRT_MALLOC_FAILED;
    }
    memset(s_ctx.rx_buf,     0, ACP_CLIENT_RX_BUF_SIZE);
    memset(s_ctx.ws_msg_buf, 0, ACP_CLIENT_WS_MSG_BUF_SIZE);
    memset(s_ctx.reply_buf,  0, ACP_CLIENT_REPLY_BUF_SIZE);

    OPERATE_RET rt = tal_mutex_create_init(&s_ctx.tx_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("acp mutex create failed rt=%d", rt);
        return rt;
    }

    rt = tal_mutex_create_init(&s_ctx.state_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("acp state_mutex create failed rt=%d", rt);
        return rt;
    }

    rt = tal_semaphore_create_init(&s_ctx.recv_sem, 0, 1);
    if (rt != OPRT_OK) {
        PR_ERR("acp recv_sem create failed rt=%d", rt);
        return rt;
    }

#if defined(ACP_CLIENT_STACK_SIZE)
    THREAD_CFG_T cfg = {0};
    cfg.stackDepth = ACP_CLIENT_STACK_SIZE;
    cfg.priority   = THREAD_PRIO_1;
    cfg.thrdname   = "acp_client";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    cfg.psram_mode = 1;
#endif

    rt = tal_thread_create_and_start(&s_ctx.thread, NULL, NULL,
                                     acp_client_task, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("acp thread create failed rt=%d", rt);
        return rt;
    }
#endif

    PR_INFO("acp client init ok host=%s port=%u",
            s_gw_host, s_gw_port);
    return OPRT_OK;
}

/**
 * @brief Initialise the ACP client and start the background task.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET acp_client_init(void)
{
#if defined(ACP_CLIENT_STACK_SIZE)
    PR_INFO("acp client init wait network...");
    tal_event_subscribe(EVENT_MQTT_CONNECTED, "acp_client_init", __acp_client_init_evt_cb, SUBSCRIBE_TYPE_NORMAL);
#endif
    return OPRT_OK;
}

/**
 * @brief Send a user message to the OpenClaw agent and trigger an LLM reply.
 *
 * @note dev2 uses chat.send directly and treats ACP as a one-question-one-answer
 *       async side channel for openclaw_ctrl.
 * @param[in] text  Message text (ASR result or user input).
 * @return OPRT_OK on success.
 */
OPERATE_RET acp_client_inject(const char *text)
{
    if (!text || text[0] == '\0') {
        return OPRT_INVALID_PARM;
    }
    if (s_ctx.state != ACP_STATE_CONNECTED || s_ctx.fd < 0) {
        PR_WARN("acp inject skipped: not connected");
        return OPRT_RESOURCE_NOT_READY;
    }

    tal_mutex_lock(s_ctx.state_mutex);
    if (s_ctx.recv_requested) {
        tal_mutex_unlock(s_ctx.state_mutex);
        PR_WARN("acp inject skipped: previous request still waiting for final reply");
        return OPRT_RESOURCE_NOT_READY;
    }
    tal_mutex_unlock(s_ctx.state_mutex);

    cJSON *root   = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    if (!root || !params) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        return OPRT_MALLOC_FAILED;
    }

    char id_str[16]       = {0};
    char idem_key[32]     = {0};
    uint32_t req_id = (unsigned)(++s_ctx.req_id_counter);
    snprintf(id_str,   sizeof(id_str),   "%lu", req_id);
    snprintf(idem_key, sizeof(idem_key), "device-%lu", req_id);

    __acp_snapshot_pending_route();

    tal_mutex_lock(s_ctx.state_mutex);
    PR_INFO("acp pending route channel=%s chat_id=%s",
            s_ctx.pending_channel[0] ? s_ctx.pending_channel : "(empty)",
            s_ctx.pending_chat_id[0] ? s_ctx.pending_chat_id : "(empty)");
    if (s_ctx.pending_channel[0] == '\0' || s_ctx.pending_chat_id[0] == '\0') {
        PR_WARN("acp pending route incomplete; async reply may not be routable");
    }
    tal_mutex_unlock(s_ctx.state_mutex);

    cJSON_AddStringToObject(root,   "type",   "req");
    cJSON_AddStringToObject(root,   "id",     id_str);
    cJSON_AddStringToObject(root,   "method", "chat.send");
    cJSON_AddStringToObject(params, "sessionKey",     s_ctx.session_key);
    cJSON_AddStringToObject(params, "message",        text);
    cJSON_AddStringToObject(params, "idempotencyKey", idem_key);
    cJSON_AddItemToObject(root, "params", params);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return OPRT_MALLOC_FAILED;
    }

    tal_mutex_lock(s_ctx.state_mutex);
    s_ctx.recv_requested = TRUE;
    s_ctx.recv_started_ms = tal_system_get_millisecond();
    tal_mutex_unlock(s_ctx.state_mutex);

    tal_mutex_lock(s_ctx.tx_mutex);
    OPERATE_RET rt = __ws_send_json(s_ctx.fd, payload);
    tal_mutex_unlock(s_ctx.tx_mutex);

    cJSON_free(payload);

    if (rt != OPRT_OK) {
        PR_ERR("acp chat.send failed rt=%d", rt);
        tal_mutex_lock(s_ctx.state_mutex);
        __acp_clear_pending_locked();
        tal_mutex_unlock(s_ctx.state_mutex);
    } else {
        PR_INFO("acp chat.send sent idem=%s: %.64s", idem_key, text);
        tal_semaphore_post(s_ctx.recv_sem);
    }
    return rt;
}

/**
 * @brief Stop the ACP client and close the connection.
 * @return OPRT_OK on success.
 */
OPERATE_RET acp_client_stop(void)
{
    s_ctx.stop_requested = TRUE;
    __disconnect();
    PR_INFO("acp client stop requested");
    return OPRT_OK;
}

/**
 * @brief Query whether the ACP session is currently connected.
 *
 * Thread-safe: reads a single enum value that is only written from
 * the acp_client task under the receive loop.
 *
 * @return TRUE  if the ACP session is established (hello-ok received).
 * @return FALSE if disconnected or still connecting.
 */
bool acp_client_is_connected(void)
{
    return (s_ctx.state == ACP_STATE_CONNECTED) ? TRUE : FALSE;
}

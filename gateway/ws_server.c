/**
 * @file ws_server.c
 * @brief WebSocket server implementation for TuyaOpenClaw gateway
 *
 * Ported from TuyaOpen/apps/mimiclaw/gateway/ws_server.c.
 * Key adaptations:
 *   - MIMI_LOG* → PR_INFO / PR_WARN / PR_ERR / PR_DEBUG
 *   - MIMI_WS_* → CLAW_WS_* (defined in ws_server.h)
 *   - Inbound text messages are injected into the message_bus (channel="ws")
 *     so they flow through agent_loop like Feishu / cron messages.
 *   - Removed mimi_config.h / mimi_base.h dependencies
 */

#include "ws_server.h"
#include "tuya_app_config.h"
#include "app_base_config.h"
#include "tool_files.h"
#include "sys_bus.h"
#include "cJSON.h"
#include "mix_method.h"
#include "tal_hash.h"
#include "tal_kv.h"
#include "tal_log.h"
#include "tal_mutex.h"
#include "tal_network.h"
#include "tal_system.h"
#include "tal_thread.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  client structure                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int     fd;
    BOOL_T  active;
    BOOL_T  handshake_done;
    char    chat_id[96];
    /* PSRAM-allocated at first ws_clients_init() and kept across resets.
     * Previously inline rx_buf[4096] × CLAW_WS_MAX_CLIENTS = 16 KiB .bss. */
    uint8_t *rx_buf;       /* CLAW_WS_CLIENT_RX_BUF_SIZE bytes */
    size_t  rx_len;
} ws_client_t;

/* ------------------------------------------------------------------ */
/*  module-level state                                                 */
/* ------------------------------------------------------------------ */

static THREAD_HANDLE   s_ws_thread  = NULL;
static MUTEX_HANDLE    s_ws_mutex   = NULL;
static volatile BOOL_T s_ws_running = FALSE;
static int             s_listen_fd  = -1;
static ws_client_t     s_clients[CLAW_WS_MAX_CLIENTS];

/* ---- authentication token ---- */
static char s_ws_token[CLAW_WS_TOKEN_MAX_LEN] = {0};

/* ------------------------------------------------------------------ */
/*  client helpers                                                     */
/* ------------------------------------------------------------------ */

static void ws_reset_client(ws_client_t *client)
{
    if (!client) {
        return;
    }
    /* Keep rx_buf (PSRAM) across resets — wipe payload, drop runtime state. */
    uint8_t *kept_rx = client->rx_buf;
    memset(client, 0, sizeof(ws_client_t));
    client->rx_buf = kept_rx;
    client->fd     = -1;
    if (client->rx_buf) {
        memset(client->rx_buf, 0, CLAW_WS_CLIENT_RX_BUF_SIZE);
    }
}

static void ws_clients_init(void)
{
    for (int i = 0; i < CLAW_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].rx_buf) {
            /* Lazy one-shot allocation in PSRAM. Was 4 KiB inline × N clients
             * in .bss; now lives on PSRAM heap and is kept for the lifetime
             * of the process. */
            s_clients[i].rx_buf = (uint8_t *)claw_malloc(CLAW_WS_CLIENT_RX_BUF_SIZE);
            if (!s_clients[i].rx_buf) {
                PR_ERR("ws_clients_init: rx_buf alloc failed for client %d size=%d",
                       i, (int)CLAW_WS_CLIENT_RX_BUF_SIZE);
                /* Continue — connections to this slot will fail later. */
            }
        }
        ws_reset_client(&s_clients[i]);
    }
}

static void ws_close_fd(int *fd)
{
    if (!fd || *fd < 0) {
        return;
    }
    tal_net_close(*fd);
    *fd = -1;
}

static void ws_close_client_locked(ws_client_t *client)
{
    if (!client || !client->active) {
        return;
    }
    PR_INFO("client disconnected chat_id=%s fd=%d",
            client->chat_id[0] ? client->chat_id : "unknown", client->fd);
    ws_close_fd(&client->fd);
    ws_reset_client(client);
}

static ws_client_t *ws_find_client_by_chat_id_locked(const char *chat_id)
{
    if (!chat_id || !chat_id[0]) {
        return NULL;
    }
    for (int i = 0; i < CLAW_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            continue;
        }
        if (strncmp(s_clients[i].chat_id, chat_id, sizeof(s_clients[i].chat_id)) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  HTTP header helpers                                                */
/* ------------------------------------------------------------------ */

static int ws_find_header_end(const uint8_t *buf, size_t len)
{
    if (!buf || len < 4) {
        return -1;
    }
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}

static bool ws_get_header_value(const char *headers, const char *name,
                                char *value, size_t value_size)
{
    if (!headers || !name || !value || value_size == 0) {
        return false;
    }

    size_t      name_len = strlen(name);
    const char *line     = headers;
    while (*line) {
        const char *line_end = strstr(line, "\r\n");
        size_t      line_len = line_end ? (size_t)(line_end - line) : strlen(line);
        if (line_len == 0) {
            break;
        }

        if (line_len > name_len &&
            tuya_strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *val = line + name_len + 1;
            while (*val == ' ' || *val == '\t') {
                val++;
            }
            size_t copy_len = line_len - (size_t)(val - line);
            if (copy_len > value_size - 1) {
                copy_len = value_size - 1;
            }
            memcpy(value, val, copy_len);
            value[copy_len] = '\0';
            return true;
        }

        if (!line_end) {
            break;
        }
        line = line_end + 2;
    }
    return false;
}

/**
 * @brief Extract the value of the "token" query parameter from an HTTP request line
 *
 * Searches for "token=" in the raw HTTP header string and copies the value
 * (terminated by space, '&', '\r', or '\n') into token_buf.
 *
 * @param[in]  hdr        Raw HTTP headers (null-terminated)
 * @param[out] token_buf  Buffer to receive the extracted token
 * @param[in]  buf_size   Size of token_buf including null terminator
 * @return true if a non-empty token was found, false otherwise
 */
static bool ws_extract_url_token(const char *hdr,
                                 char *token_buf, size_t buf_size)
{
    if (!hdr || !token_buf || buf_size == 0) {
        return false;
    }
    token_buf[0] = '\0';

    const char *tok = strstr(hdr, "token=");
    if (!tok) {
        return false;
    }
    tok += 6; /* skip "token=" */

    size_t i = 0;
    while (tok[i] && tok[i] != ' ' && tok[i] != '&'
           && tok[i] != '\r' && tok[i] != '\n'
           && i < buf_size - 1) {
        token_buf[i] = tok[i];
        i++;
    }
    token_buf[i] = '\0';
    return (i > 0);
}

/* ------------------------------------------------------------------ */
/*  WebSocket send helpers                                             */
/* ------------------------------------------------------------------ */

static OPERATE_RET ws_send_all(int fd, const uint8_t *buf, size_t len)
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

static OPERATE_RET ws_send_frame(int fd, uint8_t opcode,
                                 const uint8_t *payload, size_t payload_len)
{
    uint8_t  header[14] = {0};
    size_t   header_len = 0;
    uint64_t plen64     = (uint64_t)payload_len;

    header[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (payload_len <= 125) {
        header[1]  = (uint8_t)payload_len;
        header_len = 2;
    } else if (payload_len <= 0xFFFF) {
        header[1]  = 126;
        header[2]  = (uint8_t)((payload_len >> 8) & 0xFF);
        header[3]  = (uint8_t)(payload_len & 0xFF);
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)((plen64 >> (56 - i * 8)) & 0xFF);
        }
        header_len = 10;
    }

    OPERATE_RET rt = ws_send_all(fd, header, header_len);
    if (rt != OPRT_OK) {
        return rt;
    }
    if (payload_len > 0) {
        rt = ws_send_all(fd, payload, payload_len);
    }
    return rt;
}

/* ------------------------------------------------------------------ */
/*  WebSocket handshake                                                */
/* ------------------------------------------------------------------ */

static OPERATE_RET ws_build_accept_key(const char *client_key,
                                       char *accept_key, size_t accept_size)
{
    static const char ws_guid[]    = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char              key_cat[128] = {0};
    uint8_t           sha1[20]     = {0};
    char              b64[64]      = {0};

    if (!client_key || !accept_key || accept_size == 0) {
        return OPRT_INVALID_PARM;
    }

    int n = snprintf(key_cat, sizeof(key_cat), "%s%s", client_key, ws_guid);
    if (n <= 0 || (size_t)n >= sizeof(key_cat)) {
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    OPERATE_RET rt = tal_sha1_ret((const uint8_t *)key_cat, strlen(key_cat), sha1);
    if (rt != OPRT_OK) {
        return rt;
    }

    if (!tuya_base64_encode(sha1, b64, sizeof(sha1))) {
        return OPRT_COM_ERROR;
    }

    snprintf(accept_key, accept_size, "%s", b64);
    return OPRT_OK;
}

static OPERATE_RET ws_do_handshake_locked(ws_client_t *client)
{
    if (!client || !client->active) {
        return OPRT_INVALID_PARM;
    }

    int hdr_end = ws_find_header_end(client->rx_buf, client->rx_len);
    if (hdr_end < 0) {
        return OPRT_RESOURCE_NOT_READY;
    }

    client->rx_buf[hdr_end - 1] = '\0';
    const char *hdr             = (const char *)client->rx_buf;

    /* ---- Token authentication ---- */
    if (s_ws_token[0] != '\0') {
        char client_token[CLAW_WS_TOKEN_MAX_LEN] = {0};
        if (!ws_extract_url_token(hdr, client_token, sizeof(client_token)) ||
            strcmp(client_token, s_ws_token) != 0) {
            PR_WARN("ws auth failed fd=%d", client->fd);
            const char *resp_403 =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            (void)ws_send_all(client->fd,
                              (const uint8_t *)resp_403, strlen(resp_403));
            return OPRT_AUTHENTICATION_FAIL;
        }
        PR_DEBUG("ws auth ok fd=%d", client->fd);
    }

    char ws_key[64] = {0};
    if (!ws_get_header_value(hdr, "Sec-WebSocket-Key", ws_key, sizeof(ws_key))) {
        PR_WARN("handshake missing websocket header fd=%d", client->fd);
        return OPRT_CJSON_GET_ERR;
    }

    char        accept_key[64] = {0};
    OPERATE_RET rt             = ws_build_accept_key(ws_key, accept_key, sizeof(accept_key));
    if (rt != OPRT_OK) {
        return rt;
    }

    char *resp = (char *)claw_malloc(256);
    if (!resp) {
        return OPRT_MALLOC_FAILED;
    }
    memset(resp, 0, 256);
    int  n         = snprintf(resp, 256,
                              "HTTP/1.1 101 Switching Protocols\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: %s\r\n\r\n",
                              accept_key);
    if (n <= 0 || (size_t)n >= 256) {
        claw_free(resp);
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    rt = ws_send_all(client->fd, (const uint8_t *)resp, (size_t)n);
    if (rt != OPRT_OK) {
        claw_free(resp);
        return rt;
    }

    client->handshake_done = TRUE;
    if (client->chat_id[0] == '\0') {
        snprintf(client->chat_id, sizeof(client->chat_id), "ws_%d", client->fd);
    }

    size_t remain = client->rx_len - (size_t)hdr_end;
    if (remain > 0) {
        memmove(client->rx_buf, client->rx_buf + hdr_end, remain);
    }
    client->rx_len = remain;

    PR_INFO("handshake success chat_id=%s fd=%d", client->chat_id, client->fd);
    claw_free(resp);
    return OPRT_OK;
}

/* ------------------------------------------------------------------ */
/*  WebSocket frame decode                                             */
/* ------------------------------------------------------------------ */

static OPERATE_RET ws_decode_one_frame(ws_client_t *client,
                                       uint8_t *opcode, uint8_t **payload,
                                       size_t *payload_len, size_t *consumed)
{
    if (!client || !opcode || !payload || !payload_len || !consumed) {
        return OPRT_INVALID_PARM;
    }

    if (client->rx_len < 2) {
        return OPRT_RESOURCE_NOT_READY;
    }

    const uint8_t *buf    = client->rx_buf;
    uint8_t        op     = (uint8_t)(buf[0] & 0x0F);
    bool           masked = (buf[1] & 0x80) != 0;
    uint64_t       plen   = (uint64_t)(buf[1] & 0x7F);
    size_t         off    = 2;

    if (plen == 126) {
        if (client->rx_len < off + 2) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = (uint64_t)((buf[off] << 8) | buf[off + 1]);
        off += 2;
    } else if (plen == 127) {
        if (client->rx_len < off + 8) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | buf[off + i];
        }
        off += 8;
    }

    if (plen > (uint64_t)(((size_t)CLAW_WS_CLIENT_RX_BUF_SIZE) - 16)) {
        return OPRT_MSG_OUT_OF_LIMIT;
    }

    if (masked) {
        if (client->rx_len < off + 4) {
            return OPRT_RESOURCE_NOT_READY;
        }
    }

    size_t frame_len = off + (masked ? 4 : 0) + (size_t)plen;
    if (client->rx_len < frame_len) {
        return OPRT_RESOURCE_NOT_READY;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        memcpy(mask, buf + off, sizeof(mask));
        off += sizeof(mask);
    }

    uint8_t *data = claw_malloc((size_t)plen + 1);
    if (!data) {
        return OPRT_MALLOC_FAILED;
    }

    if (plen > 0) {
        memcpy(data, buf + off, (size_t)plen);
        if (masked) {
            for (size_t i = 0; i < (size_t)plen; i++) {
                data[i] = (uint8_t)(data[i] ^ mask[i % 4]);
            }
        }
    }
    data[plen] = '\0';

    *opcode      = op;
    *payload     = data;
    *payload_len = (size_t)plen;
    *consumed    = frame_len;
    return OPRT_OK;
}

static void ws_consume_rx(ws_client_t *client, size_t consumed)
{
    if (!client || consumed == 0 || consumed > client->rx_len) {
        return;
    }
    if (consumed < client->rx_len) {
        memmove(client->rx_buf, client->rx_buf + consumed, client->rx_len - consumed);
    }
    client->rx_len -= consumed;
}

/* ------------------------------------------------------------------ */
/*  inbound message handling                                           */
/* ------------------------------------------------------------------ */

static void ws_handle_text_message_locked(ws_client_t *client,
                                          const uint8_t *payload,
                                          size_t payload_len)
{
    if (!client || !payload || payload_len == 0) {
        return;
    }

    cJSON *root = cJSON_Parse((const char *)payload);
    if (!root) {
        PR_WARN("invalid ws json chat_id=%s", client->chat_id);
        return;
    }

    cJSON *type    = cJSON_GetObjectItem(root, "type");
    cJSON *content = cJSON_GetObjectItem(root, "content");
    cJSON *chat_id = cJSON_GetObjectItem(root, "chat_id");

    if (!cJSON_IsString(type) || !type->valuestring ||
        strcmp(type->valuestring, "message") != 0 ||
        !cJSON_IsString(content) || !content->valuestring) {
        cJSON_Delete(root);
        return;
    }

    /* Update chat_id if client supplied one.
     * Sanitise the user-supplied string: reject values containing '%'
     * to prevent any potential format-string issues downstream. */
    if (cJSON_IsString(chat_id) && chat_id->valuestring && chat_id->valuestring[0]) {
        if (!strchr(chat_id->valuestring, '%')) {
            snprintf(client->chat_id, sizeof(client->chat_id), "%s", chat_id->valuestring);
        } else {
            PR_WARN("chat_id contains forbidden '%%' character, ignored");
        }
    }

    /* Forward message text to the AI agent */
    PR_INFO("ws inbound chat_id=%s content=%.64s...", client->chat_id, content->valuestring);

    size_t clen = strlen(content->valuestring);
    char  *cbuf = claw_malloc(clen + 1);
    if (!cbuf) {
        PR_ERR("ws: malloc failed for inbound content");
        cJSON_Delete(root);
        return;
    }
    memcpy(cbuf, content->valuestring, clen);
    cbuf[clen] = '\0';

    sys_msg_t msg = {0};
    snprintf(msg.channel, sizeof(msg.channel), "%s", SYS_CHAN_WS);
    snprintf(msg.chat_id, sizeof(msg.chat_id), "%s", client->chat_id);
    msg.content = cbuf;

    if (sys_bus_push_inbound(&msg) != OPRT_OK) {
        PR_ERR("ws: sys_bus_push_inbound failed");
        claw_free(cbuf);
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/*  client buffer processing                                           */
/* ------------------------------------------------------------------ */

static void ws_process_client_buffer_locked(ws_client_t *client)
{
    if (!client || !client->active) {
        return;
    }

    if (!client->handshake_done) {
        OPERATE_RET hs = ws_do_handshake_locked(client);
        if (hs == OPRT_RESOURCE_NOT_READY) {
            return;
        }
        if (hs != OPRT_OK) {
            ws_close_client_locked(client);
            return;
        }
    }

    while (client->active && client->rx_len > 0) {
        uint8_t  opcode      = 0;
        uint8_t *payload     = NULL;
        size_t   payload_len = 0;
        size_t   consumed    = 0;

        OPERATE_RET rt = ws_decode_one_frame(client, &opcode, &payload,
                                             &payload_len, &consumed);
        if (rt == OPRT_RESOURCE_NOT_READY) {
            break;
        }
        if (rt != OPRT_OK || consumed == 0) {
            claw_free(payload);
            ws_close_client_locked(client);
            break;
        }

        ws_consume_rx(client, consumed);

        if (opcode == 0x1) {
            /* text frame */
            ws_handle_text_message_locked(client, payload, payload_len);
        } else if (opcode == 0x8) {
            /* close frame */
            (void)ws_send_frame(client->fd, 0x8, payload, payload_len);
            claw_free(payload);
            ws_close_client_locked(client);
            break;
        } else if (opcode == 0x9) {
            /* ping → pong */
            (void)ws_send_frame(client->fd, 0xA, payload, payload_len);
        }

        claw_free(payload);
    }
}

/* ------------------------------------------------------------------ */
/*  accept new client                                                  */
/* ------------------------------------------------------------------ */

static void ws_accept_client_locked(void)
{
    TUYA_IP_ADDR_T addr = 0;
    uint16_t       port = 0;
    int            fd   = tal_net_accept(s_listen_fd, &addr, &port);
    if (fd < 0) {
        return;
    }

    (void)tal_net_set_reuse(fd);
    (void)tal_net_set_block(fd, FALSE);

    ws_client_t *slot = NULL;
    for (int i = 0; i < CLAW_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            slot = &s_clients[i];
            break;
        }
    }

    if (!slot) {
        PR_WARN("max clients reached, reject fd=%d", fd);
        tal_net_close(fd);
        return;
    }

    ws_reset_client(slot);
    slot->fd     = fd;
    slot->active = TRUE;
    snprintf(slot->chat_id, sizeof(slot->chat_id), "ws_%d", fd);
    PR_INFO("client accepted fd=%d ip=%s:%u", fd, tal_net_addr2str(addr), (unsigned)port);
}

/* ------------------------------------------------------------------ */
/*  server task (select loop)                                          */
/* ------------------------------------------------------------------ */

static void ws_server_task(void *arg)
{
    (void)arg;
    PR_INFO("ws server task started");

    while (s_ws_running) {
        TUYA_FD_SET_T readfds;
        TAL_FD_ZERO(&readfds);
        int maxfd = -1;

        tal_mutex_lock(s_ws_mutex);
        if (s_listen_fd >= 0) {
            TAL_FD_SET(s_listen_fd, &readfds);
            maxfd = s_listen_fd;
        }
        for (int i = 0; i < CLAW_WS_MAX_CLIENTS; i++) {
            if (!s_clients[i].active || s_clients[i].fd < 0) {
                continue;
            }
            TAL_FD_SET(s_clients[i].fd, &readfds);
            if (s_clients[i].fd > maxfd) {
                maxfd = s_clients[i].fd;
            }
        }
        tal_mutex_unlock(s_ws_mutex);

        if (maxfd < 0) {
            tal_system_sleep(50);
            continue;
        }

        int ready = tal_net_select(maxfd + 1, &readfds, NULL, NULL, 200);
        if (ready <= 0) {
            continue;
        }

        tal_mutex_lock(s_ws_mutex);

        if (s_listen_fd >= 0 && TAL_FD_ISSET(s_listen_fd, &readfds)) {
            ws_accept_client_locked();
        }

        for (int i = 0; i < CLAW_WS_MAX_CLIENTS; i++) {
            ws_client_t *client = &s_clients[i];
            if (!client->active || client->fd < 0) {
                continue;
            }
            if (!TAL_FD_ISSET(client->fd, &readfds)) {
                continue;
            }

            if (client->rx_len >= ((size_t)CLAW_WS_CLIENT_RX_BUF_SIZE)) {
                ws_close_client_locked(client);
                continue;
            }

            int n = tal_net_recv(client->fd,
                                 client->rx_buf + client->rx_len,
                                 (uint32_t)(((size_t)CLAW_WS_CLIENT_RX_BUF_SIZE) - client->rx_len));
            if (n == OPRT_RESOURCE_NOT_READY) {
                continue;
            }
            if (n <= 0) {
                ws_close_client_locked(client);
                continue;
            }

            client->rx_len += (size_t)n;
            ws_process_client_buffer_locked(client);
        }

        tal_mutex_unlock(s_ws_mutex);
    }

    PR_INFO("ws server task stopped");
}

/* ------------------------------------------------------------------ */
/*  send text response to a client                                     */
/* ------------------------------------------------------------------ */

static OPERATE_RET ws_send_text_to_client_locked(ws_client_t *client,
                                                 const char *chat_id,
                                                 const char *text)
{
    if (!client || !chat_id || !text) {
        return OPRT_INVALID_PARM;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return OPRT_MALLOC_FAILED;
    }
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *payload = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!payload) {
        return OPRT_MALLOC_FAILED;
    }

    OPERATE_RET rt = ws_send_frame(client->fd, 0x1,
                                   (const uint8_t *)payload, strlen(payload));
    cJSON_free(payload);
    return rt;
}

/* ================================================================== */
/*  PUBLIC API                                                         */
/* ================================================================== */

/**
 * @brief Load the authentication token from compile-time macro and/or KV storage
 *
 * Priority: KV storage (runtime) > CLAW_WS_AUTH_TOKEN (compile-time).
 * An empty token disables authentication entirely.
 *
 * @return none
 */
static void ws_load_auth_token(void)
{
    /* 1. Seed from compile-time default */
    if (CLAW_WS_AUTH_TOKEN[0] != '\0') {
        snprintf(s_ws_token, sizeof(s_ws_token), "%s", CLAW_WS_AUTH_TOKEN);
    }

    /* 2. Override with runtime KV value if present */
    uint8_t *kv_val = NULL;
    size_t   kv_len = 0;
    if (tal_kv_get(CLAW_WS_TOKEN_KV_KEY, &kv_val, &kv_len) == OPRT_OK && kv_val) {
        if (kv_val[0] != '\0') {
            snprintf(s_ws_token, sizeof(s_ws_token), "%s", (char *)kv_val);
        }
        tal_kv_free(kv_val);
    }

    if (s_ws_token[0] != '\0') {
        PR_INFO("ws auth token loaded (len=%u)", (unsigned)strlen(s_ws_token));
    } else {
        PR_WARN("ws auth token is EMPTY, authentication disabled");
    }
}

OPERATE_RET ws_server_start(void)
{
    if (s_ws_thread) {
        return OPRT_OK;
    }

    ws_load_auth_token();

    /* If token is empty, skip initialization */
    if (s_ws_token[0] == '\0') {
        PR_WARN("ws_server: token is empty, skip initialization");
        return OPRT_OK;
    }

    if (!s_ws_mutex) {
        OPERATE_RET rt = tal_mutex_create_init(&s_ws_mutex);
        if (rt != OPRT_OK) {
            PR_ERR("create ws mutex failed rt=%d", rt);
            return rt;
        }
    }

    ws_clients_init();

    s_listen_fd = tal_net_socket_create(PROTOCOL_TCP);
    if (s_listen_fd < 0) {
        PR_ERR("create ws socket failed");
        return OPRT_SOCK_ERR;
    }

    (void)tal_net_set_reuse(s_listen_fd);
    (void)tal_net_set_block(s_listen_fd, FALSE);

    if (tal_net_bind(s_listen_fd, TY_IPADDR_ANY, CLAW_WS_PORT) < 0) {
        PR_ERR("bind ws failed port=%d errno=%d", CLAW_WS_PORT, tal_net_get_errno());
        ws_close_fd(&s_listen_fd);
        return OPRT_SOCK_ERR;
    }

    if (tal_net_listen(s_listen_fd, CLAW_WS_MAX_CLIENTS) < 0) {
        PR_ERR("listen ws failed errno=%d", tal_net_get_errno());
        ws_close_fd(&s_listen_fd);
        return OPRT_SOCK_ERR;
    }

    THREAD_CFG_T cfg = {0};
#ifdef WS_SERVER_STACK_SIZE
    cfg.stackDepth   = WS_SERVER_STACK_SIZE;
#else
    cfg.stackDepth   = 10 * 1024;
#endif
    cfg.priority     = THREAD_PRIO_1;
    cfg.thrdname     = "claw_ws";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    cfg.psram_mode = 1;
#endif

    s_ws_running   = TRUE;
    OPERATE_RET rt = tal_thread_create_and_start(&s_ws_thread, NULL, NULL,
                                                 ws_server_task, NULL, &cfg);
    if (rt != OPRT_OK) {
        s_ws_running = FALSE;
        ws_close_fd(&s_listen_fd);
        PR_ERR("create ws thread failed rt=%d", rt);
        return rt;
    }

    PR_INFO("ws server started port=%d", CLAW_WS_PORT);
    return OPRT_OK;
}

OPERATE_RET ws_server_send(const char *chat_id, const char *text)
{
    if (!chat_id || !text) {
        return OPRT_INVALID_PARM;
    }

    if (!s_ws_running || !s_ws_thread) {
        return OPRT_RESOURCE_NOT_READY;
    }

    tal_mutex_lock(s_ws_mutex);
    ws_client_t *client = ws_find_client_by_chat_id_locked(chat_id);
    if (!client || !client->handshake_done) {
        tal_mutex_unlock(s_ws_mutex);
        return OPRT_NOT_FOUND;
    }

    OPERATE_RET rt = ws_send_text_to_client_locked(client, chat_id, text);
    if (rt != OPRT_OK) {
        PR_WARN("send ws response failed chat_id=%s rt=%d", chat_id, rt);
        ws_close_client_locked(client);
    }
    tal_mutex_unlock(s_ws_mutex);
    return rt;
}

OPERATE_RET ws_server_stop(void)
{
    s_ws_running = FALSE;

    if (s_ws_thread) {
        tal_thread_delete(s_ws_thread);
        s_ws_thread = NULL;
    }

    if (!s_ws_mutex) {
        ws_close_fd(&s_listen_fd);
        return OPRT_OK;
    }

    for (int i = 0; i < CLAW_WS_MAX_CLIENTS; i++) {
        ws_close_client_locked(&s_clients[i]);
    }
    ws_close_fd(&s_listen_fd);
    tal_mutex_release(s_ws_mutex);
    s_ws_mutex = NULL;
    ws_clients_init();

    return OPRT_OK;
}

OPERATE_RET ws_server_set_token(const char *token)
{
    if (!token) {
        return OPRT_INVALID_PARM;
    }

    if (!s_ws_mutex) {
        return OPRT_RESOURCE_NOT_READY;
    }

    tal_mutex_lock(s_ws_mutex);
    snprintf(s_ws_token, sizeof(s_ws_token), "%s", token);
    tal_mutex_unlock(s_ws_mutex);

    /* Persist to KV so the token survives reboots */
    OPERATE_RET rt = tal_kv_set(CLAW_WS_TOKEN_KV_KEY,
                                (const uint8_t *)token,
                                strlen(token) + 1);
    if (rt != OPRT_OK) {
        PR_WARN("ws token kv persist failed rt=%d", rt);
    }
    return rt;
}

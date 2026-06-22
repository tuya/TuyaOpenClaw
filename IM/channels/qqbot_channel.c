#include "channels/qqbot_channel.h"

#include "bus/message_bus.h"
#include "cJSON.h"
#include "im_config.h"
#include "im_utils.h"
#include "proxy/http_proxy.h"
#include "certs/tls_cert_bundle.h"
#include "tuya_transporter.h"
#include "tuya_tls.h"

#include "mbedtls/ssl.h"
#include "tal_network.h"

#include <inttypes.h>
#include <limits.h>

static const char *TAG = "qqbot";

/* ================================================================
 * Constants
 * ================================================================ */

#define QQ_TOKEN_HOST          IM_QQ_TOKEN_HOST
#define QQ_API_HOST            IM_QQ_API_HOST
#define QQ_WS_HOST             "api.sgroup.qq.com"
#define QQ_WS_PATH             "/websocket"
#define QQ_HTTP_TIMEOUT_MS     10000
#define QQ_HTTP_RESP_BUF       (4 * 1024)
#define QQ_WS_RX_BUF_SIZE      IM_QQ_GATEWAY_RX_BUF_SIZE
#define QQ_RECONNECT_BASE_MS   IM_QQ_FAIL_BASE_MS
#define QQ_RECONNECT_MAX_MS    IM_QQ_FAIL_MAX_MS
#define QQ_TOKEN_FAIL_BASE_MS  IM_QQ_TOKEN_FAIL_BASE_MS
#define QQ_TOKEN_FAIL_MAX_MS   IM_QQ_TOKEN_FAIL_MAX_MS
#define QQ_DEDUP_CACHE_SIZE    64
#define QQ_PROXY_READ_SLICE_MS 1000
#define QQ_PROXY_READ_TOTAL_MS 15000

/* ================================================================
 * Module State
 * ================================================================ */

static char          s_app_id[64]        = {0};
static char          s_client_secret[64] = {0};
static char          s_access_token[256] = {0};
static char          s_bot_openid[64]    = {0};
static char          s_session_id[64]    = {0};
static int64_t       s_last_seq          = -1;
static MUTEX_HANDLE  s_token_mutex       = NULL;
static THREAD_HANDLE s_token_thread      = NULL;
static THREAD_HANDLE s_ws_thread         = NULL;

static uint64_t s_seen_msg_keys[QQ_DEDUP_CACHE_SIZE] = {0};
static size_t   s_seen_msg_idx                       = 0;

/* ================================================================
 * Connection Struct
 * ================================================================ */

typedef enum {
    QQ_CONN_NONE = 0,
    QQ_CONN_PROXY,
    QQ_CONN_DIRECT,
} qq_conn_mode_t;

typedef struct {
    qq_conn_mode_t     mode;
    proxy_conn_t      *proxy;
    tuya_transporter_t tcp;
    tuya_tls_hander    tls;
    int                socket_fd;
    uint8_t           *rx_buf;
    size_t             rx_cap;
    size_t             rx_len;
} qq_conn_t;

/* ================================================================
 * Connection Helpers
 * ================================================================ */

static OPERATE_RET qq_direct_open(qq_conn_t *conn, const char *host, int port, int timeout_ms)
{
    if (!conn || !host || port <= 0 || timeout_ms <= 0) {
        return OPRT_INVALID_PARM;
    }

    conn->tcp = tuya_transporter_create(TRANSPORT_TYPE_TCP, NULL);
    if (!conn->tcp) {
        return OPRT_COM_ERROR;
    }
    conn->mode = QQ_CONN_DIRECT;

    tuya_tcp_config_t cfg = {0};
    cfg.isReuse           = TRUE;
    cfg.isDisableNagle    = TRUE;
    cfg.sendTimeoutMs     = timeout_ms;
    cfg.recvTimeoutMs     = timeout_ms;
    (void)tuya_transporter_ctrl(conn->tcp, TUYA_TRANSPORTER_SET_TCP_CONFIG, &cfg);

    OPERATE_RET rt = tuya_transporter_connect(conn->tcp, (char *)host, port, timeout_ms);
    if (rt != OPRT_OK) {
        return rt;
    }

    rt = tuya_transporter_ctrl(conn->tcp, TUYA_TRANSPORTER_GET_TCP_SOCKET, &conn->socket_fd);
    if (rt != OPRT_OK || conn->socket_fd < 0) {
        return OPRT_SOCK_ERR;
    }

    uint8_t *cacert     = NULL;
    size_t   cacert_len = 0;
    bool     verify     = false;

    rt = im_tls_query_domain_certs(host, &cacert, &cacert_len);
    if (rt == OPRT_OK && cacert && cacert_len > 0) {
        verify = true;
    } else {
        IM_LOGD(TAG, "tls cert unavailable for %s, fallback no-verify rt=%d", host, rt);
    }
    if (verify && cacert_len > (size_t)INT_MAX) {
        IM_LOGW(TAG, "tls cert too large for %s, fallback no-verify", host);
        verify = false;
    }

    conn->tls = tuya_tls_connect_create();
    if (!conn->tls) {
        if (cacert) {
            im_free(cacert);
        }
        return OPRT_MALLOC_FAILED;
    }

    int timeout_s = timeout_ms / 1000;
    if (timeout_s <= 0) {
        timeout_s = 1;
    }

    tuya_tls_config_t cfg_tls = {
        .mode         = TUYA_TLS_SERVER_CERT_MODE,
        .hostname     = (char *)host,
        .port         = (uint16_t)port,
        .timeout      = (uint32_t)timeout_s,
        .verify       = verify,
        .ca_cert      = verify ? (char *)cacert : NULL,
        .ca_cert_size = verify ? (int)cacert_len : 0,
    };
    (void)tuya_tls_config_set(conn->tls, &cfg_tls);

    rt = tuya_tls_connect(conn->tls, (char *)host, port, conn->socket_fd, timeout_s);
    if (cacert) {
        im_free(cacert);
    }
    if (rt != OPRT_OK) {
        return rt;
    }

    return OPRT_OK;
}

static void qq_conn_close(qq_conn_t *conn)
{
    if (!conn) {
        return;
    }

    if (conn->mode == QQ_CONN_PROXY) {
        if (conn->proxy) {
            proxy_conn_close(conn->proxy);
            conn->proxy = NULL;
        }
    } else if (conn->mode == QQ_CONN_DIRECT) {
        if (conn->tls) {
            (void)tuya_tls_disconnect(conn->tls);
            tuya_tls_connect_destroy(conn->tls);
            conn->tls = NULL;
        }
        if (conn->tcp) {
            (void)tuya_transporter_close(conn->tcp);
            (void)tuya_transporter_destroy(conn->tcp);
            conn->tcp = NULL;
        }
        conn->socket_fd = -1;
    }

    conn->mode = QQ_CONN_NONE;
    if (conn->rx_buf) {
        im_free(conn->rx_buf);
        conn->rx_buf = NULL;
    }
    conn->rx_cap = 0;
    conn->rx_len = 0;
}

static OPERATE_RET qq_conn_open(qq_conn_t *conn, const char *host, int port, int timeout_ms)
{
    if (!conn || !host || port <= 0 || timeout_ms <= 0) {
        return OPRT_INVALID_PARM;
    }
    memset(conn, 0, sizeof(*conn));
    conn->socket_fd = -1;

    if (http_proxy_is_enabled()) {
        conn->proxy = proxy_conn_open(host, port, timeout_ms);
        if (!conn->proxy) {
            return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
        }
        conn->mode = QQ_CONN_PROXY;
        return OPRT_OK;
    }

    OPERATE_RET rt = qq_direct_open(conn, host, port, timeout_ms);
    if (rt != OPRT_OK) {
        qq_conn_close(conn);
        return rt;
    }
    return OPRT_OK;
}

static int qq_conn_write(qq_conn_t *conn, const uint8_t *data, int len)
{
    if (!conn || !data || len <= 0) {
        return -1;
    }

    if (conn->mode == QQ_CONN_PROXY) {
        return proxy_conn_write(conn->proxy, (const char *)data, len);
    }
    if (conn->mode != QQ_CONN_DIRECT || !conn->tls) {
        return -1;
    }

    int sent = 0;
    while (sent < len) {
        int n = tuya_tls_write(conn->tls, (uint8_t *)data + sent, (uint32_t)(len - sent));
        if (n <= 0) {
            return -1;
        }
        sent += n;
    }
    return sent;
}

static int qq_conn_read(qq_conn_t *conn, uint8_t *buf, int len, int timeout_ms)
{
    if (!conn || !buf || len <= 0 || timeout_ms <= 0) {
        return -1;
    }

    if (conn->mode == QQ_CONN_PROXY) {
        return proxy_conn_read(conn->proxy, (char *)buf, len, timeout_ms);
    }
    if (conn->mode != QQ_CONN_DIRECT || !conn->tls || conn->socket_fd < 0) {
        return -1;
    }

    TUYA_FD_SET_T readfds;
    tal_net_fd_zero(&readfds);
    tal_net_fd_set(conn->socket_fd, &readfds);
    int ready = tal_net_select(conn->socket_fd + 1, &readfds, NULL, NULL, timeout_ms);
    if (ready < 0) {
        return -1;
    }
    if (ready == 0) {
        return OPRT_RESOURCE_NOT_READY;
    }

    int n = tuya_tls_read(conn->tls, buf, (uint32_t)len);
    if (n > 0) {
        return n;
    }
    if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    if (n == OPRT_RESOURCE_NOT_READY || n == MBEDTLS_ERR_SSL_WANT_READ ||
        n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT || n == -100) {
        return OPRT_RESOURCE_NOT_READY;
    }
    return n;
}

/* ================================================================
 * WebSocket Frame Helpers
 * ================================================================ */

static OPERATE_RET qq_conn_ensure_rx_buf(qq_conn_t *conn, size_t min_cap)
{
    if (!conn || min_cap == 0) {
        return OPRT_INVALID_PARM;
    }
    if (conn->rx_buf && conn->rx_cap >= min_cap) {
        return OPRT_OK;
    }

    uint8_t *buf = im_malloc(min_cap);
    if (!buf) {
        return OPRT_MALLOC_FAILED;
    }
    if (conn->rx_buf) {
        im_free(conn->rx_buf);
    }
    conn->rx_buf = buf;
    conn->rx_cap = min_cap;
    conn->rx_len = 0;
    return OPRT_OK;
}

static OPERATE_RET qq_ws_send_frame(qq_conn_t *conn, uint8_t opcode, const uint8_t *payload, size_t payload_len)
{
    if (!conn || (payload_len > 0 && !payload)) {
        return OPRT_INVALID_PARM;
    }

    size_t  header_len = 0;
    uint8_t header[14] = {0};

    header[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (payload_len <= 125) {
        header[1]  = (uint8_t)(0x80 | payload_len);
        header_len = 2;
    } else if (payload_len <= 0xFFFF) {
        header[1]  = (uint8_t)(0x80 | 126);
        header[2]  = (uint8_t)((payload_len >> 8) & 0xFF);
        header[3]  = (uint8_t)(payload_len & 0xFF);
        header_len = 4;
    } else {
        header[1]       = (uint8_t)(0x80 | 127);
        uint64_t plen64 = (uint64_t)payload_len;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)((plen64 >> (56 - i * 8)) & 0xFF);
        }
        header_len = 10;
    }

    uint32_t m       = (uint32_t)tal_system_get_random(0xFFFFFFFFu);
    uint8_t  mask[4] = {
        (uint8_t)(m & 0xFF), (uint8_t)((m >> 8) & 0xFF),
        (uint8_t)((m >> 16) & 0xFF), (uint8_t)((m >> 24) & 0xFF),
    };

    size_t   frame_len = header_len + 4 + payload_len;
    uint8_t *frame     = im_malloc(frame_len);
    if (!frame) {
        return OPRT_MALLOC_FAILED;
    }

    memcpy(frame, header, header_len);
    memcpy(frame + header_len, mask, 4);
    for (size_t i = 0; i < payload_len; i++) {
        frame[header_len + 4 + i] = (uint8_t)(payload[i] ^ mask[i % 4]);
    }

    int n = qq_conn_write(conn, frame, (int)frame_len);
    im_free(frame);
    return (n == (int)frame_len) ? OPRT_OK : OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
}

static OPERATE_RET qq_ws_handshake(qq_conn_t *conn, const char *host, const char *path)
{
    if (!conn || !host || !path) {
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = qq_conn_ensure_rx_buf(conn, QQ_WS_RX_BUF_SIZE);
    if (rt != OPRT_OK) {
        return rt;
    }

    const char *ws_key   = "dGhlIHNhbXBsZSBub25jZQ==";
    char        req[512] = {0};
    int         req_len  = snprintf(req, sizeof(req),
                                    "GET %s HTTP/1.1\r\n"
                                    "Host: %s\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: %s\r\n"
                                    "Sec-WebSocket-Version: 13\r\n"
                                    "User-Agent: IM/1.0\r\n\r\n",
                                    path, host, ws_key);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        return OPRT_BUFFER_NOT_ENOUGH;
    }
    if (qq_conn_write(conn, (const uint8_t *)req, req_len) != req_len) {
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }

    char    *hdr      = (char *)im_malloc(2048);
    if (!hdr) {
        return OPRT_MALLOC_FAILED;
    }
    memset(hdr, 0, 2048);
    int      total    = 0;
    int      hdr_end  = -1;
    uint32_t start_ms = tal_system_get_millisecond();

    while ((int)(tal_system_get_millisecond() - start_ms) < QQ_HTTP_TIMEOUT_MS && total < 2048 - 1) {
        int n = qq_conn_read(conn, (uint8_t *)hdr + total, 2048 - total - 1, 1000);
        if (n == OPRT_RESOURCE_NOT_READY) {
            continue;
        }
        if (n <= 0) {
            im_free(hdr);
            return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
        }
        total += n;
        hdr[total] = '\0';
        hdr_end    = im_find_header_end(hdr, total);
        if (hdr_end > 0) {
            break;
        }
    }

    if (hdr_end <= 0) {
        im_free(hdr);
        return OPRT_TIMEOUT;
    }

    uint16_t status = im_parse_http_status(hdr);
    if (status != 101) {
        IM_LOGE(TAG, "qqbot ws handshake failed http=%u", status);
        im_free(hdr);
        return OPRT_COM_ERROR;
    }

    size_t remain = (size_t)(total - hdr_end);
    conn->rx_len  = 0;
    if (remain > 0) {
        if (remain > conn->rx_cap) {
            im_free(hdr);
            return OPRT_BUFFER_NOT_ENOUGH;
        }
        memcpy(conn->rx_buf, hdr + hdr_end, remain);
        conn->rx_len = remain;
    }

    im_free(hdr);
    IM_LOGI(TAG, "qqbot ws handshake ok");
    return OPRT_OK;
}

static OPERATE_RET qq_ws_decode_one_frame(qq_conn_t *conn, uint8_t *opcode, uint8_t **payload,
                                          size_t *payload_len, size_t *consumed)
{
    if (!conn || !opcode || !payload || !payload_len || !consumed) {
        return OPRT_INVALID_PARM;
    }
    if (!conn->rx_buf || conn->rx_cap == 0) {
        return OPRT_INVALID_PARM;
    }
    if (conn->rx_len < 2) {
        return OPRT_RESOURCE_NOT_READY;
    }

    const uint8_t *buf    = conn->rx_buf;
    bool           masked = (buf[1] & 0x80) != 0;
    uint64_t       plen   = (uint64_t)(buf[1] & 0x7F);
    size_t         off    = 2;

    if (plen == 126) {
        if (conn->rx_len < off + 2) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = (uint64_t)((buf[off] << 8) | buf[off + 1]);
        off += 2;
    } else if (plen == 127) {
        if (conn->rx_len < off + 8) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | buf[off + i];
        }
        off += 8;
    }

    if (masked && conn->rx_len < off + 4) {
        return OPRT_RESOURCE_NOT_READY;
    }
    if (conn->rx_cap <= 16 || plen > (uint64_t)(conn->rx_cap - 16)) {
        return OPRT_MSG_OUT_OF_LIMIT;
    }

    size_t frame_len = off + (masked ? 4 : 0) + (size_t)plen;
    if (conn->rx_len < frame_len) {
        return OPRT_RESOURCE_NOT_READY;
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        memcpy(mask_key, buf + off, 4);
        off += 4;
    }

    uint8_t *data = im_malloc((size_t)plen + 1);
    if (!data) {
        return OPRT_MALLOC_FAILED;
    }

    if (plen > 0) {
        memcpy(data, buf + off, (size_t)plen);
        if (masked) {
            for (size_t i = 0; i < (size_t)plen; i++) {
                data[i] ^= mask_key[i % 4];
            }
        }
    }
    data[plen] = '\0';

    *opcode      = (uint8_t)(buf[0] & 0x0F);
    *payload     = data;
    *payload_len = (size_t)plen;
    *consumed    = frame_len;
    return OPRT_OK;
}

static void qq_ws_consume_rx(qq_conn_t *conn, size_t consumed)
{
    if (!conn || consumed == 0 || consumed > conn->rx_len) {
        return;
    }
    if (consumed < conn->rx_len) {
        memmove(conn->rx_buf, conn->rx_buf + consumed, conn->rx_len - consumed);
    }
    conn->rx_len -= consumed;
}

static OPERATE_RET qq_ws_poll_frame(qq_conn_t *conn, int wait_ms, uint8_t *opcode,
                                    uint8_t **payload, size_t *payload_len)
{
    if (!conn || !opcode || !payload || !payload_len) {
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = qq_conn_ensure_rx_buf(conn, QQ_WS_RX_BUF_SIZE);
    if (rt != OPRT_OK) {
        return rt;
    }

    *payload     = NULL;
    *payload_len = 0;

    size_t consumed = 0;
    rt = qq_ws_decode_one_frame(conn, opcode, payload, payload_len, &consumed);
    if (rt == OPRT_OK) {
        qq_ws_consume_rx(conn, consumed);
        return OPRT_OK;
    }
    if (rt != OPRT_RESOURCE_NOT_READY) {
        return rt;
    }

    uint8_t *tmp = (uint8_t *)im_malloc(1024);
    if (!tmp) {
        return OPRT_MALLOC_FAILED;
    }
    memset(tmp, 0, 1024);

    int n = qq_conn_read(conn, tmp, 1024, wait_ms);
    if (n == OPRT_RESOURCE_NOT_READY) {
        im_free(tmp);
        return OPRT_RESOURCE_NOT_READY;
    }
    if (n <= 0) {
        im_free(tmp);
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }
    if (conn->rx_len + (size_t)n > conn->rx_cap) {
        im_free(tmp);
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    memcpy(conn->rx_buf + conn->rx_len, tmp, (size_t)n);
    conn->rx_len += (size_t)n;
    im_free(tmp);

    consumed = 0;
    rt = qq_ws_decode_one_frame(conn, opcode, payload, payload_len, &consumed);
    if (rt == OPRT_OK) {
        qq_ws_consume_rx(conn, consumed);
    }
    return rt;
}

/* ================================================================
 * HTTP Helper (for token fetch and message send)
 * ================================================================ */

static OPERATE_RET qq_http_post(const char *host, const char *path, const char *auth_header,
                                const char *body, char *resp_buf, size_t resp_buf_size,
                                uint16_t *status_out)
{
    if (!host || !path || !resp_buf || resp_buf_size == 0) {
        return OPRT_INVALID_PARM;
    }

    int body_len = body ? (int)strlen(body) : 0;

    if (http_proxy_is_enabled()) {
        proxy_conn_t *conn = proxy_conn_open(host, 443, QQ_HTTP_TIMEOUT_MS);
        if (!conn) {
            return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
        }

        char *req = (char *)im_malloc(1024);
        if (!req) {
            proxy_conn_close(conn);
            return OPRT_MALLOC_FAILED;
        }
        int req_len;
        if (auth_header) {
            req_len = snprintf(req, 1024,
                               "POST %s HTTP/1.1\r\nHost: %s\r\n%s\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %d\r\nConnection: close\r\n\r\n",
                               path, host, auth_header, body_len);
        } else {
            req_len = snprintf(req, 1024,
                               "POST %s HTTP/1.1\r\nHost: %s\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %d\r\nConnection: close\r\n\r\n",
                               path, host, body_len);
        }
        if (req_len <= 0 || req_len >= 1024) {
            im_free(req);
            proxy_conn_close(conn);
            return OPRT_BUFFER_NOT_ENOUGH;
        }
        if (proxy_conn_write(conn, req, req_len) != req_len) {
            im_free(req);
            proxy_conn_close(conn);
            return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
        }
        if (body_len > 0 && proxy_conn_write(conn, body, body_len) != body_len) {
            im_free(req);
            proxy_conn_close(conn);
            return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
        }
        im_free(req);

        size_t raw_cap = 4096;
        size_t raw_len = 0;
        char  *raw     = im_calloc(1, raw_cap);
        if (!raw) {
            proxy_conn_close(conn);
            return OPRT_MALLOC_FAILED;
        }

        uint32_t t0 = tal_system_get_millisecond();
        while (1) {
            if (raw_len + 1024 >= raw_cap) {
                char *tmp = im_realloc(raw, raw_cap * 2);
                if (!tmp) {
                    im_free(raw);
                    proxy_conn_close(conn);
                    return OPRT_MALLOC_FAILED;
                }
                raw     = tmp;
                raw_cap *= 2;
            }
            int n = proxy_conn_read(conn, raw + raw_len, (int)(raw_cap - raw_len - 1), QQ_PROXY_READ_SLICE_MS);
            if (n == OPRT_RESOURCE_NOT_READY) {
                if ((int)(tal_system_get_millisecond() - t0) >= QQ_PROXY_READ_TOTAL_MS) {
                    im_free(raw);
                    proxy_conn_close(conn);
                    return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
                }
                continue;
            }
            if (n < 0) {
                if (raw_len > 0) {
                    break;
                }
                im_free(raw);
                proxy_conn_close(conn);
                return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
            }
            if (n == 0) {
                break;
            }
            raw_len += (size_t)n;
            raw[raw_len] = '\0';
            t0 = tal_system_get_millisecond();
        }
        proxy_conn_close(conn);

        if (raw_len == 0) {
            im_free(raw);
            return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
        }
        if (status_out) {
            *status_out = im_parse_http_status(raw);
        }
        resp_buf[0] = '\0';
        char *b = strstr(raw, "\r\n\r\n");
        if (b) {
            b += 4;
            size_t bl = strlen(b);
            size_t cp = bl < resp_buf_size - 1 ? bl : resp_buf_size - 1;
            memcpy(resp_buf, b, cp);
            resp_buf[cp] = '\0';
        }
        im_free(raw);
        return OPRT_OK;
    }

    /* Direct TLS path */
    qq_conn_t *conn = im_calloc(1, sizeof(qq_conn_t));
    if (!conn) {
        return OPRT_MALLOC_FAILED;
    }

    OPERATE_RET rt = qq_direct_open(conn, host, 443, QQ_HTTP_TIMEOUT_MS);
    if (rt != OPRT_OK) {
        qq_conn_close(conn);
        im_free(conn);
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }

    char *req = (char *)im_malloc(1024);
    if (!req) {
        qq_conn_close(conn);
        im_free(conn);
        return OPRT_MALLOC_FAILED;
    }
    int req_len;
    if (auth_header) {
        req_len = snprintf(req, 1024,
                           "POST %s HTTP/1.1\r\nHost: %s\r\n%s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\nConnection: close\r\n\r\n",
                           path, host, auth_header, body_len);
    } else {
        req_len = snprintf(req, 1024,
                           "POST %s HTTP/1.1\r\nHost: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\nConnection: close\r\n\r\n",
                           path, host, body_len);
    }
    if (req_len <= 0 || req_len >= 1024) {
        im_free(req);
        qq_conn_close(conn);
        im_free(conn);
        return OPRT_BUFFER_NOT_ENOUGH;
    }
    if (qq_conn_write(conn, (const uint8_t *)req, req_len) != req_len) {
        im_free(req);
        qq_conn_close(conn);
        im_free(conn);
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }
    if (body_len > 0 && qq_conn_write(conn, (const uint8_t *)body, body_len) != body_len) {
        im_free(req);
        qq_conn_close(conn);
        im_free(conn);
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }
    im_free(req);

    size_t raw_cap = 4096;
    size_t raw_len = 0;
    char  *raw     = im_calloc(1, raw_cap);
    if (!raw) {
        qq_conn_close(conn);
        im_free(conn);
        return OPRT_MALLOC_FAILED;
    }

    uint32_t t0 = tal_system_get_millisecond();
    while (1) {
        if (raw_len + 1024 >= raw_cap) {
            char *tmp = im_realloc(raw, raw_cap * 2);
            if (!tmp) {
                im_free(raw);
                qq_conn_close(conn);
                im_free(conn);
                return OPRT_MALLOC_FAILED;
            }
            raw     = tmp;
            raw_cap *= 2;
        }
        int n = qq_conn_read(conn, (uint8_t *)raw + raw_len, (int)(raw_cap - raw_len - 1), QQ_PROXY_READ_SLICE_MS);
        if (n == OPRT_RESOURCE_NOT_READY) {
            if ((int)(tal_system_get_millisecond() - t0) >= QQ_PROXY_READ_TOTAL_MS) {
                im_free(raw);
                qq_conn_close(conn);
                im_free(conn);
                return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
            }
            continue;
        }
        if (n < 0) {
            if (raw_len > 0) {
                break;
            }
            im_free(raw);
            qq_conn_close(conn);
            im_free(conn);
            return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
        }
        if (n == 0) {
            break;
        }
        raw_len += (size_t)n;
        raw[raw_len] = '\0';
        t0 = tal_system_get_millisecond();
    }
    qq_conn_close(conn);
    im_free(conn);

    if (raw_len == 0) {
        im_free(raw);
        return OPRT_LINK_CORE_HTTP_CLIENT_SEND_ERROR;
    }
    if (status_out) {
        *status_out = im_parse_http_status(raw);
    }
    resp_buf[0] = '\0';
    char *b = strstr(raw, "\r\n\r\n");
    if (b) {
        b += 4;
        size_t bl = strlen(b);
        size_t cp = bl < resp_buf_size - 1 ? bl : resp_buf_size - 1;
        memcpy(resp_buf, b, cp);
        resp_buf[cp] = '\0';
    }
    im_free(raw);
    return OPRT_OK;
}

/* ================================================================
 * Deduplication
 * ================================================================ */

static bool seen_msg_contains(const char *msg_id)
{
    if (!msg_id || msg_id[0] == '\0') {
        return false;
    }
    uint64_t key = im_fnv1a64(msg_id);
    for (size_t i = 0; i < QQ_DEDUP_CACHE_SIZE; i++) {
        if (s_seen_msg_keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void seen_msg_insert(const char *msg_id)
{
    if (!msg_id || msg_id[0] == '\0') {
        return;
    }
    s_seen_msg_keys[s_seen_msg_idx] = im_fnv1a64(msg_id);
    s_seen_msg_idx                  = (s_seen_msg_idx + 1) % QQ_DEDUP_CACHE_SIZE;
}

/* ================================================================
 * Token Manager Thread
 * ================================================================ */

static void qqbot_token_task(void *arg)
{
    (void)arg;
    IM_LOGI(TAG, "qqbot token task started");

    uint32_t fail_delay_ms = 0;

    while (1) {
        if (s_app_id[0] == '\0' || s_client_secret[0] == '\0') {
            tal_system_sleep(3000);
            continue;
        }

        char body[256] = {0};
        int  n         = snprintf(body, sizeof(body),
                                  "{\"appId\":\"%s\",\"clientSecret\":\"%s\"}",
                                  s_app_id, s_client_secret);
        if (n <= 0 || n >= (int)sizeof(body)) {
            tal_system_sleep(10000);
            continue;
        }

        char    *resp   = im_calloc(1, QQ_HTTP_RESP_BUF);
        if (!resp) {
            tal_system_sleep(5000);
            continue;
        }

        uint16_t    status = 0;
        OPERATE_RET rt     = qq_http_post(QQ_TOKEN_HOST, "/app/getAppAccessToken",
                                          NULL, body, resp, QQ_HTTP_RESP_BUF, &status);

        uint32_t expires_in = 0;

        if (rt == OPRT_OK && status == 200) {
            cJSON      *root  = cJSON_Parse(resp);
            const char *token = root ? im_json_str(root, "access_token", NULL) : NULL;
            if (token && token[0] != '\0') {
                (void)tal_mutex_lock(s_token_mutex);
                im_safe_copy(s_access_token, sizeof(s_access_token), token);
                (void)tal_mutex_unlock(s_token_mutex);
                expires_in    = (uint32_t)im_json_uint(root, "expires_in", 7200);
                fail_delay_ms = 0;
                IM_LOGI(TAG, "qqbot token refreshed expires_in=%u", (unsigned)expires_in);
            } else {
                IM_LOGE(TAG, "qqbot token parse failed body=%.80s", resp);
            }
            if (root) {
                cJSON_Delete(root);
            }
        } else {
            IM_LOGE(TAG, "qqbot token fetch failed rt=%d http=%u", rt, (unsigned)status);
        }

        im_free(resp);

        if (expires_in > 0) {
            uint32_t sleep_ms = (expires_in * 2 / 3) * 1000u;
            tal_system_sleep(sleep_ms);
        } else {
            if (fail_delay_ms == 0) {
                fail_delay_ms = QQ_TOKEN_FAIL_BASE_MS;
            } else {
                fail_delay_ms *= 2;
                if (fail_delay_ms > QQ_TOKEN_FAIL_MAX_MS) {
                    fail_delay_ms = QQ_TOKEN_FAIL_MAX_MS;
                }
            }
            IM_LOGW(TAG, "qqbot token retry in %u ms", (unsigned)fail_delay_ms);
            tal_system_sleep(fail_delay_ms);
        }
    }
}

/* ================================================================
 * WebSocket Gateway Event Loop
 * ================================================================ */

static void publish_inbound(const char *chat_id, const char *content)
{
    if (!chat_id || !content || chat_id[0] == '\0' || content[0] == '\0') {
        return;
    }

    im_msg_t in = {0};
    im_safe_copy(in.channel, sizeof(in.channel), IM_CHAN_QQBOT);
    im_safe_copy(in.chat_id, sizeof(in.chat_id), chat_id);
    in.content = im_strdup(content);
    if (!in.content) {
        return;
    }

    OPERATE_RET rt = message_bus_push_inbound(&in);
    if (rt != OPRT_OK) {
        IM_LOGW(TAG, "push inbound failed rt=%d", rt);
        im_free(in.content);
    }
}

static void handle_c2c_message(cJSON *d)
{
    if (!cJSON_IsObject(d)) {
        return;
    }

    const char *msg_id  = im_json_str(d, "id", NULL);
    const char *content = im_json_str(d, "content", NULL);
    cJSON      *author  = cJSON_GetObjectItem(d, "author");
    const char *openid  = author ? im_json_str(author, "user_openid", NULL) : NULL;

    if (!openid || openid[0] == '\0') {
        return;
    }
    if (!content || content[0] == '\0') {
        return;
    }
    if (msg_id && seen_msg_contains(msg_id)) { /* called only from qqbot_ws_task */
        return;
    }
    if (msg_id) {
        seen_msg_insert(msg_id); /* called only from qqbot_ws_task */
    }

    char chat_id[128] = {0};
    snprintf(chat_id, sizeof(chat_id), "c2c:%s", openid);
    IM_LOGI(TAG, "rx c2c chat=%s len=%u", chat_id, (unsigned)strlen(content));
    publish_inbound(chat_id, content);
}

static void handle_group_message(cJSON *d)
{
    if (!cJSON_IsObject(d)) {
        return;
    }

    const char *msg_id    = im_json_str(d, "id", NULL);
    const char *content   = im_json_str(d, "content", NULL);
    const char *group_oid = im_json_str(d, "group_openid", NULL);

    if (!group_oid || group_oid[0] == '\0') {
        return;
    }
    if (!content || content[0] == '\0') {
        return;
    }
    if (msg_id && seen_msg_contains(msg_id)) { /* called only from qqbot_ws_task */
        return;
    }
    if (msg_id) {
        seen_msg_insert(msg_id); /* called only from qqbot_ws_task */
    }

    char chat_id[128] = {0};
    snprintf(chat_id, sizeof(chat_id), "group:%s", group_oid);
    IM_LOGI(TAG, "rx group chat=%s len=%u", chat_id, (unsigned)strlen(content));
    publish_inbound(chat_id, content);
}

static OPERATE_RET handle_gateway_payload(qq_conn_t *conn, const char *json_str, int64_t *seq,
                                          uint32_t *heartbeat_ms, uint32_t *next_heartbeat_ms)
{
    if (!conn || !json_str || !seq || !heartbeat_ms || !next_heartbeat_ms) {
        return OPRT_INVALID_PARM;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return OPRT_CR_CJSON_ERR;
    }

    cJSON *op_item  = cJSON_GetObjectItem(root, "op");
    cJSON *seq_item = cJSON_GetObjectItem(root, "s");
    int    op       = cJSON_IsNumber(op_item) ? (int)op_item->valuedouble : -1;

    if (cJSON_IsNumber(seq_item)) {
        *seq = (int64_t)seq_item->valuedouble;
    }

    OPERATE_RET rt = OPRT_OK;

    if (op == 10) {
        /* HELLO — server sends heartbeat interval */
        cJSON *d  = cJSON_GetObjectItem(root, "d");
        cJSON *hb = d ? cJSON_GetObjectItem(d, "heartbeat_interval") : NULL;
        if (cJSON_IsNumber(hb) && hb->valuedouble > 0) {
            *heartbeat_ms = (uint32_t)hb->valuedouble;
            if (*heartbeat_ms < 1000) {
                *heartbeat_ms = 1000;
            }
            *next_heartbeat_ms = tal_system_get_millisecond() + *heartbeat_ms;
            IM_LOGI(TAG, "qqbot HELLO heartbeat=%u ms", (unsigned)*heartbeat_ms);
        }

        /* Send IDENTIFY immediately */
        char *identify = im_malloc(512);
        if (!identify) {
            cJSON_Delete(root);
            return OPRT_MALLOC_FAILED;
        }
        (void)tal_mutex_lock(s_token_mutex);
        int n = snprintf(identify, 512,
                         "{\"op\":2,\"d\":{\"token\":\"QQBot %s\","
                         "\"intents\":%u,\"shard\":[0,1]}}",
                         s_access_token, (unsigned)IM_QQ_GATEWAY_INTENTS);
        (void)tal_mutex_unlock(s_token_mutex);
        if (n > 0 && n < 512) {
            rt = qq_ws_send_frame(conn, 0x1, (const uint8_t *)identify, (size_t)n);
            IM_LOGI(TAG, "qqbot IDENTIFY sent intents=%u", (unsigned)IM_QQ_GATEWAY_INTENTS);
        }
        im_free(identify);

    } else if (op == 11) {
        IM_LOGD(TAG, "qqbot HEARTBEAT_ACK");

    } else if (op == 1) {
        /* Server-requested heartbeat */
        char hb_payload[64] = {0};
        if (*seq >= 0) {
            snprintf(hb_payload, sizeof(hb_payload), "{\"op\":1,\"d\":%" PRId64 "}", *seq);
        } else {
            snprintf(hb_payload, sizeof(hb_payload), "{\"op\":1,\"d\":null}");
        }
        rt = qq_ws_send_frame(conn, 0x1, (const uint8_t *)hb_payload, strlen(hb_payload));
        if (rt == OPRT_OK && *heartbeat_ms > 0) {
            *next_heartbeat_ms = tal_system_get_millisecond() + *heartbeat_ms;
        }

    } else if (op == 7 || op == 9) {
        IM_LOGW(TAG, "qqbot gateway requested reconnect op=%d", op);
        rt = OPRT_COM_ERROR;

    } else if (op == 0) {
        const char *t = im_json_str(root, "t", NULL);
        cJSON      *d = cJSON_GetObjectItem(root, "d");

        if (t && strcmp(t, "READY") == 0) {
            if (d) {
                const char *sid   = im_json_str(d, "session_id", NULL);
                const char *botid = NULL;
                cJSON      *user  = cJSON_GetObjectItem(d, "user");
                if (user) {
                    botid = im_json_str(user, "id", NULL);
                }
                if (sid) {
                    im_safe_copy(s_session_id, sizeof(s_session_id), sid);
                }
                if (botid) {
                    im_safe_copy(s_bot_openid, sizeof(s_bot_openid), botid);
                }
                IM_LOGI(TAG, "qqbot READY session=%s bot_id=%s", s_session_id, s_bot_openid);
            }
        } else if (t && strcmp(t, "RESUMED") == 0) {
            IM_LOGI(TAG, "qqbot RESUMED");
        } else if (t && strcmp(t, "C2C_MESSAGE_CREATE") == 0) {
            handle_c2c_message(d);
        } else if (t && strcmp(t, "GROUP_AT_MESSAGE_CREATE") == 0) {
            handle_group_message(d);
        }
    }

    cJSON_Delete(root);
    return rt;
}

static void qqbot_ws_task(void *arg)
{
    (void)arg;
    IM_LOGI(TAG, "qqbot ws task started");

    uint32_t reconnect_ms = 0;

    while (1) {
        /* Wait for a valid token before connecting */
        {
            bool has_token = false;
            while (!has_token) {
                (void)tal_mutex_lock(s_token_mutex);
                has_token = (s_access_token[0] != '\0');
                (void)tal_mutex_unlock(s_token_mutex);
                if (!has_token) {
                    tal_system_sleep(1000);
                }
            }
        }

        if (reconnect_ms > 0) {
            IM_LOGW(TAG, "qqbot ws reconnect in %u ms", (unsigned)reconnect_ms);
            tal_system_sleep(reconnect_ms);
        }

        qq_conn_t *conn = im_calloc(1, sizeof(qq_conn_t));
        if (!conn) {
            reconnect_ms = QQ_RECONNECT_BASE_MS;
            continue;
        }

        OPERATE_RET rt = qq_conn_open(conn, QQ_WS_HOST, 443, QQ_HTTP_TIMEOUT_MS);
        if (rt != OPRT_OK) {
            IM_LOGW(TAG, "qqbot ws connect failed rt=%d", rt);
            im_free(conn);
            reconnect_ms = (reconnect_ms == 0) ? QQ_RECONNECT_BASE_MS : reconnect_ms * 2;
            if (reconnect_ms > QQ_RECONNECT_MAX_MS) {
                reconnect_ms = QQ_RECONNECT_MAX_MS;
            }
            continue;
        }

        rt = qq_ws_handshake(conn, QQ_WS_HOST, QQ_WS_PATH);
        if (rt != OPRT_OK) {
            IM_LOGW(TAG, "qqbot ws handshake failed rt=%d", rt);
            qq_conn_close(conn);
            im_free(conn);
            reconnect_ms = (reconnect_ms == 0) ? QQ_RECONNECT_BASE_MS : reconnect_ms * 2;
            if (reconnect_ms > QQ_RECONNECT_MAX_MS) {
                reconnect_ms = QQ_RECONNECT_MAX_MS;
            }
            continue;
        }

        int64_t  seq               = -1;
        uint32_t heartbeat_ms      = 0;
        uint32_t next_heartbeat_ms = 0;

        while (1) {
            if (heartbeat_ms > 0) {
                uint32_t now = tal_system_get_millisecond();
                if ((int32_t)(now - next_heartbeat_ms) >= 0) {
                    char hb[64] = {0};
                    if (seq >= 0) {
                        snprintf(hb, sizeof(hb), "{\"op\":1,\"d\":%" PRId64 "}", seq);
                    } else {
                        snprintf(hb, sizeof(hb), "{\"op\":1,\"d\":null}");
                    }
                    OPERATE_RET hb_rt = qq_ws_send_frame(conn, 0x1, (const uint8_t *)hb, strlen(hb));
                    if (hb_rt != OPRT_OK) {
                        IM_LOGW(TAG, "qqbot heartbeat failed rt=%d", hb_rt);
                        break;
                    }
                    next_heartbeat_ms = now + heartbeat_ms;
                }
            }

            uint8_t  opcode      = 0;
            uint8_t *payload     = NULL;
            size_t   payload_len = 0;
            rt = qq_ws_poll_frame(conn, 500, &opcode, &payload, &payload_len);
            if (rt == OPRT_RESOURCE_NOT_READY) {
                continue;
            }
            if (rt != OPRT_OK) {
                im_free(payload);
                IM_LOGW(TAG, "qqbot ws poll failed rt=%d", rt);
                break;
            }

            if (opcode == 0x1 && payload && payload_len > 0) {
                OPERATE_RET hrt = handle_gateway_payload(conn, (const char *)payload,
                                                         &seq, &heartbeat_ms, &next_heartbeat_ms);
                if (hrt != OPRT_OK) {
                    im_free(payload);
                    break;
                }
                s_last_seq = seq;
            } else if (opcode == 0x8) {
                if (payload && payload_len >= 2) {
                    int cc = ((int)payload[0] << 8) | (int)payload[1];
                    IM_LOGW(TAG, "qqbot ws close code=%d", cc);
                }
                im_free(payload);
                break;
            } else if (opcode == 0x9) {
                (void)qq_ws_send_frame(conn, 0xA, payload, payload_len);
            }

            im_free(payload);
        }

        qq_conn_close(conn);
        im_free(conn);

        reconnect_ms = (reconnect_ms == 0) ? QQ_RECONNECT_BASE_MS : reconnect_ms * 2;
        if (reconnect_ms > QQ_RECONNECT_MAX_MS) {
            reconnect_ms = QQ_RECONNECT_MAX_MS;
        }
    }
}

/* ================================================================
 * Message Sender
 * ================================================================ */

OPERATE_RET qqbot_send_message(const char *chat_id, const char *text)
{
    if (!chat_id || !text) {
        return OPRT_INVALID_PARM;
    }

    (void)tal_mutex_lock(s_token_mutex);
    if (s_access_token[0] == '\0') {
        (void)tal_mutex_unlock(s_token_mutex);
        IM_LOGW(TAG, "qqbot send: no token yet");
        return OPRT_NOT_FOUND;
    }
    char auth_hdr[320] = {0};
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: QQBot %s", s_access_token);
    (void)tal_mutex_unlock(s_token_mutex);

    bool        is_c2c    = (strncmp(chat_id, "c2c:", 4) == 0);
    bool        is_group  = (strncmp(chat_id, "group:", 6) == 0);
    char        path[256] = {0};

    if (is_c2c) {
        snprintf(path, sizeof(path), "/v2/users/%s/messages", chat_id + 4);
    } else if (is_group) {
        snprintf(path, sizeof(path), "/v2/groups/%s/messages", chat_id + 6);
    } else {
        IM_LOGE(TAG, "qqbot send: unrecognised chat_id format: %s", chat_id);
        return OPRT_INVALID_PARM;
    }

    size_t text_len = strlen(text);
    size_t offset   = 0;
    bool   all_ok   = true;

    while (offset < text_len || (text_len == 0 && offset == 0)) {
        size_t chunk = text_len - offset;
        if (chunk > IM_QQ_MAX_MSG_LEN) {
            chunk = IM_QQ_MAX_MSG_LEN;
        }
        if (text_len == 0) {
            chunk = 0;
        }

        char *seg = im_calloc(1, chunk + 1);
        if (!seg) {
            return OPRT_MALLOC_FAILED;
        }
        if (chunk > 0) {
            memcpy(seg, text + offset, chunk);
        }
        seg[chunk] = '\0';

        cJSON *body_obj = cJSON_CreateObject();
        if (!body_obj) {
            im_free(seg);
            return OPRT_MALLOC_FAILED;
        }
        cJSON_AddStringToObject(body_obj, "content", seg);
        cJSON_AddNumberToObject(body_obj, "msg_type", 0);
        cJSON_AddNumberToObject(body_obj, "msg_seq", (double)(tal_system_get_random(0xFFFF)));
        char *json_body = cJSON_PrintUnformatted(body_obj);
        cJSON_Delete(body_obj);
        im_free(seg);
        if (!json_body) {
            return OPRT_MALLOC_FAILED;
        }

        char    *resp   = im_calloc(1, QQ_HTTP_RESP_BUF);
        uint16_t status = 0;

        if (resp) {
            OPERATE_RET rt = qq_http_post(QQ_API_HOST, path, auth_hdr, json_body,
                                          resp, QQ_HTTP_RESP_BUF, &status);
            if (rt == OPRT_OK && (status == 200 || status == 201 || status == 202)) {
                IM_LOGI(TAG, "qqbot send ok chat=%s bytes=%u http=%u",
                        chat_id, (unsigned)chunk, (unsigned)status);
            } else {
                IM_LOGE(TAG, "qqbot send failed chat=%s rt=%d http=%u body=%.80s",
                        chat_id, rt, (unsigned)status, resp ? resp : "");
                all_ok = false;
            }
            im_free(resp);
        } else {
            all_ok = false;
        }

        cJSON_free(json_body);
        if (text_len == 0) {
            break;
        }
        offset += chunk;
    }

    return all_ok ? OPRT_OK : OPRT_COM_ERROR;
}

/* ================================================================
 * Public API
 * ================================================================ */

OPERATE_RET qqbot_channel_init(void)
{
    if (IM_SECRET_QQ_APP_ID[0] != '\0') {
        im_safe_copy(s_app_id, sizeof(s_app_id), IM_SECRET_QQ_APP_ID);
    }
    if (IM_SECRET_QQ_CLIENT_SECRET[0] != '\0') {
        im_safe_copy(s_client_secret, sizeof(s_client_secret), IM_SECRET_QQ_CLIENT_SECRET);
    }

    char tmp[64] = {0};
    if (im_kv_get_string(IM_NVS_QQ, IM_NVS_KEY_QQ_APP_ID, tmp, sizeof(tmp)) == OPRT_OK
            && tmp[0] != '\0') {
        im_safe_copy(s_app_id, sizeof(s_app_id), tmp);
    }
    memset(tmp, 0, sizeof(tmp));
    if (im_kv_get_string(IM_NVS_QQ, IM_NVS_KEY_QQ_SECRET, tmp, sizeof(tmp)) == OPRT_OK
            && tmp[0] != '\0') {
        im_safe_copy(s_client_secret, sizeof(s_client_secret), tmp);
    }

    if (!s_token_mutex) {
        if (tal_mutex_create_init(&s_token_mutex) != OPRT_OK) {
            IM_LOGE(TAG, "qqbot: failed to create token mutex");
            return OPRT_COM_ERROR;
        }
    }

    IM_LOGI(TAG, "qqbot init app_id=%s credentials=%s",
            s_app_id[0] ? s_app_id : "empty",
            (s_app_id[0] && s_client_secret[0]) ? "configured" : "missing");

    return OPRT_OK;
}

OPERATE_RET qqbot_channel_start(void)
{
    if (s_app_id[0] == '\0' || s_client_secret[0] == '\0') {
        IM_LOGW(TAG, "qqbot start: credentials not configured");
        return OPRT_NOT_FOUND;
    }

    if (!s_token_thread) {
        THREAD_CFG_T cfg = {0};
        cfg.stackDepth   = IM_QQ_TOKEN_STACK;
        cfg.priority     = IM_QQ_POLL_PRIO;
        cfg.thrdname     = "im_qq_token";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
        cfg.psram_mode = 1;
#endif
        OPERATE_RET rt   = tal_thread_create_and_start(&s_token_thread, NULL, NULL,
                                                       qqbot_token_task, NULL, &cfg);
        if (rt != OPRT_OK) {
            IM_LOGE(TAG, "qqbot: create token thread failed rt=%d", rt);
            return rt;
        }
    }

    if (!s_ws_thread) {
        THREAD_CFG_T cfg = {0};
        cfg.stackDepth   = IM_QQ_POLL_STACK;
        cfg.priority     = IM_QQ_POLL_PRIO;
        cfg.thrdname     = "im_qq_ws";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
        cfg.psram_mode = 1;
#endif
        OPERATE_RET rt   = tal_thread_create_and_start(&s_ws_thread, NULL, NULL,
                                                       qqbot_ws_task, NULL, &cfg);
        if (rt != OPRT_OK) {
            IM_LOGE(TAG, "qqbot: create ws thread failed rt=%d", rt);
            return rt;
        }
    }

    IM_LOGI(TAG, "qqbot channel started");
    return OPRT_OK;
}

OPERATE_RET qqbot_set_app_id(const char *app_id)
{
    if (!app_id) {
        return OPRT_INVALID_PARM;
    }
    im_safe_copy(s_app_id, sizeof(s_app_id), app_id);
    return im_kv_set_string(IM_NVS_QQ, IM_NVS_KEY_QQ_APP_ID, app_id);
}

OPERATE_RET qqbot_set_client_secret(const char *secret)
{
    if (!secret) {
        return OPRT_INVALID_PARM;
    }
    im_safe_copy(s_client_secret, sizeof(s_client_secret), secret);
    return im_kv_set_string(IM_NVS_QQ, IM_NVS_KEY_QQ_SECRET, secret);
}

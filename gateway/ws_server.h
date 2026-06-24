/**
 * @file ws_server.h
 * @brief WebSocket server for DuckyClaw gateway
 *
 * Provides a lightweight WebSocket server that accepts external client
 * connections. Text messages received from clients are forwarded to the
 * message_bus so agent_loop can process them uniformly. Responses can be
 * pushed back to a specific client through ws_server_send().
 *
 * Ported from TuyaOpen/apps/mimiclaw/gateway.
 */

#pragma once

#include "tuya_cloud_types.h"

/* ---------- configurable defaults (override in tuya_config.h) ---------- */

#ifndef CLAW_WS_PORT
#define CLAW_WS_PORT          18789
#endif

#ifndef CLAW_WS_MAX_CLIENTS
#define CLAW_WS_MAX_CLIENTS   4
#endif

#ifndef CLAW_WS_CLIENT_RX_BUF_SIZE
#define CLAW_WS_CLIENT_RX_BUF_SIZE 4096
#endif

#ifndef CLAW_WS_TOKEN_MAX_LEN
#define CLAW_WS_TOKEN_MAX_LEN 128
#endif

#define CLAW_WS_TOKEN_KV_KEY  "ws_auth_token"

/* ---------- public API ---------- */

/**
 * @brief Start the WebSocket server
 *
 * Creates a TCP listener on CLAW_WS_PORT and spawns a background thread
 * to accept / serve clients.
 *
 * @return OPRT_OK on success
 */
OPERATE_RET ws_server_start(void);

/**
 * @brief Send a text response to a specific WebSocket client
 *
 * @param chat_id  Client identifier (assigned during connection)
 * @param text     UTF-8 text to send
 * @return OPRT_OK on success, OPRT_NOT_FOUND if client not connected
 */
OPERATE_RET ws_server_send(const char *chat_id, const char *text);

/**
 * @brief Stop the WebSocket server and disconnect all clients
 *
 * @return OPRT_OK on success
 */
OPERATE_RET ws_server_stop(void);

/**
 * @brief Set or update the WebSocket authentication token at runtime
 *
 * The new token is persisted to KV storage and takes effect immediately.
 * Pass an empty string to disable authentication.
 *
 * @param token  The new authentication token (NULL-terminated)
 * @return OPRT_OK on success
 */
OPERATE_RET ws_server_set_token(const char *token);

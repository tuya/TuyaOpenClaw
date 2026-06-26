/**
 * @file acp_client.h
 * @brief ACP (Agent Client Protocol) WebSocket client for TuyaOpenClaw.
 *
 * @version 2.0
 * @date 2026-03-19
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#ifndef __ACP_CLIENT_H__
#define __ACP_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

#ifndef ACP_CLIENT_RX_BUF_SIZE
#define ACP_CLIENT_RX_BUF_SIZE    (8 * 1024)
#endif

#ifndef ACP_CLIENT_REPLY_BUF_SIZE
#define ACP_CLIENT_REPLY_BUF_SIZE (8 * 1024)
#endif

#ifndef ACP_CLIENT_RECONNECT_MS
#define ACP_CLIENT_RECONNECT_MS   5000
#endif

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */

/**
 * @brief Callback invoked when the OpenClaw agent finishes a reply.
 *
 * @param[in] text       Complete assistant reply text (NUL-terminated).
 * @param[in] user_data  Opaque pointer supplied at registration time.
 */
typedef void (*acp_reply_cb_t)(const char *text, void *user_data);

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Register a fallback callback for incoming OpenClaw agent replies.
 *
 * @param[in] cb         Callback function; NULL clears the existing callback.
 * @param[in] user_data  Forwarded opaque pointer.
 * @return none
 * @note Must be called before acp_client_init().
 */
void acp_client_set_reply_cb(acp_reply_cb_t cb, void *user_data);

/**
 * @brief Initialise the ACP client and start the background task.
 *
 * Reads connection parameters from OPENCLAW_GATEWAY_HOST, OPENCLAW_GATEWAY_PORT,
 * OPENCLAW_GATEWAY_TOKEN, and TUYAOPENCLAW_DEVICE_ID (all defined in tuya_app_config.h).
 * The background task handles connection, reconnection, and frame I/O.
 *
 * @return OPRT_OK on success, error code on failure.
 * @note Call after the network link is up (e.g., inside EVENT_MQTT_CONNECTED).
 */
OPERATE_RET acp_client_init(void);

/**
 * @brief Inject a user message into the OpenClaw agent session.
 *
 * Sends a chat.send ACP request to the connected OpenClaw gateway.
 * On success, the current app_im route is snapshotted and the background
 * thread is notified to wait for a single final ACP reply. That reply is
 * then injected back into message_bus as a new agent turn. If no final
 * reply arrives within 120 seconds, the client injects a timeout notice
 * into message_bus for the same route.
 *
 * @param[in] text  User message text (NUL-terminated, UTF-8).
 * @return OPRT_OK on success.
 * @return OPRT_RESOURCE_NOT_READY if the ACP session is not yet established
 *                                 or the previous ACP request is still pending.
 * @return OPRT_INVALID_PARM if text is NULL or empty.
 */
OPERATE_RET acp_client_inject(const char *text);

/**
 * @brief Stop the ACP client and close the connection.
 *
 * @return OPRT_OK on success.
 */
OPERATE_RET acp_client_stop(void);

/**
 * @brief Query whether the ACP session is currently connected.
 *
 * @return TRUE  if the ACP session is established.
 * @return FALSE if disconnected or still connecting.
 */
bool acp_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* __ACP_CLIENT_H__ */

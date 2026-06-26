/**
 * @file tuya_app_config.h
 * @brief tuya_app_config module is used to 
 * @version 0.1
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __TUYA_APP_CONFIG_H__
#define __TUYA_APP_CONFIG_H__

#include "tuya_cloud_types.h"

#if __has_include("tuya_app_config_secrets.h")
#include "tuya_app_config_secrets.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// https://pbt.tuya.com/s?p=dd46368ae3840e54f018b2c45dc1550b&u=c38c8fc0a5d14c4f66cae9f0cfcb2a24&t=2
#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID "xxxxxxxxxxxxxxxx"
#endif

// https://platform.tuya.com/purchase/index?type=6
#ifndef TUYA_OPENSDK_UUID
#define TUYA_OPENSDK_UUID    "uuidxxxxxxxxxxxxxxxx"             // Please change the correct uuid
#endif
#ifndef TUYA_OPENSDK_AUTHKEY
#define TUYA_OPENSDK_AUTHKEY "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" // Please change the correct authkey
#endif

// IM configuration
// feishu | telegram | discord | weixin | qqbot
// NOTE: ESP32-S3 does not support weixin
#ifndef IM_SECRET_CHANNEL_MODE
#define IM_SECRET_CHANNEL_MODE      "feishu"
#endif

#ifndef IM_SECRET_FS_APP_ID
#define IM_SECRET_FS_APP_ID         ""
#endif
#ifndef IM_SECRET_FS_APP_SECRET
#define IM_SECRET_FS_APP_SECRET     ""
#endif

#ifndef IM_SECRET_TG_TOKEN
#define IM_SECRET_TG_TOKEN          ""
#endif

#ifndef IM_SECRET_DC_TOKEN
#define IM_SECRET_DC_TOKEN          ""
#endif
#ifndef IM_SECRET_DC_CHANNEL_ID
#define IM_SECRET_DC_CHANNEL_ID     ""
#endif

#ifndef IM_SECRET_QQ_APP_ID
#define IM_SECRET_QQ_APP_ID         ""
#endif
#ifndef IM_SECRET_QQ_CLIENT_SECRET
#define IM_SECRET_QQ_CLIENT_SECRET  ""
#endif

// WebSocket authentication token (empty string = auth disabled)
#ifndef CLAW_WS_AUTH_TOKEN
#define CLAW_WS_AUTH_TOKEN          ""
#endif

/* ---------------------------------------------------------------------------
 * ACP Gateway configuration (openclaw direct connection)
 * Override in tuya_app_config_secrets.h for environment-specific values.
 * --------------------------------------------------------------------------- */

#ifndef OPENCLAW_GATEWAY_HOST
#define OPENCLAW_GATEWAY_HOST    "192.168.1.1"   /* Linux machine LAN IP */
#endif

#ifndef OPENCLAW_GATEWAY_PORT
#define OPENCLAW_GATEWAY_PORT    18789
#endif

#ifndef OPENCLAW_GATEWAY_TOKEN
#define OPENCLAW_GATEWAY_TOKEN   ""              /* Set in tuya_app_config_secrets.h */
#endif

#ifndef TUYAOPENCLAW_DEVICE_ID
#define TUYAOPENCLAW_DEVICE_ID       "tuyaopenclaw-001" /* Unique per device, e.g. MAC address */
#endif

#ifdef __cplusplus
}
#endif

#endif /* __TUYA_APP_CONFIG_H__ */

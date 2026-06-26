/**
 * @file heartbeat.h
 * @brief Heartbeat service for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __HEARTBEAT_H__
#define __HEARTBEAT_H__

#include "tuya_cloud_types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Heartbeat check interval in milliseconds (default 30 minutes) */
#ifndef CLAW_HEARTBEAT_INTERVAL_MS
#define CLAW_HEARTBEAT_INTERVAL_MS (30 * 60 * 1000)
#endif

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Initialize heartbeat service
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET heartbeat_init(void);

/**
 * @brief Start heartbeat timer
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET heartbeat_start(void);

/**
 * @brief Stop heartbeat timer
 */
void heartbeat_stop(void);

/**
 * @brief Manually trigger a heartbeat check
 * @return true if tasks were found and message sent
 */
bool heartbeat_trigger(void);

#ifdef __cplusplus
}
#endif

#endif /* __HEARTBEAT_H__ */

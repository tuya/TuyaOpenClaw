/**
 * @file tuyaopen_claw_chat.h
 * @brief tuyaopen_claw_chat module: AI stream event bridge for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 */

#ifndef __TUYAOPEN_CLAW_CHAT_H__
#define __TUYAOPEN_CLAW_CHAT_H__

#include "tuya_cloud_types.h"
#include "ai_chat_main.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
********************function declaration********************
***********************************************************/
OPERATE_RET tuyaopen_claw_chat_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __TUYAOPEN_CLAW_CHAT_H__ */

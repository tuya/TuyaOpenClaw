/**
 * @file ai_ui_chat_xiaozhi.h
 * @brief Xiaozhi-style chat UI with standby face animation interface.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __AI_UI_CHAT_XIAOZHI_H__
#define __AI_UI_CHAT_XIAOZHI_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Emotion types for standby face */
typedef enum {
    XIAOZHI_EMO_HAPPY,
    XIAOZHI_EMO_RELAXED,
    XIAOZHI_EMO_CURIOUS,
    XIAOZHI_EMO_SLEEPY,
    XIAOZHI_EMO_EXCITED,
    XIAOZHI_EMO_MAX,
} XIAOZHI_EMO_TYPE_E;

/***********************************************************
********************function declaration********************/

/**
 * @brief Register Xiaozhi-style chat UI.
 *
 * This UI provides:
 *   - Animated standby face with blinking eyes
 *   - Automatic switching between standby and chat modes
 *   - Smooth LVGL-based animations
 *
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_ui_chat_xiaozhi_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __AI_UI_CHAT_XIAOZHI_H__ */

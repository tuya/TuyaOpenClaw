/**
 * @file tool_hw.h
 * @brief MCP hardware peripheral tools for TuyaOpenClaw
 * @version 0.1
 * @date 2026-03-31
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Exposes GPIO, I2C, ADC, UART, PWM and servo MCP tools.
 * Compiled only when ENABLE_HARDWARE_MCP == 1.
 */

#ifndef __TOOL_HW_H__
#define __TOOL_HW_H__

#include "tuya_cloud_types.h"
#include "ai_mcp_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Register all hardware MCP tools
 *
 * Registers gpio_write, gpio_read, i2c_scan, adc_read,
 * uart_write, uart_read, pwm_set and servo_set tools with
 * the MCP server.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_hw_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOOL_HW_H__ */

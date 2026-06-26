/**
 * @file tool_hw.c
 * @brief MCP hardware peripheral tools for TuyaOpenClaw (T5 platform)
 * @version 0.2
 * @date 2026-03-31
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * All tools accept board pin numbers (0-55, corresponding to T5 P0-P55).
 * Each tool looks up the T5 peripheral mapping table to resolve the correct
 * software port / channel.  If the given pin does not support the requested
 * function an error string is returned instead of performing the operation.
 *
 * Reference mapping:
 *   https://github.com/Tuya-Community/TuyaOpen.io/.../t5ai-peripheral-mapping.md
 *
 * Implements:
 *   gpio_write  - Drive a GPIO pin high or low
 *   gpio_read   - Read the level of a GPIO input pin
 *   i2c_scan    - Scan an I2C bus (SCL+SDA pair)
 *   adc_read    - Read ADC sampled value on a pin
 *   uart_write  - Send a string over UART 2 (P30/P31)
 *   uart_read   - Read buffered data from UART 2
 *   pwm_set     - Output PWM on a pin with given duty cycle
 *   servo_set   - Drive a servo to a given angle via PWM
 */

#include "tool_hw.h"
#include "tool_files.h"     /* claw_malloc / claw_free */
#include "ai_mcp_server.h"

#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_i2c.h"
#include "tkl_pinmux.h"
#include "tkl_adc.h"
#include "tkl_pwm.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

/* Total number of T5 GPIO pins (P0 – P55) */
#define T5_GPIO_PIN_MAX     56

/* UART port used by uart_write / uart_read tools */
#define HW_UART_PORT        TUYA_UART_NUM_2

/* UART 2 default board pins on T5: P30=RX, P31=TX */
#define HW_UART2_DEFAULT_RX_PIN  30
#define HW_UART2_DEFAULT_TX_PIN  31

/* UART RX ring-buffer size (bytes) */
#define HW_UART_RX_BUF      256

/* Servo PWM frequency (Hz) and pulse-width duty bounds */
#define HW_SERVO_FREQ_HZ    50
/*
 * Period = 20 ms.  On a 1-10000 scale:
 *   0.5 ms  →  10000 * 0.5 / 20 = 250
 *   2.5 ms  →  10000 * 2.5 / 20 = 1250
 */
#define HW_SERVO_DUTY_MIN   250
#define HW_SERVO_DUTY_MAX   1250

/* Result buffer length for strings returned to the agent */
#define HW_RESULT_BUF       512

/* ---------------------------------------------------------------------------
 * T5 ADC pin mapping  (board pin → ADC port + channel)
 * --------------------------------------------------------------------------- */
typedef struct {
    uint8_t board_pin;
    uint8_t adc_port;   /* TUYA_ADC_NUM_0 = 0 */
    uint8_t channel;
} T5_ADC_MAP_T;

static const T5_ADC_MAP_T s_adc_map[] = {
    {25, 0,  1},
    {24, 0,  2},
    {23, 0,  3},
    {28, 0,  4},
    {22, 0,  5},
    {21, 0,  6},
    { 8, 0, 10},
    { 0, 0, 12},
    { 1, 0, 13},
    {12, 0, 14},
    {13, 0, 15},
};
#define T5_ADC_MAP_LEN  (sizeof(s_adc_map) / sizeof(s_adc_map[0]))

/* ---------------------------------------------------------------------------
 * T5 PWM pin mapping  (board pin → PWM port)
 * --------------------------------------------------------------------------- */
typedef struct {
    uint8_t board_pin;
    uint8_t pwm_port;   /* TUYA_PWM_NUM_X */
} T5_PWM_MAP_T;

static const T5_PWM_MAP_T s_pwm_map[] = {
    {18,  0},   /* PWMG0_PWM0 → TUYA_PWM_NUM_0  */
    {24,  1},   /* PWMG0_PWM4 → TUYA_PWM_NUM_1  */
    {32,  2},   /* PWMG1_PWM0 → TUYA_PWM_NUM_2  */
    {34,  3},   /* PWMG1_PWM2 → TUYA_PWM_NUM_3  */
    {36,  4},   /* PWMG1_PWM4 → TUYA_PWM_NUM_4  */
    {19,  5},   /* PWMG0_PWM1 → TUYA_PWM_NUM_5  */
    { 8,  6},   /* PWMG0_PWM2 → TUYA_PWM_NUM_6  */
    { 9,  7},   /* PWMG0_PWM3 → TUYA_PWM_NUM_7  */
    {25,  8},   /* PWMG0_PWM5 → TUYA_PWM_NUM_8  */
    {33,  9},   /* PWMG1_PWM1 → TUYA_PWM_NUM_9  */
    {35, 10},   /* PWMG1_PWM3 → TUYA_PWM_NUM_10 */
    {37, 11},   /* PWMG1_PWM5 → TUYA_PWM_NUM_11 */
};
#define T5_PWM_MAP_LEN  (sizeof(s_pwm_map) / sizeof(s_pwm_map[0]))

/* ---------------------------------------------------------------------------
 * T5 I2C pair mapping  (board SCL+SDA pins → port + pinmux functions)
 * --------------------------------------------------------------------------- */
typedef struct {
    uint8_t scl_pin;
    uint8_t sda_pin;
    uint8_t i2c_port;       /* TUYA_I2C_NUM_X */
    uint32_t scl_func;      /* TUYA_IIC*_SCL  */
    uint32_t sda_func;      /* TUYA_IIC*_SDA  */
} T5_I2C_MAP_T;

static const T5_I2C_MAP_T s_i2c_map[] = {
    {20, 21, 0, TUYA_IIC0_SCL, TUYA_IIC0_SDA},
    { 6,  7, 1, TUYA_IIC1_SCL, TUYA_IIC1_SDA},
    {14, 15, 1, TUYA_IIC1_SCL, TUYA_IIC1_SDA},
    {38, 39, 1, TUYA_IIC1_SCL, TUYA_IIC1_SDA},
    {42, 43, 1, TUYA_IIC1_SCL, TUYA_IIC1_SDA},
};
#define T5_I2C_MAP_LEN  (sizeof(s_i2c_map) / sizeof(s_i2c_map[0]))

/* ---------------------------------------------------------------------------
 * File-scope variables
 * --------------------------------------------------------------------------- */

static bool s_uart_inited = false;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Get integer property from MCP property list
 * @param[in]  props  Property list
 * @param[in]  name   Property name
 * @param[out] out    Receives the integer value
 * @return true if found, false otherwise
 */
static bool __get_int_prop(const MCP_PROPERTY_LIST_T *props,
                           const char *name, int *out)
{
    const MCP_PROPERTY_T *p = ai_mcp_property_list_find(props, name);
    if (!p || p->type != MCP_PROPERTY_TYPE_INTEGER) {
        return false;
    }
    *out = p->default_val.int_val;
    return true;
}

/**
 * @brief Get string property from MCP property list
 * @param[in] props  Property list
 * @param[in] name   Property name
 * @return Pointer to string value, or NULL if not found
 */
static const char *__get_str_prop(const MCP_PROPERTY_LIST_T *props,
                                  const char *name)
{
    const MCP_PROPERTY_T *p = ai_mcp_property_list_find(props, name);
    if (!p || p->type != MCP_PROPERTY_TYPE_STRING) {
        return NULL;
    }
    return p->default_val.str_val;
}

/**
 * @brief Look up the T5 ADC mapping for a given board pin
 * @param[in]  board_pin  Board pin number (0-55)
 * @param[out] entry      Pointer-to-entry on success
 * @return true if pin supports ADC, false otherwise
 */
static bool __adc_lookup(int board_pin, const T5_ADC_MAP_T **entry)
{
    for (size_t i = 0; i < T5_ADC_MAP_LEN; i++) {
        if (s_adc_map[i].board_pin == (uint8_t)board_pin) {
            *entry = &s_adc_map[i];
            return true;
        }
    }
    return false;
}

/**
 * @brief Look up the T5 PWM mapping for a given board pin
 * @param[in]  board_pin  Board pin number (0-55)
 * @param[out] entry      Pointer-to-entry on success
 * @return true if pin supports PWM, false otherwise
 */
static bool __pwm_lookup(int board_pin, const T5_PWM_MAP_T **entry)
{
    for (size_t i = 0; i < T5_PWM_MAP_LEN; i++) {
        if (s_pwm_map[i].board_pin == (uint8_t)board_pin) {
            *entry = &s_pwm_map[i];
            return true;
        }
    }
    return false;
}

/**
 * @brief Look up the T5 I2C mapping for a given SCL+SDA board pin pair
 * @param[in]  scl_pin  SCL board pin number
 * @param[in]  sda_pin  SDA board pin number
 * @param[out] entry    Pointer-to-entry on success
 * @return true if the pair is a valid I2C mapping, false otherwise
 */
static bool __i2c_lookup(int scl_pin, int sda_pin, const T5_I2C_MAP_T **entry)
{
    for (size_t i = 0; i < T5_I2C_MAP_LEN; i++) {
        if (s_i2c_map[i].scl_pin == (uint8_t)scl_pin &&
            s_i2c_map[i].sda_pin == (uint8_t)sda_pin) {
            *entry = &s_i2c_map[i];
            return true;
        }
    }
    return false;
}

/**
 * @brief Lazy-initialise UART 2 for write/read tools
 *
 * Default T5 UART2 pins: P30 (RX), P31 (TX).
 *
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __uart_ensure_init(void)
{
    if (s_uart_inited) {
        return OPRT_OK;
    }

    tkl_io_pinmux_config((TUYA_PIN_NAME_E)HW_UART2_DEFAULT_RX_PIN, TUYA_UART2_RX);
    tkl_io_pinmux_config((TUYA_PIN_NAME_E)HW_UART2_DEFAULT_TX_PIN, TUYA_UART2_TX);

    TAL_UART_CFG_T cfg = {0};
    cfg.base_cfg.baudrate = 115200;
    cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    cfg.base_cfg.parity   = TUYA_UART_PARITY_TYPE_NONE;
    cfg.rx_buffer_size    = HW_UART_RX_BUF;
    cfg.open_mode         = 0;

    OPERATE_RET rt = tal_uart_init(HW_UART_PORT, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("UART2 init failed, err<%d>", rt);
        return rt;
    }

    s_uart_inited = true;
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Tool callbacks
 * --------------------------------------------------------------------------- */

/**
 * @brief gpio_write tool callback
 *
 * All T5 pins P0-P55 support GPIO output.  The board pin number maps
 * directly to TUYA_GPIO_NUM_N.
 *
 * @param[in]  properties  pin (int 0-55), level (int 0|1)
 * @param[out] ret_val     Result string
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_gpio_write(const MCP_PROPERTY_LIST_T *properties,
                                     MCP_RETURN_VALUE_T *ret_val,
                                     void *user_data)
{
    int pin   = 0;
    int level = 0;

    if (!__get_int_prop(properties, "pin", &pin) ||
        !__get_int_prop(properties, "level", &level)) {
        ai_mcp_return_value_set_str(ret_val, "error: missing parameter pin or level");
        return OPRT_INVALID_PARM;
    }

    if (pin < 0 || pin >= T5_GPIO_PIN_MAX) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pin P%d is out of range, T5 supports P0-P%d",
                 pin, T5_GPIO_PIN_MAX - 1);
        ai_mcp_return_value_set_str(ret_val, buf);
        return OPRT_INVALID_PARM;
    }

    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = (level != 0) ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW,
    };

    PR_NOTICE("gpio_write pin %d level %d", pin, level);
    OPERATE_RET rt = tkl_gpio_init((TUYA_GPIO_NUM_E)pin, &cfg);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: gpio_init P%d failed, code %d", pin, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    rt = tkl_gpio_write((TUYA_GPIO_NUM_E)pin,
                        (level != 0) ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: gpio_write P%d failed, code %d", pin, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    char buf[HW_RESULT_BUF];
    snprintf(buf, sizeof(buf),
             "ok: P%d set to %s", pin, (level != 0) ? "HIGH" : "LOW");
    ai_mcp_return_value_set_str(ret_val, buf);
    return OPRT_OK;
}

/**
 * @brief gpio_read tool callback
 *
 * All T5 pins P0-P55 support GPIO input.  The board pin number maps
 * directly to TUYA_GPIO_NUM_N.
 *
 * @param[in]  properties  pin (int 0-55)
 * @param[out] ret_val     "P<n>: HIGH" or "P<n>: LOW"
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_gpio_read(const MCP_PROPERTY_LIST_T *properties,
                                    MCP_RETURN_VALUE_T *ret_val,
                                    void *user_data)
{
    int pin = 0;
    if (!__get_int_prop(properties, "pin", &pin)) {
        ai_mcp_return_value_set_str(ret_val, "error: missing parameter pin");
        return OPRT_INVALID_PARM;
    }

    if (pin < 0 || pin >= T5_GPIO_PIN_MAX) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pin P%d is out of range, T5 supports P0-P%d",
                 pin, T5_GPIO_PIN_MAX - 1);
        ai_mcp_return_value_set_str(ret_val, buf);
        return OPRT_INVALID_PARM;
    }

    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
    };

    OPERATE_RET rt = tkl_gpio_init((TUYA_GPIO_NUM_E)pin, &cfg);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: gpio_init P%d failed, code %d", pin, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    TUYA_GPIO_LEVEL_E read_level = TUYA_GPIO_LEVEL_LOW;
    PR_NOTICE("gpio_read pin %d", pin);
    rt = tkl_gpio_read((TUYA_GPIO_NUM_E)pin, &read_level);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: gpio_read P%d failed, code %d", pin, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    char buf[HW_RESULT_BUF];
    snprintf(buf, sizeof(buf), "P%d: %s", pin,
             (read_level == TUYA_GPIO_LEVEL_HIGH) ? "HIGH" : "LOW");
    ai_mcp_return_value_set_str(ret_val, buf);
    return OPRT_OK;
}

/**
 * @brief i2c_scan tool callback
 *
 * Validates that the supplied SCL/SDA board pin pair is a supported T5 I2C
 * pair (see s_i2c_map).  If not, returns an error listing valid pairs.
 * Configures pinmux, initialises the I2C bus, scans addresses 0x00-0x7F,
 * then de-initialises.
 *
 * Valid T5 I2C pairs (SCL, SDA):
 *   (P20, P21) → I2C0
 *   (P0,  P1)  → I2C1
 *   (P14, P15) → I2C1
 *   (P38, P39) → I2C1
 *   (P42, P43) → I2C1
 *
 * @param[in]  properties  scl_pin (int), sda_pin (int)
 * @param[out] ret_val     Comma-separated list of found addresses or "none found"
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_i2c_scan(const MCP_PROPERTY_LIST_T *properties,
                                   MCP_RETURN_VALUE_T *ret_val,
                                   void *user_data)
{
    int scl_pin = 0;
    int sda_pin = 0;

    if (!__get_int_prop(properties, "scl_pin", &scl_pin) ||
        !__get_int_prop(properties, "sda_pin", &sda_pin)) {
        ai_mcp_return_value_set_str(ret_val,
            "error: missing parameter scl_pin or sda_pin");
        return OPRT_INVALID_PARM;
    }

    const T5_I2C_MAP_T *entry = NULL;
    if (!__i2c_lookup(scl_pin, sda_pin, &entry)) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: (P%d, P%d) is not a supported T5 I2C pair. "
                 "Valid pairs (SCL,SDA): (P20,P21),(P6,P7),(P14,P15),(P38,P39),(P42,P43)",
                 scl_pin, sda_pin);
        ai_mcp_return_value_set_str(ret_val, buf);
        return OPRT_INVALID_PARM;
    }

    tkl_io_pinmux_config((TUYA_PIN_NAME_E)entry->scl_pin, (TUYA_PIN_FUNC_E)entry->scl_func);
    tkl_io_pinmux_config((TUYA_PIN_NAME_E)entry->sda_pin, (TUYA_PIN_FUNC_E)entry->sda_func);

    TUYA_IIC_BASE_CFG_T cfg = {
        .role       = TUYA_IIC_MODE_MASTER,
        .speed      = TUYA_IIC_BUS_SPEED_100K,
        .addr_width = TUYA_IIC_ADDRESS_7BIT,
    };

    PR_NOTICE("i2c_scan scl_pin %d sda_pin %d", scl_pin, sda_pin);
    OPERATE_RET rt = tkl_i2c_init((TUYA_I2C_NUM_E)entry->i2c_port, &cfg);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: i2c_init port %d (P%d/P%d) failed, code %d",
                 entry->i2c_port, scl_pin, sda_pin, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    char result[HW_RESULT_BUF];
    int  pos   = 0;
    int  found = 0;

    pos += snprintf(result + pos, sizeof(result) - (size_t)pos,
                    "I2C%d (P%d/P%d) found devices: ",
                    entry->i2c_port, scl_pin, sda_pin);

    for (uint8_t addr = 0x00; addr <= 0x7F; addr++) {
        uint8_t dummy[1] = {0};
        if (tkl_i2c_master_send((TUYA_I2C_NUM_E)entry->i2c_port,
                                 addr, dummy, 0, TRUE) == OPRT_OK) {
            if (found > 0 && pos < (int)sizeof(result) - 8) {
                pos += snprintf(result + pos,
                                sizeof(result) - (size_t)pos, ", ");
            }
            if (pos < (int)sizeof(result) - 8) {
                pos += snprintf(result + pos,
                                sizeof(result) - (size_t)pos, "0x%02X", addr);
            }
            found++;
        }
    }

    if (found == 0) {
        snprintf(result, sizeof(result),
                 "I2C%d (P%d/P%d): no devices found",
                 entry->i2c_port, scl_pin, sda_pin);
    }

    tkl_i2c_deinit((TUYA_I2C_NUM_E)entry->i2c_port);

    ai_mcp_return_value_set_str(ret_val, result);
    return OPRT_OK;
}

/**
 * @brief adc_read tool callback
 *
 * Looks up the board pin in the T5 ADC mapping table to obtain the ADC
 * port and channel.  Performs a single-channel read and de-initialises.
 *
 * Supported T5 ADC pins:
 *   P25(ch1), P24(ch2), P23(ch3), P28(ch4), P22(ch5), P21(ch6),
 *   P8(ch10), P0(ch12), P1(ch13), P12(ch14), P13(ch15) — all on ADC port 0.
 *
 * @param[in]  properties  pin (int)
 * @param[out] ret_val     "P<n> (ch<c>): <value>"
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_adc_read(const MCP_PROPERTY_LIST_T *properties,
                                   MCP_RETURN_VALUE_T *ret_val,
                                   void *user_data)
{
    int pin = 0;
    if (!__get_int_prop(properties, "pin", &pin)) {
        ai_mcp_return_value_set_str(ret_val, "error: missing parameter pin");
        return OPRT_INVALID_PARM;
    }

    const T5_ADC_MAP_T *entry = NULL;
    if (!__adc_lookup(pin, &entry)) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: P%d does not support ADC on T5. "
                 "ADC pins: P25,P24,P23,P28,P22,P21,P8,P0,P1,P12,P13",
                 pin);
        ai_mcp_return_value_set_str(ret_val, buf);
        return OPRT_INVALID_PARM;
    }

    TUYA_ADC_BASE_CFG_T adc_cfg = {
        .ch_nums  = 1,
        .width    = 12,
        .mode     = TUYA_ADC_CONTINUOUS,
        .type     = TUYA_ADC_INNER_SAMPLE_VOL,
        .conv_cnt = 1,
    };
    adc_cfg.ch_list.data = (uint32_t)(1u << entry->channel);

    PR_NOTICE("adc_read pin %d channel %d", pin, entry->channel);
    OPERATE_RET rt = tkl_adc_init((TUYA_ADC_NUM_E)entry->adc_port, &adc_cfg);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: adc_init P%d (port %d ch %d) failed, code %d",
                 pin, entry->adc_port, entry->channel, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    int32_t adc_value = 0;
    rt = tkl_adc_read_single_channel((TUYA_ADC_NUM_E)entry->adc_port,
                                      entry->channel, &adc_value);
    tkl_adc_deinit((TUYA_ADC_NUM_E)entry->adc_port);

    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: adc_read P%d (ch %d) failed, code %d",
                 pin, entry->channel, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    char buf[HW_RESULT_BUF];
    snprintf(buf, sizeof(buf), "P%d (ch%d): %d", pin, entry->channel, (int)adc_value);
    ai_mcp_return_value_set_str(ret_val, buf);
    return OPRT_OK;
}

/**
 * @brief uart_write tool callback
 *
 * Lazily initialises UART 2 using T5 default pins P30(RX)/P31(TX) and
 * transmits the given string.
 *
 * @param[in]  properties  data (string)
 * @param[out] ret_val     "ok: sent N bytes via UART2 (P30/P31)"
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_uart_write(const MCP_PROPERTY_LIST_T *properties,
                                     MCP_RETURN_VALUE_T *ret_val,
                                     void *user_data)
{
    const char *data = __get_str_prop(properties, "data");
    if (!data) {
        ai_mcp_return_value_set_str(ret_val, "error: missing parameter data");
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = __uart_ensure_init();
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf), "error: UART2 init failed, code %d", rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    size_t len     = strlen(data);
    PR_NOTICE("uart_write data %s len %d", data, len);
    int    written = tal_uart_write(HW_UART_PORT, (const uint8_t *)data, (uint32_t)len);

    char buf[HW_RESULT_BUF];
    snprintf(buf, sizeof(buf),
             "ok: sent %d bytes via UART2 (P%d/P%d)",
             written, HW_UART2_DEFAULT_RX_PIN, HW_UART2_DEFAULT_TX_PIN);
    ai_mcp_return_value_set_str(ret_val, buf);
    return OPRT_OK;
}

/**
 * @brief uart_read tool callback
 *
 * Lazily initialises UART 2 using T5 default pins P30(RX)/P31(TX) and
 * reads all available bytes from the RX buffer (non-blocking).
 *
 * @param[in]  properties  (none)
 * @param[out] ret_val     Received string, or "no data" if buffer empty
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_uart_read(const MCP_PROPERTY_LIST_T *properties,
                                    MCP_RETURN_VALUE_T *ret_val,
                                    void *user_data)
{
    OPERATE_RET rt = __uart_ensure_init();
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf), "error: UART2 init failed, code %d", rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    char *rx_buf = (char *)claw_malloc(HW_UART_RX_BUF + 1);
    if (!rx_buf) {
        ai_mcp_return_value_set_str(ret_val, "error: malloc failed");
        return OPRT_MALLOC_FAILED;
    }

    int read_len = tal_uart_read(HW_UART_PORT, (uint8_t *)rx_buf, HW_UART_RX_BUF);
    PR_NOTICE("uart_read read_len %d rx_buf %s", read_len, rx_buf);
    if (read_len <= 0) {
        claw_free(rx_buf);
        ai_mcp_return_value_set_str(ret_val, "no data in UART2 buffer");
        return OPRT_OK;
    }

    rx_buf[read_len] = '\0';
    ai_mcp_return_value_set_str(ret_val, rx_buf);
    claw_free(rx_buf);
    return OPRT_OK;
}

/**
 * @brief pwm_set tool callback
 *
 * Looks up the board pin in the T5 PWM mapping table to obtain the PWM
 * port.  Initialises the PWM channel at 1000 Hz with the given duty cycle
 * and starts the output.
 *
 * Supported T5 PWM pins and their ports:
 *   P18→0, P24→1, P32→2, P34→3, P36→4, P19→5,
 *   P8→6,  P9→7,  P25→8, P33→9, P35→10, P37→11
 *
 * @param[in]  properties  pin (int), duty (int 1-10000)
 * @param[out] ret_val     "ok: P<n> (PWM<p>) duty=<d>/10000"
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_pwm_set(const MCP_PROPERTY_LIST_T *properties,
                                  MCP_RETURN_VALUE_T *ret_val,
                                  void *user_data)
{
    int pin  = 0;
    int duty = 0;

    if (!__get_int_prop(properties, "pin", &pin) ||
        !__get_int_prop(properties, "duty", &duty)) {
        ai_mcp_return_value_set_str(ret_val,
            "error: missing parameter pin or duty");
        return OPRT_INVALID_PARM;
    }

    const T5_PWM_MAP_T *entry = NULL;
    if (!__pwm_lookup(pin, &entry)) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: P%d does not support PWM on T5. "
                 "PWM pins: P18,P24,P32,P34,P36,P19,P8,P9,P25,P33,P35,P37",
                 pin);
        ai_mcp_return_value_set_str(ret_val, buf);
        return OPRT_INVALID_PARM;
    }

    if (duty < 1 || duty > 10000) {
        ai_mcp_return_value_set_str(ret_val,
            "error: duty must be in range 1-10000 (10000 = 100%)");
        return OPRT_INVALID_PARM;
    }

    TUYA_PWM_BASE_CFG_T pwm_cfg = {
        .duty       = (uint32_t)duty,
        .cycle      = 10000,
        .frequency  = 1000,
        .polarity   = TUYA_PWM_POSITIVE,
        .count_mode = TUYA_PWM_CNT_UP,
    };

    PR_NOTICE("pwm_set pin %d duty %d", pin, duty);
    OPERATE_RET rt = tkl_pwm_init((TUYA_PWM_NUM_E)entry->pwm_port, &pwm_cfg);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pwm_init P%d (port %d) failed, code %d",
                 pin, entry->pwm_port, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    PR_NOTICE("pwm_start pin %d", pin);
    rt = tkl_pwm_start((TUYA_PWM_NUM_E)entry->pwm_port);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pwm_start P%d (port %d) failed, code %d",
                 pin, entry->pwm_port, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    PR_NOTICE("pwm_duty_set pin %d duty %d", pin, duty);
    rt = tkl_pwm_duty_set((TUYA_PWM_NUM_E)entry->pwm_port, duty);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pwm_duty_set P%d (port %d) failed, code %d",
                 pin, entry->pwm_port, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    char buf[HW_RESULT_BUF];
    snprintf(buf, sizeof(buf),
             "ok: P%d (PWM%d) started, duty=%d/10000 (%.2f%%), freq=1000Hz",
             pin, entry->pwm_port, duty, (float)duty / 100.0f);
    ai_mcp_return_value_set_str(ret_val, buf);
    return OPRT_OK;
}

/**
 * @brief servo_set tool callback
 *
 * Looks up the board pin in the T5 PWM mapping table.  Drives the servo
 * at 50 Hz; maps angle 0-180° to duty counts 250-1250 (pulse 0.5-2.5 ms).
 *
 * duty = HW_SERVO_DUTY_MIN + angle * (HW_SERVO_DUTY_MAX - HW_SERVO_DUTY_MIN) / 180
 *
 * Supported T5 PWM pins (same as pwm_set):
 *   P18,P24,P32,P34,P36,P19,P8,P9,P25,P33,P35,P37
 *
 * @param[in]  properties  pin (int), angle (int 0-180)
 * @param[out] ret_val     "ok: P<n> servo at <a>° (duty=<d>)"
 * @param[in]  user_data   Unused
 * @return OPRT_OK on success, error code on failure
 */
static OPERATE_RET __tool_servo_set(const MCP_PROPERTY_LIST_T *properties,
                                    MCP_RETURN_VALUE_T *ret_val,
                                    void *user_data)
{
    int pin   = 0;
    int angle = 0;

    if (!__get_int_prop(properties, "pin", &pin) ||
        !__get_int_prop(properties, "angle", &angle)) {
        ai_mcp_return_value_set_str(ret_val,
            "error: missing parameter pin or angle");
        return OPRT_INVALID_PARM;
    }

    const T5_PWM_MAP_T *entry = NULL;
    if (!__pwm_lookup(pin, &entry)) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: P%d does not support PWM/servo on T5. "
                 "PWM pins: P18,P24,P32,P34,P36,P19,P8,P9,P25,P33,P35,P37",
                 pin);
        ai_mcp_return_value_set_str(ret_val, buf);
        return OPRT_INVALID_PARM;
    }

    if (angle < 0 || angle > 180) {
        ai_mcp_return_value_set_str(ret_val,
            "error: angle must be in range 0-180 degrees");
        return OPRT_INVALID_PARM;
    }

    int duty = HW_SERVO_DUTY_MIN +
               angle * (HW_SERVO_DUTY_MAX - HW_SERVO_DUTY_MIN) / 180;

    // duty = 10000 - duty;
    TUYA_PWM_BASE_CFG_T pwm_cfg = {
        .duty       = (uint32_t)duty,
        .cycle      = 10000,
        .frequency  = HW_SERVO_FREQ_HZ,
        .polarity   = TUYA_PWM_POSITIVE,
        .count_mode = TUYA_PWM_CNT_UP,
    };

    PR_NOTICE("servo_set pin %d angle %d, duty %d", pin, angle, duty);
    OPERATE_RET rt = tkl_pwm_init((TUYA_PWM_NUM_E)entry->pwm_port, &pwm_cfg);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pwm_init P%d (port %d) failed, code %d",
                 pin, entry->pwm_port, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    PR_NOTICE("pwm_start pin %d", pin);
    rt = tkl_pwm_start((TUYA_PWM_NUM_E)entry->pwm_port);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pwm_start P%d (port %d) failed, code %d",
                 pin, entry->pwm_port, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    PR_NOTICE("pwm_duty_set pin %d duty %d", pin, duty);
    rt = tkl_pwm_duty_set((TUYA_PWM_NUM_E)entry->pwm_port, duty);
    if (rt != OPRT_OK) {
        char buf[HW_RESULT_BUF];
        snprintf(buf, sizeof(buf),
                 "error: pwm_duty_set P%d (port %d) failed, code %d",
                 pin, entry->pwm_port, rt);
        ai_mcp_return_value_set_str(ret_val, buf);
        return rt;
    }

    char buf[HW_RESULT_BUF];
    snprintf(buf, sizeof(buf),
             "ok: P%d (PWM%d) servo at %d degrees "
             "(duty=%d, pulse=%.2f ms, freq=%d Hz)",
             pin, entry->pwm_port, angle, duty,
             (float)duty * 20.0f / 10000.0f,
             HW_SERVO_FREQ_HZ);
    ai_mcp_return_value_set_str(ret_val, buf);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Tool registration
 * --------------------------------------------------------------------------- */

/**
 * @brief Register all hardware MCP tools
 * @return OPRT_OK on success, error code on failure
 * @note Must be called after ai_mcp_server_init().
 */
OPERATE_RET tool_hw_register(void)
{
    OPERATE_RET rt = OPRT_OK;

    /* gpio_write */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "gpio_write",
            "Set a T5 board pin (P0-P55) to output mode and drive it HIGH or LOW. "
            "All T5 pins support GPIO output.",
            __tool_gpio_write, NULL,
            MCP_PROP_INT_RANGE("pin", "Board pin number (0-55, e.g. 18 for P18)", 0, 55),
            MCP_PROP_INT_DEF_RANGE("level",
                "Output level: 1=HIGH, 0=LOW", 0, 0, 1)
        )
    );

    /* gpio_read */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "gpio_read",
            "Configure a T5 board pin (P0-P55) as pull-up input and read its level. "
            "Returns HIGH or LOW.",
            __tool_gpio_read, NULL,
            MCP_PROP_INT_RANGE("pin", "Board pin number (0-55)", 0, 55)
        )
    );

    /* i2c_scan */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "i2c_scan",
            "Scan an I2C bus using the specified T5 SCL/SDA pin pair and return "
            "found 7-bit device addresses. "
            "Valid T5 pairs (SCL,SDA): (P20,P21),(P6,P7),(P14,P15),(P38,P39),(P42,P43).",
            __tool_i2c_scan, NULL,
            MCP_PROP_INT_RANGE("scl_pin", "SCL board pin number", 0, 55),
            MCP_PROP_INT_RANGE("sda_pin", "SDA board pin number", 0, 55)
        )
    );

    /* adc_read */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "adc_read",
            "Read the 12-bit ADC sampled value on a T5 board pin. "
            "Supported pins: P25,P24,P23,P28,P22,P21,P8,P0,P1,P12,P13.",
            __tool_adc_read, NULL,
            MCP_PROP_INT_RANGE("pin", "Board pin number with ADC capability", 0, 55)
        )
    );

    /* uart_write */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "uart_write",
            "Send a string over UART 2 (115200 8N1). "
            "T5 default pins: P30=RX, P31=TX. "
            "UART is initialised automatically on first use.",
            __tool_uart_write, NULL,
            MCP_PROP_STR("data", "String to transmit over UART 2")
        )
    );

    /* uart_read */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "uart_read",
            "Read all buffered bytes from UART 2 receive buffer (P30/P31). "
            "Returns received string, or 'no data' if buffer is empty.",
            __tool_uart_read, NULL
        )
    );

    /* pwm_set */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "pwm_set",
            "Initialise a T5 PWM pin at 1000 Hz and start the output. "
            "Supported pins: P18,P24,P32,P34,P36,P19,P8,P9,P25,P33,P35,P37. "
            "duty: 1-10000 (10000=100%).",
            __tool_pwm_set, NULL,
            MCP_PROP_INT_RANGE("pin", "Board pin number with PWM capability", 0, 55),
            MCP_PROP_INT_DEF_RANGE("duty",
                "Duty cycle 1-10000 (10000=100%)", 5000, 1, 10000)
        )
    );

    /* servo_set */
    TUYA_CALL_ERR_RETURN(
        AI_MCP_TOOL_ADD(
            "servo_set",
            "Drive a servo to the given angle via T5 PWM pin (50 Hz). "
            "Pulse 0.5 ms=0° to 2.5 ms=180°. "
            "Supported pins: P18,P24,P32,P34,P36,P19,P8,P9,P25,P33,P35,P37.",
            __tool_servo_set, NULL,
            MCP_PROP_INT_RANGE("pin", "Board pin number connected to servo signal", 0, 55),
            MCP_PROP_INT_DEF_RANGE("angle",
                "Target angle in degrees (0-180)", 90, 0, 180)
        )
    );

    PR_DEBUG("Hardware MCP tools registered: gpio_write/read, i2c_scan, "
             "adc_read, uart_write/read, pwm_set, servo_set");
    return rt;
}

/**
 * @file ducky_claw_chat_bot.c
 * @version 0.1
 * @date 2025-03-25
 */

#include "tal_api.h"
#include "tal_thread.h"
#include "netmgr.h"

#include "ai_agent.h"
#include "ai_chat_main.h"
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
#include "ai_ui_camera.h"
#endif
#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
#include "ai_video_input.h"
#endif
#include "ducky_claw_chat.h"
#include "agent_loop.h"
#include "acp_client.h"

#include "app_im.h"
#include "sys_bus.h"
#include "tal_log.h"

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "tkl_wifi.h"
#endif
/***********************************************************
************************macro define************************
***********************************************************/
#define PRINTF_FREE_HEAP_TTIME (10 * 1000)
#define DISP_NET_STATUS_TIME   (1 * 1000)

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
***********************const declaration********************
***********************************************************/

/***********************************************************
***********************variable define**********************
***********************************************************/
static TIMER_ID sg_printf_heap_tm;

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
static AI_UI_WIFI_STATUS_E sg_wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
static TIMER_ID            sg_disp_status_tm;
#endif
/***********************************************************
***********************function define**********************
***********************************************************/

static void __printf_free_heap_tm_cb(TIMER_ID timer_id, void *arg)
{
    // extern void tal_thread_dump_watermark(void);
    // tal_thread_dump_watermark();
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    uint32_t free_heap       = tal_system_get_free_heap_size();
    uint32_t free_psram_heap = tal_psram_get_free_heap_size();
    PR_INFO("Free heap size:%d, Free psram heap size:%d", free_heap, free_psram_heap);
#else
    uint32_t free_heap = tal_system_get_free_heap_size();
    PR_INFO("Free heap size:%d", free_heap);
#endif
}

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
static void __display_net_status_update(void)
{
    AI_UI_WIFI_STATUS_E wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
    netmgr_status_e     net_status  = NETMGR_LINK_DOWN;

    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &net_status);
    if (net_status == NETMGR_LINK_UP) {
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
        // get rssi
        int8_t rssi = 0;
#ifndef PLATFORM_T5
        // BUG: Getting RSSI causes a crash on T5 platform
        tkl_wifi_station_get_conn_ap_rssi(&rssi);
#endif
        if (rssi >= -60) {
            wifi_status = AI_UI_WIFI_STATUS_GOOD;
        } else if (rssi >= -70) {
            wifi_status = AI_UI_WIFI_STATUS_FAIR;
        } else {
            wifi_status = AI_UI_WIFI_STATUS_WEAK;
        }
#else
        wifi_status = AI_UI_WIFI_STATUS_GOOD;
#endif
    } else {
        wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
    }

    if (wifi_status != sg_wifi_status) {
        sg_wifi_status = wifi_status;
        ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&wifi_status, sizeof(AI_UI_WIFI_STATUS_E));
    }
}

static void __display_status_tm_cb(TIMER_ID timer_id, void *arg)
{
    __display_net_status_update();
}

#endif

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
static void __ai_video_display_flush(TDL_CAMERA_FRAME_T *frame)
{
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    AI_UI_VIDEO_T ui_video = {
        .width  = frame->width,
        .height = frame->height,
        .yuv422 = frame->data,
        .len    = frame->data_len,
    };
    ai_ui_camera_flush(&ui_video);
#endif
}
#endif


#define STREAM_DATA_MAX_LEN (16*1024)

static void __ai_chat_handle_event(AI_NOTIFY_EVENT_T *event)
{
    static char *stream_data = NULL;
    static uint32_t data_write_offset = 0;
    (void)event;

    if (NULL == event) {
        return;
    }


    switch (event->type) {
    case AI_USER_EVT_ASR_OK: {
        /* Restore TTS and UI output for this iteration. */
#if 0
        AI_NOTIFY_TEXT_T *asr = (AI_NOTIFY_TEXT_T *)event->data;
        if (!asr || asr->datalen == 0 || !asr->data) {
            break;
        }
        char *asr_text = (char *)tal_malloc(asr->datalen + 1);
        if (asr_text) {
            memcpy(asr_text, asr->data, asr->datalen);
            asr_text[asr->datalen] = '\0';
            PR_DEBUG("[acp] asr inbound: %.64s", asr_text);

            sys_msg_t msg = {0};
            strncpy(msg.channel, SYS_CHAN_ACP, sizeof(msg.channel) - 1);
            msg.channel[sizeof(msg.channel) - 1] = '\0';
            PR_DEBUG("asr inbound: channel=%s chat_id=%s", msg.channel, msg.chat_id);
            msg.content = asr_text;
            if (sys_bus_push_inbound(&msg) != OPRT_OK) {
                PR_ERR("sys_bus_push_inbound failed");
                tal_free(asr_text);
            }
        }
#endif
    } break;
    case AI_USER_EVT_TEXT_STREAM_START: {
        if (stream_data == NULL) {
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
            stream_data = tal_psram_malloc(STREAM_DATA_MAX_LEN);
#else
            stream_data = tal_malloc(STREAM_DATA_MAX_LEN);
#endif
            if (stream_data == NULL) {
                PR_ERR("Failed to allocate stream data memory");
                return;
            }
        }
        memset(stream_data, 0, STREAM_DATA_MAX_LEN);
        data_write_offset = 0;

        AI_NOTIFY_TEXT_T *text = (AI_NOTIFY_TEXT_T *)event->data;
        if (text && text->datalen > 0 && text->data && data_write_offset + text->datalen <= STREAM_DATA_MAX_LEN) {
            memcpy(stream_data + data_write_offset, text->data, text->datalen);
            data_write_offset += text->datalen;
        }
    } break;
    case AI_USER_EVT_TEXT_STREAM_DATA: {
        AI_NOTIFY_TEXT_T *text = (AI_NOTIFY_TEXT_T *)event->data;

        if (data_write_offset + text->datalen >= STREAM_DATA_MAX_LEN) {
            /* Only flush to IM if we are NOT in a tool-loop iteration */
            if (!agent_loop_in_tool_loop()) {
                app_im_bot_send_message((char *)stream_data);
            }
            memset(stream_data, 0, STREAM_DATA_MAX_LEN);
            data_write_offset = 0;
        }

        memcpy(stream_data + data_write_offset, text->data, text->datalen);
        data_write_offset += text->datalen;
    } break;
    case AI_USER_EVT_TEXT_STREAM_STOP: {
        /* Accumulate the final chunk into stream_data; do NOT post the
         * semaphore here.  The AI may still have MCP tool calls to execute
         * after the text stream ends.  We wait for AI_USER_EVT_END instead. */
        build_current_context("assistant", (char *)stream_data);
        /* Keep stream_data intact so AI_USER_EVT_END can read it */
    } break;
    case AI_USER_EVT_END: {
        /* The full AI turn is complete (text + all MCP tool calls done).
         * Pass the accumulated text to the agent loop and unblock it. */
        agent_loop_set_last_response((char *)stream_data);
        agent_loop_notify_turn_done();

        memset(stream_data, 0, STREAM_DATA_MAX_LEN);
        data_write_offset = 0;
    } break;
    default: break;
    }
}

OPERATE_RET ducky_claw_chat_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    AI_CHAT_MODE_CFG_T ai_chat_cfg = {
        .default_mode = AI_CHAT_MODE_WAKEUP,
        .default_vol  = 70,
        .evt_cb       = __ai_chat_handle_event,
    };
    TUYA_CALL_ERR_RETURN(ai_chat_init(&ai_chat_cfg));

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
    TUYA_CALL_ERR_LOG(ai_video_init());
    TUYA_CALL_ERR_LOG(ai_video_set_yuv_frame_flush_cb(__ai_video_display_flush));
#endif

#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)
    TUYA_CALL_ERR_RETURN(ai_mcp_init());
#endif

    // Free heap size
    tal_sw_timer_create(__printf_free_heap_tm_cb, NULL, &sg_printf_heap_tm);
    tal_sw_timer_start(sg_printf_heap_tm, PRINTF_FREE_HEAP_TTIME, TAL_TIMER_CYCLE);

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&sg_wifi_status, sizeof(AI_UI_WIFI_STATUS_E));

    ai_ui_disp_msg(AI_UI_DISP_STATUS, (uint8_t *)INITIALIZING, strlen(INITIALIZING));
    ai_ui_disp_msg(AI_UI_DISP_EMOTION, (uint8_t *)EMOJI_NEUTRAL, strlen(EMOJI_NEUTRAL));

    // display status update
    tal_sw_timer_create(__display_status_tm_cb, NULL, &sg_disp_status_tm);
    tal_sw_timer_start(sg_disp_status_tm, DISP_NET_STATUS_TIME, TAL_TIMER_CYCLE);
#endif

    return OPRT_OK;
}

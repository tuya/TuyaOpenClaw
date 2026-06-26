/**
 * @file heartbeat.c
 * @brief Heartbeat service for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * The heartbeat service periodically checks a Markdown file (HEARTBEAT.md)
 * for actionable tasks. If any uncompleted tasks are found, it sends
 * a prompt to the AI agent via ai_agent_send_text, instructing it to
 * read the file and follow the instructions.
 */

#include "heartbeat.h"
#include "tool_files.h"

#include "tal_api.h"
#include "sys_bus.h"

#include <ctype.h>
#include <string.h>

/***********************************************************
************************macro define************************
***********************************************************/

/* Heartbeat file path */
#define CLAW_HEARTBEAT_FILE   CLAW_FS_ROOT_PATH "/HEARTBEAT.md"

/* Prompt sent to agent when tasks are found */
#define HEARTBEAT_PROMPT \
    "Read " CLAW_HEARTBEAT_FILE \
    " and follow any instructions or tasks listed there. " \
    "If nothing needs attention, reply with just: HEARTBEAT_OK"

/***********************************************************
***********************variable define**********************
***********************************************************/

static TIMER_ID s_heartbeat_timer = NULL;

/***********************************************************
***********************function define**********************
***********************************************************/

/**
 * @brief Check if HEARTBEAT.md contains actionable tasks
 *
 * Parses the file looking for non-empty, non-comment lines that
 * are not marked as completed (i.e., not checked with [x]).
 *
 * @return true if uncompleted tasks exist
 */
static bool heartbeat_has_tasks(void)
{
    TUYA_FILE f = claw_fopen(CLAW_HEARTBEAT_FILE, "r");
    if (!f) {
        return false;
    }

    char *line = (char *)claw_malloc(256);
    if (!line) {
        claw_fclose(f);
        return false;
    }
    memset(line, 0, 256);
    bool found_task = false;

    while (claw_fgets(line, 256, f)) {
        const char *p = line;

        /* Skip leading whitespace */
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        /* Skip empty lines */
        if (*p == '\0' || *p == '\n' || *p == '\r') {
            continue;
        }

        /* Skip comment lines (headings) */
        if (*p == '#') {
            continue;
        }

        /* Check for completed checkbox: - [x] or * [x] */
        if ((*p == '-' || *p == '*') && *(p + 1) == ' ' && *(p + 2) == '[') {
            char mark = *(p + 3);
            if ((mark == 'x' || mark == 'X') && *(p + 4) == ']') {
                continue; /* Completed task, skip */
            }
        }

        /* Found an actionable task */
        found_task = true;
        break;
    }

    claw_free(line);
    claw_fclose(f);
    return found_task;
}

/**
 * @brief Send heartbeat check message to AI agent
 *
 * @return true if message was sent successfully
 */
static bool heartbeat_send(void)
{
    if (!heartbeat_has_tasks()) {
        PR_DEBUG("No actionable tasks in HEARTBEAT.md");
        return false;
    }

    sys_msg_t im = {0};
    strncpy(im.channel, SYS_CHAN_HEARTBEAT, sizeof(im.channel) - 1);
    im.content = claw_malloc(strlen(HEARTBEAT_PROMPT) + 1);
    if (!im.content) {
        PR_ERR("heartbeat: malloc failed");
        return false;
    }
    strcpy(im.content, HEARTBEAT_PROMPT);

    OPERATE_RET rt = sys_bus_push_inbound(&im);
    if (rt != OPRT_OK) {
        PR_WARN("Send heartbeat message failed: %d", rt);
        return false;
    }

    PR_INFO("Triggered agent heartbeat check");
    return true;
}

/**
 * @brief Timer callback for periodic heartbeat checks
 */
static void heartbeat_timer_callback(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    (void)heartbeat_send();
}

/* ===== Public API ===== */

OPERATE_RET heartbeat_init(void)
{
    PR_INFO("Heartbeat initialized (file=%s interval=%u s)",
            CLAW_HEARTBEAT_FILE,
            (unsigned)(CLAW_HEARTBEAT_INTERVAL_MS / 1000));
    return OPRT_OK;
}

OPERATE_RET heartbeat_start(void)
{
    if (s_heartbeat_timer) {
        PR_WARN("Heartbeat already running");
        return OPRT_OK;
    }

    OPERATE_RET rt = tal_sw_timer_create(heartbeat_timer_callback, NULL,
                                          &s_heartbeat_timer);
    if (rt != OPRT_OK || !s_heartbeat_timer) {
        PR_ERR("Create heartbeat timer failed: %d", rt);
        s_heartbeat_timer = NULL;
        return rt == OPRT_OK ? OPRT_COM_ERROR : rt;
    }

    rt = tal_sw_timer_start(s_heartbeat_timer, CLAW_HEARTBEAT_INTERVAL_MS,
                            TAL_TIMER_CYCLE);
    if (rt != OPRT_OK) {
        PR_ERR("Start heartbeat timer failed: %d", rt);
        (void)tal_sw_timer_delete(s_heartbeat_timer);
        s_heartbeat_timer = NULL;
        return rt;
    }

    PR_INFO("Heartbeat started (every %u min)",
            (unsigned)(CLAW_HEARTBEAT_INTERVAL_MS / 60000));
    return OPRT_OK;
}

void heartbeat_stop(void)
{
    if (s_heartbeat_timer) {
        (void)tal_sw_timer_stop(s_heartbeat_timer);
        (void)tal_sw_timer_delete(s_heartbeat_timer);
        s_heartbeat_timer = NULL;
        PR_INFO("Heartbeat stopped");
    }
}

bool heartbeat_trigger(void)
{
    return heartbeat_send();
}

/**
 * @file agent_loop.c
 * @brief Agent main loop: synchronous inner tool-use loop modelled after
 *        the mimiclaw reference implementation.
 *
 * Design (mirrors mimiclaw/agent/agent_loop.c):
 *
 *   outer loop: wait for inbound user message
 *   inner loop (up to TOOL_LOOP_MAX iterations):
 *     1. Build system prompt + history + current content
 *     2. Send to cloud AI (ai_agent_send_text)
 *     3. BLOCK on s_turn.sem until cloud AI finishes (STREAM_STOP fires)
 *     4. If a tool was called this iteration → continue inner loop
 *        (tool result already recorded in history by __on_tool_executed)
 *     5. If no tool called → final response, forward to IM, break
 *
 * @version 0.4
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "agent_loop.h"
#include "context_builder.h"
#include "tool_files.h"

#include "cJSON.h"
#include "tal_api.h"
#include "tal_mutex.h"
#include "tal_semaphore.h"
#include <stdint.h>
#include <string.h>

#include "sys_bus.h"
#include "app_im.h"
#include "ai_agent.h"
#include "ai_chat_main.h"
#include "tuya_cloud_types.h"
#include "tuya_error_code.h"
#include "ai_mcp_server.h"

/***********************************************************
************************macro define************************
***********************************************************/
#ifndef DUCKY_CLAW_AGENT_STACK
#define DUCKY_CLAW_AGENT_STACK   (24*1024)
#endif

#ifndef DUCKY_CLAW_CONTEXT_BUF_SIZE
#define DUCKY_CLAW_CONTEXT_BUF_SIZE   (32 * 1024)
#endif

#ifndef DUCKY_CLAW_HISTORY_MAX_COUNT
#define DUCKY_CLAW_HISTORY_MAX_COUNT  10
#endif

/* Maximum inner-loop iterations per user turn (mirrors MIMI_AGENT_MAX_TOOL_ITER) */
#define TOOL_LOOP_MAX   10

/* Timeout waiting for cloud AI to finish one full round including all MCP
 * tool calls within that round (ms).  A single AI session may invoke several
 * tools sequentially before emitting AI_EVENT_END, so allow 120 s. */
#define TURN_WAIT_MS    (120 * 1000)

/* Buffer size for a single tool result string.
 * Must be large enough to hold the full cron_list / read_file output.
 * 2 KB covers ~10 cron jobs with full field text. */
#define TOOL_RESULT_BUF_SIZE  (512)

/* Buffer holding the last complete AI text stream, set by
 * agent_loop_set_last_response() called from ducky_claw_chat.c on STREAM_STOP.
 * The inner loop forwards it to IM when no tool was called. */
#define LAST_RESPONSE_MAX  (16 * 1024)
/***********************************************************
***********************typedef define***********************
***********************************************************/

static const char *__json_get_string(const cJSON *item)
{
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }

    return item->valuestring;
}

/**
 * Per-turn state shared between agent_loop_task (consumer) and the
 * STREAM_STOP callback / tool hook (producers).
 */
typedef struct {
    SEM_HANDLE   sem;           /* Posted when cloud AI finishes one round  */
    MUTEX_HANDLE lock;          /* Guards tool_called / tool_result         */
    bool         tool_called;   /* True if __on_tool_executed fired         */
    bool         tool_call_ok;  /* True if the tool returned OPRT_OK       */
    char         tool_result[TOOL_RESULT_BUF_SIZE]; /* Last tool summary    */
} turn_state_t;

/***********************************************************
***********************variable define**********************
***********************************************************/
static THREAD_HANDLE s_agent_loop_thread = NULL;

static char *s_total_prompt = NULL;

static MUTEX_HANDLE s_history_mutex = NULL;
static cJSON       *s_history_json  = NULL;

static turn_state_t s_turn = {0};

static char        *s_last_response      = NULL;
static MUTEX_HANDLE s_last_response_lock = NULL;

/* True while the inner loop is active (i.e. we are in a tool-use iteration).
 * ducky_claw_chat.c uses this to suppress mid-stream overflow flushes. */
static volatile bool s_in_tool_loop = false;

/***********************************************************
***********************function define**********************
***********************************************************/

static const char *__oprt_to_str(OPERATE_RET rt)
{
    switch (rt) {
    case OPRT_OK:                   return "success";
    case OPRT_COM_ERROR:            return "common error";
    case OPRT_INVALID_PARM:         return "invalid parameter";
    case OPRT_MALLOC_FAILED:        return "memory allocation failed";
    case OPRT_NOT_SUPPORTED:        return "not supported";
    case OPRT_NETWORK_ERROR:        return "network error";
    case OPRT_NOT_FOUND:            return "not found";
    case OPRT_TIMEOUT:              return "timeout";
    case OPRT_FILE_NOT_FIND:        return "file not found";
    case OPRT_FILE_OPEN_FAILED:     return "file open failed";
    case OPRT_FILE_READ_FAILED:     return "file read failed";
    case OPRT_FILE_WRITE_FAILED:    return "file write failed";
    case OPRT_NOT_EXIST:            return "not exist";
    case OPRT_BUFFER_NOT_ENOUGH:    return "buffer not enough";
    case OPRT_RESOURCE_NOT_READY:   return "resource not ready";
    case OPRT_EXCEED_UPPER_LIMIT:   return "exceed upper limit";
    default:                        return "unknown error";
    }
}

/**
 * __on_tool_executed - MCP tool execution hook.
 *
 * Called synchronously after every cloud-AI-initiated tool call.
 * Records the result in conversation history and in s_turn.tool_result
 * so the inner loop can include it in the next prompt iteration.
 */
static void __on_tool_executed(const char *tool_name, OPERATE_RET rt,
                                const MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    if (!tool_name) {
        return;
    }

    char   buf[TOOL_RESULT_BUF_SIZE];
    size_t off = 0;
    int    len;

    len = snprintf(buf + off, TOOL_RESULT_BUF_SIZE - off,
                 "Tool \"%s\": %s (code=%d)",
                 tool_name, __oprt_to_str(rt), rt);
    if (len > 0) {
        off += (size_t)len;
    }

    if (rt == OPRT_OK && ret_val && off < TOOL_RESULT_BUF_SIZE - 2) {
        switch (ret_val->type) {
        case MCP_RETURN_TYPE_BOOLEAN:
            len = snprintf(buf + off, TOOL_RESULT_BUF_SIZE - off,
                         ", result=%s", ret_val->bool_val ? "true" : "false");
            break;
        case MCP_RETURN_TYPE_INTEGER:
            len = snprintf(buf + off, TOOL_RESULT_BUF_SIZE - off,
                         ", result=%d", ret_val->int_val);
            break;
        case MCP_RETURN_TYPE_STRING:
            if (ret_val->str_val) {
                len = snprintf(buf + off, TOOL_RESULT_BUF_SIZE - off,
                             ", result=%s", ret_val->str_val);
            } else {
                len = 0;
            }
            break;
        case MCP_RETURN_TYPE_JSON:
            if (ret_val->json_val) {
                char *s = cJSON_PrintUnformatted(ret_val->json_val);
                if (s) {
                    len = snprintf(buf + off, TOOL_RESULT_BUF_SIZE - off,
                                 ", result=%s", s);
                    cJSON_free(s);
                } else {
                    len = 0;
                }
            } else {
                len = 0;
            }
            break;
        default:
            len = 0;
            break;
        }
        if (len > 0 && off + (size_t)len < TOOL_RESULT_BUF_SIZE) {
            off += (size_t)len;
        }
    }
    buf[off < TOOL_RESULT_BUF_SIZE ? off : TOOL_RESULT_BUF_SIZE - 1] = '\0';

    PR_DEBUG("Tool exec hook: %s", buf);

    /* Record in sliding-window history */
    build_current_context("tool", buf);

    /* Signal the inner loop that a tool was called this round */
    if (s_turn.lock) {
        tal_mutex_lock(s_turn.lock);
        s_turn.tool_called = true;
        s_turn.tool_call_ok = (rt == OPRT_OK);
        strncpy(s_turn.tool_result, buf, TOOL_RESULT_BUF_SIZE - 1);
        s_turn.tool_result[TOOL_RESULT_BUF_SIZE - 1] = '\0';
        tal_mutex_unlock(s_turn.lock);
    }

    /* Suppress TTS and UI output for the remainder of this iteration. */
    // ai_agent_set_tts_suppressed(TRUE);
    // ai_chat_ui_set_output_suppressed(TRUE);
}

/**
 * build_current_context - Append a role/content pair to the shared history.
 */
OPERATE_RET build_current_context(const char *role, const char *content)
{
    if (!s_history_mutex || !s_history_json) {
        return OPRT_INVALID_PARM;
    }

    tal_mutex_lock(s_history_mutex);

    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        tal_mutex_unlock(s_history_mutex);
        return OPRT_MALLOC_FAILED;
    }

    int count = cJSON_GetArraySize(s_history_json);
    if (count >= DUCKY_CLAW_HISTORY_MAX_COUNT) {
        cJSON_DeleteItemFromArray(s_history_json, 0);
    }

    cJSON_AddStringToObject(entry, "role", role);
    cJSON_AddStringToObject(entry, "content", content);
    cJSON_AddItemToArray(s_history_json, entry);

    tal_mutex_unlock(s_history_mutex);
    return OPRT_OK;
}

/**
 * agent_loop_in_tool_loop - Returns true while the inner tool loop is active.
 *
 * Used by ducky_claw_chat.c to suppress intermediate IM flushes.
 */
bool agent_loop_in_tool_loop(void)
{
    return s_in_tool_loop;
}

/**
 * agent_loop_set_last_response - Store the completed AI text stream.
 *
 * Called by ducky_claw_chat.c on AI_USER_EVT_TEXT_STREAM_STOP before
 * calling agent_loop_notify_turn_done().  The inner loop reads this to
 * forward the final response to IM.
 */
void agent_loop_set_last_response(const char *text)
{
    if (!s_last_response || !s_last_response_lock || !text) {
        return;
    }
    tal_mutex_lock(s_last_response_lock);
    strncpy(s_last_response, text, LAST_RESPONSE_MAX - 1);
    s_last_response[LAST_RESPONSE_MAX - 1] = '\0';
    tal_mutex_unlock(s_last_response_lock);
}

/**
 * agent_loop_notify_turn_done - Called by ducky_claw_chat.c on STREAM_STOP.
 *
 * Posts the semaphore so the blocked inner loop can proceed to the next
 * iteration check.
 */
void agent_loop_notify_turn_done(void)
{
    if (s_turn.sem) {
        tal_semaphore_post(s_turn.sem);
    }
}

/**
 * __build_and_send - Build the full prompt and send it to the cloud AI.
 *
 * @content:   Current message content to append (user text or tool result).
 * @is_tool:   True when content is a tool result (uses different section header).
 *             Also used as the skip-system-prompt flag: the system prompt is only
 *             sent on the first iteration (is_tool == false) to avoid re-sending
 *             the large static context on every tool-loop round.
 * @summarize: True when the loop limit has been reached; instructs the AI to
 *             summarise what it has done so far and stop calling tools.
 */
static void __build_and_send(const char *content, bool is_tool, bool summarize)
{
    memset(s_total_prompt, 0, DUCKY_CLAW_CONTEXT_BUF_SIZE);
    size_t off = 0;
    if (!is_tool) {
        /* First iteration: include full system prompt (tools, rules, memory, skills) */
        off = context_build_system_prompt(s_total_prompt, DUCKY_CLAW_CONTEXT_BUF_SIZE);
        if (off == 0) {
            PR_ERR("context_build_system_prompt failed");
            return;
        }
    }

    /* Append sliding-window history */
    tal_mutex_lock(s_history_mutex);
    int count = cJSON_GetArraySize(s_history_json);
    if (count > 0) {
        off += snprintf(s_total_prompt + off, DUCKY_CLAW_CONTEXT_BUF_SIZE - off,
                        "\r\n\n# Recent Memory History\r\n");
        for (int j = 0; j < count; j++) {
            cJSON      *e    = cJSON_GetArrayItem(s_history_json, j);
            const char *role = __json_get_string(cJSON_GetObjectItem(e, "role"));
            const char *text = __json_get_string(cJSON_GetObjectItem(e, "content"));
            if (role && text) {
                off += snprintf(s_total_prompt + off, DUCKY_CLAW_CONTEXT_BUF_SIZE - off,
                                "\n- %s: %s", role, text);
            }
        }
    }
    tal_mutex_unlock(s_history_mutex);

    /* Append current content */
    const char *header;
    if (summarize) {
        header = "\n\n# Maximum Iterations Reached\n"
                 "You have reached the tool-call limit for this turn. "
                 "Do NOT call any more tools. "
                 "Instead, summarise what you have accomplished so far, "
                 "what succeeded, what failed, and any next steps the user should take. "
                 "Reply in the same language the user used. "
                 "Last tool result:\n";
    } else if (is_tool) {
        header = "\n\n# Tool Execution Result\n"
                 "The following tool was just executed. Review the result:\n"
                 "After executing a tool, the results of its use must be checked through some other tools (e.g., read_file).\n"
                 "- SUCCESS and correct → provide your final answer to the user.\n"
                 "- FAILURE or incorrect → analyze and retry the tool with corrected parameters.\n"
                 "- Need another tool → call it now.\n"
                 "Tool result:\n";
    } else {
        header = "\n\n# User Content\r\n";
    }

    size_t needed = strlen(content) + strlen(header) + 1;
    if (off + needed > DUCKY_CLAW_CONTEXT_BUF_SIZE) {
        PR_ERR("Prompt buffer overflow");
        return;
    }
    off += snprintf(s_total_prompt + off, DUCKY_CLAW_CONTEXT_BUF_SIZE - off,
                    "%s%s", header, content);

    // PR_NOTICE("Sending prompt (len=%u, is_tool=%d): %s ...", (unsigned)off, is_tool, s_total_prompt);
    ai_agent_send_text(s_total_prompt);
}

/**
 * agent_loop_task - Outer loop: wait for user message, run inner tool loop.
 *
 * Mirrors mimiclaw's agent_loop_task structure:
 *   outer: pop inbound message
 *   inner: send → wait → check tool_called → repeat or finish
 */
static void agent_loop_task(void *arg)
{
    PR_DEBUG("Agent loop task started");

    while (1) {
        tal_system_sleep(50);
        // PR_INFO("Device Free heap %d", tal_system_get_free_heap_size());
        sys_msg_t in = {0};
        if (sys_bus_pop_inbound(&in, UINT32_MAX) != OPRT_OK) {
            continue;
        }
        if (!in.content || !s_total_prompt) {
            claw_free(in.content);
            continue;
        }

        app_im_set_target(in.channel, in.chat_id);

        PR_INFO("===== AGENT TURN START channel=%s =====", in.channel);
        PR_DEBUG("user input: %.80s", in.content);

        /* Record user message in history */
        build_current_context("user", in.content);

        /* ---- Inner tool loop (mirrors mimiclaw while iteration < MAX) ---- */
        int  iteration  = 0;
        char *cur_content = in.content;

        s_in_tool_loop = false;

        /* Clear last-response buffer so a stale value from the previous turn
         * is never forwarded if EVT_END fires before any new text arrives. */
        if (s_last_response && s_last_response_lock) {
            tal_mutex_lock(s_last_response_lock);
            s_last_response[0] = '\0';
            tal_mutex_unlock(s_last_response_lock);
        }

        /* Drain any stale semaphore counts left over from the previous turn. */
        while (tal_semaphore_wait(s_turn.sem, 0) == OPRT_OK) {
            PR_WARN("Drained stale semaphore count before new turn");
        }

        while (iteration < TOOL_LOOP_MAX) {
            PR_INFO("--- LLM call iteration=%d ---", iteration + 1);

            /* Notify the user via IM that the agent is still working.
             * Sent at the start of every iteration so the user sees progress
             * even when tool calls take a long time. */
            app_im_bot_send_message("DuckyClaw is working...");

            /* Reset per-round tool state */
            if (s_turn.lock) {
                tal_mutex_lock(s_turn.lock);
                s_turn.tool_called = false;
                s_turn.tool_call_ok = false;
                s_turn.tool_result[0] = '\0';
                tal_mutex_unlock(s_turn.lock);
            }

            /* Restore TTS and UI output for this iteration. */
            // ai_agent_set_tts_suppressed(FALSE);
            // ai_chat_ui_set_output_suppressed(FALSE);

            /* On the last allowed iteration, ask the AI to summarise instead of
             * calling more tools.  iteration is 0-based so the last slot is
             * TOOL_LOOP_MAX-1. */
            bool is_last = (iteration == TOOL_LOOP_MAX - 1);
            if (is_last) {
                PR_WARN("Reached max iterations (%d), requesting summary", TOOL_LOOP_MAX);
            }

            /* Send prompt to cloud AI */
            __build_and_send(cur_content, s_in_tool_loop, is_last);

            /* Block until AI_USER_EVT_END (agent_loop_notify_turn_done posts sem) */
            OPERATE_RET wait_rt = tal_semaphore_wait(s_turn.sem, TURN_WAIT_MS);
            if (wait_rt != OPRT_OK) {
                PR_WARN("Turn wait timeout/error (rt=%d), aborting loop", wait_rt);
                break;
            }

            /* Check whether a tool was called this round */
            bool called = false;
            bool call_ok = false;
            char result_copy[TOOL_RESULT_BUF_SIZE];
            if (s_turn.lock) {
                tal_mutex_lock(s_turn.lock);
                called = s_turn.tool_called;
                call_ok = s_turn.tool_call_ok;
                strncpy(result_copy, s_turn.tool_result, TOOL_RESULT_BUF_SIZE - 1);
                result_copy[TOOL_RESULT_BUF_SIZE - 1] = '\0';
                tal_mutex_unlock(s_turn.lock);
            }

            PR_INFO("LLM iteration=%d tool_called=%d tool_ok=%d is_last=%d",
                    iteration + 1, called, call_ok, is_last);

            if (!called || is_last || call_ok) {
                if (s_last_response && s_last_response_lock) {
                    tal_mutex_lock(s_last_response_lock);
                    if (s_last_response[0] != '\0') {
                        app_im_bot_send_message(s_last_response);
                    }
                    tal_mutex_unlock(s_last_response_lock);
                }
                break;
            }

            /* Tool was called: next iteration feeds the tool result back */
            cur_content    = result_copy;
            s_in_tool_loop = true;
            iteration++;
        }

        s_in_tool_loop = false;
        PR_INFO("===== AGENT TURN END iterations=%d =====", iteration + 1);

        claw_free(in.content);
    }
}

int agent_loop_start_cb(void *data)
{
    (void)data;
    PR_DEBUG("Agent loop started");

    #define GREETING_MESSAGE "Wake up, my friend!(This is a greeting message, reply within 10 characters.)"

    sys_msg_t in = {0};
    strncpy(in.channel, SYS_CHAN_SYSTEM, sizeof(in.channel) - 1);
    in.content = claw_malloc(strlen(GREETING_MESSAGE) + 1);
    if (!in.content) {
        return OPRT_MALLOC_FAILED;
    }
    strncpy(in.content, GREETING_MESSAGE, strlen(GREETING_MESSAGE) + 1);
    sys_bus_push_inbound(&in);

    return 0;
}

OPERATE_RET agent_loop_init_evt_cb(void *data)
{
    OPERATE_RET rt = OPRT_OK;

    if (s_agent_loop_thread) {
        return OPRT_OK;
    }

#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    s_total_prompt = tal_psram_malloc(DUCKY_CLAW_CONTEXT_BUF_SIZE);
#else
    s_total_prompt = tal_malloc(DUCKY_CLAW_CONTEXT_BUF_SIZE);
#endif
    if (!s_total_prompt) {
        return OPRT_MALLOC_FAILED;
    }
    memset(s_total_prompt, 0, DUCKY_CLAW_CONTEXT_BUF_SIZE);

    s_history_json = cJSON_CreateArray();
    if (!s_history_json) {
        return OPRT_MALLOC_FAILED;
    }

    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_history_mutex));
    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_turn.lock));

    /* Binary semaphore: starts at 0, inner loop waits, STREAM_STOP posts */
    TUYA_CALL_ERR_RETURN(tal_semaphore_create_init(&s_turn.sem, 0, 1));

#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    s_last_response = tal_psram_malloc(LAST_RESPONSE_MAX);
#else
    s_last_response = tal_malloc(LAST_RESPONSE_MAX);
#endif
    if (!s_last_response) {
        return OPRT_MALLOC_FAILED;
    }
    memset(s_last_response, 0, LAST_RESPONSE_MAX);
    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_last_response_lock));

    ai_mcp_server_set_tool_exec_hook(__on_tool_executed, NULL);

    PR_DEBUG("Agent loop initialized");

    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = DUCKY_CLAW_AGENT_STACK;
    thrd_param.priority   = THREAD_PRIO_1;
    thrd_param.thrdname   = "agent_loop";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    thrd_param.psram_mode = 1;
#endif
    TUYA_CALL_ERR_RETURN(tal_thread_create_and_start(&s_agent_loop_thread, NULL, NULL,
                                                      agent_loop_task, NULL, &thrd_param));

    PR_INFO("Device Free heap %d", tal_system_get_free_heap_size());
    // tal_event_subscribe(EVENT_AI_CLIENT_RUN, "agent_loop", agent_loop_start_cb, SUBSCRIBE_TYPE_NORMAL);

    return OPRT_OK;
}

/**
 * agent_loop_init - Initialise the agent loop and start its thread.
 */
OPERATE_RET agent_loop_init(void)
{
    PR_DEBUG("Agent loop init");
    return tal_event_subscribe(EVENT_MQTT_CONNECTED, "agent_loop_init", agent_loop_init_evt_cb, SUBSCRIBE_TYPE_EMERGENCY);
}
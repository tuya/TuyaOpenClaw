/**
 * @file agent_loop.h
 * @brief Agent loop public interface.
 * @version 0.4
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __AGENT_LOOP_H__
#define __AGENT_LOOP_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * agent_loop_init - Initialise and start the agent loop thread.
 */
OPERATE_RET agent_loop_init(void);

/**
 * build_current_context - Append a role/content pair to the conversation history.
 * @role:    Sender role ("user", "assistant", "tool", …).
 * @content: Message text.
 */
OPERATE_RET build_current_context(const char *role, const char *content);

/**
 * agent_loop_set_last_response - Store the completed AI text stream.
 *
 * Must be called from tuyaopen_claw_chat.c on AI_USER_EVT_TEXT_STREAM_STOP
 * before agent_loop_notify_turn_done().  The inner loop uses this text
 * to forward the final response to IM.
 */
void agent_loop_set_last_response(const char *text);

/**
 * agent_loop_notify_turn_done - Signal that the cloud AI has finished one round.
 *
 * Must be called from tuyaopen_claw_chat.c on AI_USER_EVT_TEXT_STREAM_STOP.
 * Posts the internal semaphore so the blocked inner loop can proceed.
 */
void agent_loop_notify_turn_done(void);

/**
 * agent_loop_in_tool_loop - Returns true while the inner tool loop is active.
 *
 * Used by tuyaopen_claw_chat.c to suppress intermediate IM flushes during
 * tool-use iterations.
 */
bool agent_loop_in_tool_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* __AGENT_LOOP_H__ */

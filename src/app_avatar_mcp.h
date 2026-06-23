#ifndef APP_AVATAR_MCP_H
#define APP_AVATAR_MCP_H

#include "tuya_cloud_types.h"

/**
 * @brief Register avatar-specific MCP tools for AI-driven control.
 *
 * Registers the following tools:
 *   - avatar_play_animation   : body animation by name
 *   - avatar_set_emotion      : facial emotion preset with intensity
 *   - avatar_set_blendshape   : fine-grained BlendShape weights (JSON)
 *   - avatar_composite_action : animation + emotion in one call
 *
 * Must be called after ai_mcp_init().
 *
 * @return OPRT_OK on success
 */
OPERATE_RET app_avatar_mcp_init(void);

#endif /* APP_AVATAR_MCP_H */

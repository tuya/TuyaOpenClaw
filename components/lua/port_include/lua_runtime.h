/**
 * @file lua_runtime.h
 * @brief Embedded Lua 5.5 runtime entry point for DuckyClaw.
 *
 * Exposes a single synchronous function that creates a fresh sandboxed
 * lua_State, executes an inline source string, captures print() output
 * (and any runtime error message) into a caller-supplied buffer, and
 * destroys the state.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __LUA_RUNTIME_H__
#define __LUA_RUNTIME_H__

#include "tuya_cloud_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run an inline Lua source snippet under a sandboxed runtime.
 *
 * The runtime opens only the safe subset of the standard library
 * (base / coroutine / table / string / math / utf8). It installs a
 * print() override that appends to @p out_buf and a debug hook that
 * aborts execution after @p timeout_ms wall-clock milliseconds.
 *
 * On success the buffer holds the captured print() output (or a
 * "completed with no output" notice if the script printed nothing).
 * On Lua error the buffer holds whatever was printed before the
 * failure plus an "ERROR: <msg>" trailer; the function returns
 * OPRT_COM_ERROR so the caller can distinguish.
 *
 * @param[in]  source        NUL-terminated Lua source. Must not be NULL.
 * @param[in]  timeout_ms    Wall-clock budget in milliseconds; 0 means
 *                           "use the compile-time default
 *                           (CONFIG_LUA_DEFAULT_TIMEOUT_MS)".
 * @param[out] out_buf       Pre-allocated buffer for captured output.
 * @param[in]  out_buf_size  Capacity of @p out_buf in bytes; must be > 0.
 *
 * @return OPRT_OK             on script success.
 * @return OPRT_COM_ERROR      on Lua compile/runtime error
 *                             (output buffer still populated).
 * @return OPRT_INVALID_PARM   on bad arguments.
 * @return OPRT_MALLOC_FAILED  if the lua_State could not be created.
 */
OPERATE_RET lua_runtime_run_string(const char *source,
                                   uint32_t    timeout_ms,
                                   char       *out_buf,
                                   size_t      out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_RUNTIME_H__ */

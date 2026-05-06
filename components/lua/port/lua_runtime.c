/**
 * @file lua_runtime.c
 * @brief Embedded Lua 5.5 runtime for the DuckyClaw lua_run_script tool.
 *
 * Per-call lifecycle:
 *   1. lua_newstate with a claw_malloc-backed allocator (PSRAM-aware via
 *      claw_malloc when ENABLE_EXT_RAM is set).
 *   2. Open the sandboxed standard-library subset (port/linit_sandbox.c).
 *   3. Replace the global print() with a closure that appends to the
 *      caller's output buffer.
 *   4. Install a count-based debug hook (every 100 bytecode instructions)
 *      that aborts execution once the wall-clock deadline is exceeded.
 *   5. luaL_loadbufferx + lua_pcall the inline source.
 *   6. lua_close, return.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_runtime.h"
#include "lua_module_registry.h"

#include "tal_api.h"
#include "app_base_config.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Tunables (Kconfig-driven; keep defaults identical to Kconfig defaults)
 * --------------------------------------------------------------------------- */
#ifndef CONFIG_LUA_OUTPUT_BUFFER_SIZE
#define CONFIG_LUA_OUTPUT_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_LUA_DEFAULT_TIMEOUT_MS
#define CONFIG_LUA_DEFAULT_TIMEOUT_MS 3000
#endif

/* Hook invocation cadence: roughly every N bytecode instructions. */
#define LUA_RUNTIME_HOOK_COUNT 100

/* Global registry key under which we stash the per-call exec context so the
 * print closure and timeout hook can find it without polluting Lua globals. */
#define LUA_RUNTIME_CTX_KEY "__lua_runtime_ctx"

/* ---------------------------------------------------------------------------
 * Per-call execution context
 * --------------------------------------------------------------------------- */
typedef struct {
    char       *buf;          /* output buffer supplied by caller */
    size_t      size;         /* total capacity                  */
    size_t      len;          /* current fill                    */
    int         truncated;    /* set when output overflowed      */
    uint64_t    deadline_ms;  /* absolute ms; 0 → no deadline    */
} lua_exec_ctx_t;

/* ---------------------------------------------------------------------------
 * Output buffer helpers
 * --------------------------------------------------------------------------- */
static void __out_append(lua_exec_ctx_t *ctx, const char *text, size_t len)
{
    if (!ctx || !ctx->buf || ctx->size == 0 || !text || len == 0) {
        return;
    }
    if (ctx->len >= ctx->size - 1) {
        ctx->truncated = 1;
        return;
    }
    size_t room = ctx->size - 1 - ctx->len;
    size_t copy = (len < room) ? len : room;
    memcpy(ctx->buf + ctx->len, text, copy);
    ctx->len += copy;
    ctx->buf[ctx->len] = '\0';
    if (copy < len) {
        ctx->truncated = 1;
    }
}

/* ---------------------------------------------------------------------------
 * claw_malloc-backed Lua allocator
 *
 * Lua expects a single allocator that handles malloc / realloc / free, all
 * keyed by (ptr, osize, nsize). claw_malloc/claw_free routes to PSRAM when
 * ENABLE_EXT_RAM is set.
 *
 * realloc is implemented manually (alloc-copy-free) because TuyaOpen does
 * not expose a PSRAM realloc.
 * --------------------------------------------------------------------------- */
static void *__lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;

    if (nsize == 0) {
        if (ptr) {
            claw_free(ptr);
        }
        return NULL;
    }

    if (ptr == NULL) {
        return claw_malloc(nsize);
    }

    void *np = claw_malloc(nsize);
    if (!np) {
        return NULL;
    }
    size_t copy = (osize < nsize) ? osize : nsize;
    if (copy > 0) {
        memcpy(np, ptr, copy);
    }
    claw_free(ptr);
    return np;
}

/* ---------------------------------------------------------------------------
 * Helpers to stash/fetch the per-call ctx via the Lua registry
 * --------------------------------------------------------------------------- */
static void __ctx_install(lua_State *L, lua_exec_ctx_t *ctx)
{
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, LUA_RUNTIME_CTX_KEY);
}

static lua_exec_ctx_t *__ctx_fetch(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_RUNTIME_CTX_KEY);
    lua_exec_ctx_t *ctx = (lua_exec_ctx_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

/* ---------------------------------------------------------------------------
 * Lua-callable closures: print(), and the count-based timeout hook
 * --------------------------------------------------------------------------- */
static int __print_capture(lua_State *L)
{
    lua_exec_ctx_t *ctx = (lua_exec_ctx_t *)lua_touserdata(L, lua_upvalueindex(1));
    int top = lua_gettop(L);
    int i;

    for (i = 1; i <= top; i++) {
        size_t      len = 0;
        const char *text = luaL_tolstring(L, i, &len);
        if (i > 1) {
            __out_append(ctx, "\t", 1);
        }
        __out_append(ctx, text, len);
        lua_pop(L, 1); /* pop the tolstring result */
    }
    __out_append(ctx, "\n", 1);
    return 0;
}

static void __timeout_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;

    lua_exec_ctx_t *ctx = __ctx_fetch(L);
    if (!ctx || ctx->deadline_ms == 0) {
        return;
    }
    uint64_t now = (uint64_t)tal_system_get_millisecond();
    if (now > ctx->deadline_ms) {
        luaL_error(L, "execution timed out");
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
OPERATE_RET lua_runtime_run_string(const char *source,
                                   uint32_t    timeout_ms,
                                   char       *out_buf,
                                   size_t      out_buf_size)
{
    if (!source || !out_buf || out_buf_size == 0) {
        return OPRT_INVALID_PARM;
    }

    out_buf[0] = '\0';

    if (timeout_ms == 0) {
        timeout_ms = (uint32_t)CONFIG_LUA_DEFAULT_TIMEOUT_MS;
    }

    lua_exec_ctx_t ctx = {
        .buf         = out_buf,
        .size        = out_buf_size,
        .len         = 0,
        .truncated   = 0,
        .deadline_ms = (uint64_t)tal_system_get_millisecond() + (uint64_t)timeout_ms,
    };

    lua_State *L = lua_newstate(__lua_alloc, NULL, 0u);
    if (!L) {
        snprintf(out_buf, out_buf_size, "Error: failed to create Lua state");
        return OPRT_MALLOC_FAILED;
    }

    /* Sandboxed library set (port/linit_sandbox.c overrides upstream). */
    luaL_openlibs(L);

    /* Load registered hardware modules (gpio, i2c, uart, etc.). */
    lua_module_load_all(L);

    /* Register per-call ctx + override print + install hook. */
    __ctx_install(L, &ctx);

    lua_pushlightuserdata(L, &ctx);
    lua_pushcclosure(L, __print_capture, 1);
    lua_setglobal(L, "print");

    lua_sethook(L, __timeout_hook, LUA_MASKCOUNT, LUA_RUNTIME_HOOK_COUNT);

    /* "t" mode: text-only (reject precompiled bytecode → safer). */
    int status = luaL_loadbufferx(L, source, strlen(source),
                                  "=lua_run_script", "t");
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 0, 0);
    }

    if (status != LUA_OK) {
        /* Build a traceback so the caller sees line numbers and the call stack. */
        luaL_traceback(L, L, lua_tostring(L, -1), 1);
        const char *msg = lua_tostring(L, -1);
        if (!msg) {
            msg = "unknown Lua error";
        }
        __out_append(&ctx, "ERROR: ", 7);
        __out_append(&ctx, msg, strlen(msg));
        __out_append(&ctx, "\n", 1);
        lua_close(L);
        return OPRT_COM_ERROR;
    }

    if (ctx.len == 0) {
        const char done[] = "Lua script completed with no output.\n";
        __out_append(&ctx, done, sizeof(done) - 1);
    } else if (ctx.truncated) {
        const char trail[] = "[output truncated]\n";
        __out_append(&ctx, trail, sizeof(trail) - 1);
    }

    lua_close(L);
    return OPRT_OK;
}

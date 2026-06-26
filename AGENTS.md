# AGENTS.md — TuyaOpenClaw Project Guide

> This file provides project-level context for AI coding assistants and new contributors:
> business logic, technical architecture, directory layout, and coding guidelines.

---

## 1. Business Logic

TuyaOpenClaw is a **hardware-oriented AI agent running on edge devices**. Its core value is enabling users to interact with IoT devices through natural language via IM channels (Telegram / Discord / Feishu). The on-device agent receives instructions, invokes local MCP tools to perform real actions, and replies through the same channel.

**Typical user scenarios:**

| Scenario | Flow |
|----------|------|
| Set a reminder | User sends "remind me in 5 minutes" on Telegram → Agent calls `get_current_time` + `cron_add` → cron fires and pushes a message when due |
| Manage files | User sends "read /memory/MEMORY.md" → Agent calls `read_file` → returns file content |
| Remote execution | User sends "run `ls -la`" (Raspberry Pi) → Agent calls `tool_exec` → returns command output |
| Memory & diary | Agent autonomously writes important info to MEMORY.md or daily notes, preserving context across sessions |
| OpenClaw control | User sends claw-machine command → Agent sends control via ACP/WebSocket gateway |

**Key business characteristics:**

- **Unified multi-channel**: Telegram, Discord, Feishu, WebSocket, serial CLI all share a single agent loop
- **Tool loop**: A single user message can trigger up to 10 LLM ↔ tool iterations until the agent produces a final reply
- **Persistent memory**: Long-term memory (MEMORY.md), daily notes (YYYY-MM-DD.md), personality (SOUL.md), user profile (USER.md) persisted on flash/SD
- **Cross-platform deployment**: Same codebase runs on Tuya T5AI, ESP32-S3, Raspberry Pi, and Linux desktop via conditional compilation

---

## 2. Technical Architecture

### 2.1 Layered Overview

```
┌──────────────────────────────────────────────────────┐
│                     IM Channel Layer                  │
│  Telegram  │  Discord  │  Feishu  │  WebSocket │ CLI │
└─────────────────────┬────────────────────────────────┘
                      │ im_msg_t (inbound / outbound)
              ┌───────▼───────┐
              │  Message Bus  │  Thread-safe dual queue
              └───────┬───────┘
                      │
              ┌───────▼───────┐
              │  Agent Loop   │  Outer: wait for msg
              │               │  Inner: ≤10 tool iterations
              └───┬───────┬───┘
                  │       │
        ┌─────────▼─┐ ┌──▼──────────┐
        │ Context   │ │ Cloud AI    │
        │ Builder   │ │ (ai_agent)  │
        │ sys prompt│ │ stream cb   │
        └─────┬─────┘ └──────┬──────┘
              │              │
    ┌─────────▼──────────────▼──────────┐
    │           MCP Tools Layer          │
    │ files │ cron │ exec │ openclaw    │
    └─────────┬──────────────┬──────────┘
              │              │
    ┌─────────▼─────┐  ┌────▼─────────┐
    │   Memory /    │  │   Gateway    │
    │   Session     │  │  WS + ACP    │
    └───────────────┘  └──────────────┘
```

### 2.2 Core Data Flow

1. **Inbound**: IM channel (or WS/cron/ACP) builds an `im_msg_t` → `message_bus_push_inbound()`
2. **Agent consumes**: `agent_loop_task` blocks on `message_bus_pop_inbound()`
3. **Inner loop**: `context_build_system_prompt()` assembles system prompt + history → `ai_agent_send_text()` sends to cloud
4. **AI callback**: `tuyaopen_claw_chat.c`'s `__ai_chat_handle_event` receives stream events:
   - `STREAM_START/DATA/STOP`: accumulate text → record in history
   - `END`: calls `agent_loop_set_last_response()` + `agent_loop_notify_turn_done()` (posts semaphore)
5. **Tool execution**: Cloud AI triggers MCP tool call → `__on_tool_executed` hook records result → sets `s_turn.tool_called = true`
6. **Loop decision**: After semaphore post, agent checks `tool_called` — if true, feeds tool result as next input; otherwise forwards final reply to outbound queue
7. **Outbound**: `outbound_dispatch_task` dequeues message → dispatches to the corresponding IM SDK by `channel` field

### 2.3 Thread Model

| Thread | Responsibility | Entry |
|--------|---------------|-------|
| `tuya_app_main` / `main` | SDK init + `tuya_iot_yield` main loop | `user_main()` |
| `agent_loop` | Outer loop + inner tool iteration | `agent_loop_task()` |
| `outbound_loop` | Outbound message dispatch | `outbound_dispatch_task()` |
| `ws_server` | WebSocket server | `ws_server.c` |
| `acp_client` | OpenClaw gateway WS client | `acp_client.c` |
| `cron_service` | Scheduled task scheduler | `cron_service.c` |
| IM channel threads | Telegram polling / Discord gateway / Feishu WS | `*_bot.c` |

### 2.4 Synchronization

- **Agent loop ↔ AI callback**: Binary semaphore `s_turn.sem` (`agent_loop_task` waits, `tuyaopen_claw_chat` posts)
- **Shared history**: `s_history_mutex` protects `s_history_json` (cJSON array, sliding window ≤ 10 entries)
- **Tool state**: `s_turn.lock` protects `tool_called` / `tool_result`
- **Message bus**: `tal_queue` is inherently thread-safe

---

## 3. Directory Layout

```
TuyaOpenClaw/   (git repo: tuya/TuyaOpenClaw, a.k.a. Ducky)
├── agent/                  # Core agent loop and context builder
│   ├── agent_loop.c/h      #   Outer + inner tool-iteration loop (semaphore sync)
│   └── context_builder.c/h #   System prompt assembly (rules, memory, skills, personality)
│
├── IM/                     # Unified instant-messaging abstraction layer
│   ├── bus/message_bus.c/h #   Thread-safe inbound/outbound dual queue
│   ├── channels/           #   Telegram / Discord / Feishu bot implementations
│   ├── cli/serial_cli.c/h  #   Local serial/CLI input channel
│   ├── proxy/http_proxy.c/h#   TLS HTTP client (proxy mode)
│   ├── certs/              #   TLS CA certificate bundles
│   ├── im_api.h            #   Unified IM interface declarations
│   ├── im_config.h         #   IM constants and config
│   └── im_utils.c/h        #   IM utility functions
│
├── tools/                  # MCP tool registration and implementations
│   ├── tools_register.c/h  #   Unified tool registration entry point
│   ├── tool_files.c/h      #   File operations (read/write/edit/list/find)
│   ├── tool_cron.c/h       #   Scheduled tasks (cron_add/list/remove)
│   ├── tool_exec.c/h       #   Remote command execution (Linux only)
│   └── tool_openclaw_ctrl.c/h # OpenClaw gateway control
│
├── memory/                 # Persistent memory and session management
│   ├── memory_manager.c/h  #   MEMORY.md, daily notes, SOUL.md, USER.md
│   └── session_manager.c/h #   Session JSONL persistence
│
├── gateway/                # Gateway connections
│   ├── ws_server.c/h       #   Device-side WebSocket server
│   └── acp_client.c/h      #   OpenClaw ACP WebSocket client
│
├── cron_service/           # Background scheduled-task service
│   └── cron_service.c/h    #   In-memory job table + cron.json persistence + scheduler thread
│
├── heartbeat/              # Heartbeat service
│   └── heartbeat.c/h       #   Periodically reads HEARTBEAT.md to drive AI
│
├── skills/                 # Skill loader
│   └── skill_loader.c/h    #   Scans/installs built-in skill .md files
│
├── src/                    # Application glue layer
│   ├── tuya_app_main.c     #   Entry point user_main(), initialization orchestration
│   ├── tuyaopen_claw_chat.c   #   AI stream event handling, semaphore bridge to agent_loop
│   ├── app_im.c            #   IM init, outbound dispatch, channel switching
│   ├── cli_cmd.c           #   Extended CLI commands
│   └── reset_netcfg.c      #   Network config reset logic
│
├── include/                # Global headers
│   ├── tuya_app_config.h   #   Product ID, channel tokens, gateway config defaults
│   ├── tuya_app_config_secrets.h(.example) # Sensitive credentials (gitignored)
│   ├── app_im.h            #   IM application interface
│   └── tuyaopen_claw_chat.h   #   Chat module interface
│
├── ai_components/          # TuyaOpen AI component adapters (sub-CMake)
├── config/                 # Board-level Kconfig snapshots
│   ├── RaspberryPi.config
│   ├── TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config
│   └── ESP32S3_BREAD_COMPACT_WIFI.config
│
├── CMakeLists.txt          # Application-level CMake build script
├── Kconfig                 # Aggregates sub-module Kconfigs
├── CLAUDE.md               # AI coding assistant guide (Claude Code)
├── AGENTS.md               # This file
└── TuyaOpen/               # SDK submodule (git submodule)
```

---

## 4. Startup Sequence

```
user_main()
  ├── cJSON_InitHooks()           // PSRAM or standard heap
  ├── tal_log_init / tal_kv_init / tal_cli_init ...
  ├── tuya_iot_init()             // Tuya IoT client
  ├── netmgr_init()               // Network manager
  ├── board_register_hardware()
  ├── tuyaopen_claw_chat_init()      // Register AI stream event callback
  ├── app_im_init()               // Subscribe to MQTT connected event (deferred IM init)
  ├── ws_server_start()           // Start WebSocket server
  ├── tool_registry_init()        // Subscribe to MQTT connected → one-shot MCP tool chain init
  ├── acp_client_init()           // Start ACP WebSocket client
  ├── agent_loop_init()           // Create semaphore/mutex → start agent_loop thread
  └── tuya_iot_start() → for(;;) tuya_iot_yield()
```

After MQTT connects:
- `app_im_init_evt_cb` → init message_bus → start IM bot → start outbound dispatcher thread
- `__ai_mcp_init` → init filesystem, cron, heartbeat, memory, session, skills → register all MCP tools

---

## 5. Five Key Coding Guidelines

### Guideline 1: Use TuyaOpen Abstraction Layer — Never Call POSIX/libc Directly

All threading, synchronization, timers, and memory allocation must go through `tal_*` APIs (`tal_thread_*`, `tal_mutex_*`, `tal_semaphore_*`, `tal_queue_*`, `tal_malloc`/`tal_free`). Never use `pthread_*`, `malloc`/`free`, `sem_*`, etc. When external PSRAM is enabled (`ENABLE_EXT_RAM`), use the `claw_malloc`/`claw_free` wrappers that automatically switch between heap and PSRAM.

```c
// Correct
claw_malloc(size);
tal_mutex_create_init(&lock);
tal_thread_create_and_start(&handle, NULL, NULL, task_fn, NULL, &cfg);

// Wrong
malloc(size);
pthread_mutex_init(&lock, NULL);
pthread_create(&tid, NULL, task_fn, NULL);
```

### Guideline 2: Isolate Platform Differences via Conditional Compilation

Use the unified guard pattern `#if defined(X) && (X == 1)` for all platform-specific code (WiFi, display, camera, audio, PSRAM, Linux-only execution). When adding a new capability, follow the same pattern and declare a corresponding `config` entry in Kconfig.

```c
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    // WiFi-specific code
#endif

#if defined(PLATFORM_LINUX) && (PLATFORM_LINUX == 1)
    // Linux-only code (e.g. tool_exec)
#endif
```

### Guideline 3: All Inter-Module Communication Goes Through Message Bus

Any new inbound message source (new IM channel, sensor event, etc.) must build an `im_msg_t` and call `message_bus_push_inbound()`. The `agent_loop_task` is the sole consumer. No channel may call the AI interface or bypass the bus directly. Outbound messages likewise go through `message_bus_push_outbound()` → `outbound_dispatch_task`.

```c
im_msg_t msg = {0};
strncpy(msg.channel, "my_channel", sizeof(msg.channel) - 1);
msg.content = claw_malloc(len + 1);
// fill content ...
message_bus_push_inbound(&msg);
```

### Guideline 4: Return `OPERATE_RET`, Use `TUYA_CALL_ERR_*` Macros for Uniform Error Handling

All non-callback functions must return `OPERATE_RET` (`OPRT_OK` = 0 on success). Use `TUYA_CALL_ERR_RETURN` for critical steps (fail → return immediately) and `TUYA_CALL_ERR_LOG` for non-critical steps (log and continue). Always check for NULL after memory allocation.

```c
OPERATE_RET my_module_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    char *buf = claw_malloc(BUF_SIZE);
    if (!buf) {
        return OPRT_MALLOC_FAILED;
    }

    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_lock));  // fail → return
    TUYA_CALL_ERR_LOG(optional_feature_init());            // fail → log only

    return OPRT_OK;
}
```

### Guideline 5: Follow TuyaOpen C Code Style

This project follows the TuyaOpen C conventions. Below is a self-contained reference for contributors unfamiliar with the style.

#### 5.1 File Layout

**.h files** must follow this order:

| # | Section | Notes |
|---|---------|-------|
| 1 | Include guard | `#ifndef __MODULE_H__` / `#define` / `#endif` |
| 2 | C++ guard | `#ifdef __cplusplus` / `extern "C"` / `#endif` |
| 3 | Includes | Only what the header needs |
| 4 | Macros | `#define` constants |
| 5 | Type definitions | `typedef`, `struct`, `enum` |
| 6 | Function declarations | Public API |

**.c files** must follow this order:

| # | Section | Notes |
|---|---------|-------|
| 1 | Includes | Project → TuyaOpen SDK (`tal_*.h`) → System (`<string.h>`) |
| 2 | Macros | `#define` constants |
| 3 | Type definitions | `typedef`, `struct`, `enum` |
| 4 | File-scope variables | Including `STATIC` variables |
| 5 | Forward declarations | If any |
| 6 | Function implementations | No separator between functions |

**Section separators** — use only between different section types (e.g. includes → macros), never between items of the same type (e.g. function → function):

```c
/* ---------------------------------------------------------------------------
 * Section name
 * --------------------------------------------------------------------------- */
```

#### 5.2 File Header

Every `.h` and `.c` must start with a Doxygen file header in English:

```c
/**
 * @file module_name.c
 * @brief Brief description of the module
 * @version 0.1
 * @date 2025-03-25
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */
```

#### 5.3 Function Comments

Every function (both declaration in `.h` and implementation in `.c`) must have a Doxygen comment. Required tags: `@brief`, `@param` (per parameter, use `[in]`/`[out]`; omit if no params), `@return` (use `@return none` for void). Add `@note` for threading constraints or special usage.

```c
/**
 * @brief Create and start the cron service
 * @param[in] interval_ms  scheduler tick interval in milliseconds
 * @return OPRT_OK on success, error code on failure
 * @note Must be called after tool_files_fs_init(). Not reentrant.
 */
OPERATE_RET cron_service_start(uint32_t interval_ms);
```

#### 5.4 Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| File-scope variable | `s_` prefix | `s_ctx`, `s_history_json` |
| Global variable | `g_` prefix | `g_device_state` |
| Internal/static function | `__` prefix | `__on_tool_executed`, `__build_and_send` |
| Struct typedef | `_T` suffix | `MODULE_CTX_T` |
| Enum typedef | `_E` suffix | `IM_CHANNEL_E` |
| Macro / constant | `UPPER_SNAKE_CASE` | `TOOL_LOOP_MAX`, `STREAM_DATA_MAX_LEN` |
| Public function | `module_action` | `agent_loop_init`, `message_bus_push_inbound` |

#### 5.5 Types

Use standard C `<stdint.h>` / `<stdbool.h>` types. The TuyaOpen SDK defines legacy uppercase aliases (`UINT32_T`, `VOID_T`, etc.) — you will see them in older SDK headers and some existing code, but **new application code should prefer the standard names**:

| Use | Avoid in new code |
|-----|-------------------|
| `uint32_t`, `uint8_t`, `int32_t` | `UINT32_T`, `UINT8_T`, `INT32_T` |
| `void` | `VOID_T` |
| `bool` (`true`/`false`) | `BOOL_T` (`TRUE`/`FALSE`) |
| `static` | `STATIC` |
| `const` | `CONST` |
| `size_t` | (no alias) |

The following project-specific types remain in use and should **not** be replaced:
- `OPERATE_RET` — function return type for error codes (`OPRT_OK`, `OPRT_*`)
- `THREAD_HANDLE`, `MUTEX_HANDLE`, `SEM_HANDLE`, `QUEUE_HANDLE` — TuyaOpen OS abstraction handles
- `THREAD_CFG_T`, `TIMER_ID` — TuyaOpen OS config structs

> **Note**: Do not refactor existing files solely to convert legacy aliases. When modifying a function, you may update its signature to standard types if the change is localized.

#### 5.6 Braces and Formatting

- **Always use braces**, even for single-statement `if`/`else`/`while`/`for` blocks.
- Indent with **4 spaces** (no tabs).
- Space after keywords: `if (`, `while (`, `for (`.

```c
// Wrong
if (ptr == NULL)
    return OPRT_INVALID_PARM;

// Correct
if (ptr == NULL) {
    return OPRT_INVALID_PARM;
}
```

#### 5.7 Minimal Complete Example

```c
/**
 * @file my_module.c
 * @brief Example module following TuyaOpen C style
 * @version 0.1
 * @date 2025-03-25
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */
#include "my_module.h"

#include "tal_api.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define MY_BUF_SIZE  1024

/* ---------------------------------------------------------------------------
 * File-scope variables
 * --------------------------------------------------------------------------- */
static MUTEX_HANDLE s_lock = NULL;
static char        *s_buf  = NULL;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Internal helper to reset buffer
 * @return none
 */
static void __reset_buf(void)
{
    if (s_buf) {
        memset(s_buf, 0, MY_BUF_SIZE);
    }
}

/**
 * @brief Initialize my module
 * @return OPRT_OK on success, error code on failure
 */
OPERATE_RET my_module_init(void)
{
    if (s_lock) {
        return OPRT_OK;
    }

    s_buf = claw_malloc(MY_BUF_SIZE);
    if (!s_buf) {
        return OPRT_MALLOC_FAILED;
    }

    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_lock));

    __reset_buf();
    return OPRT_OK;
}
```

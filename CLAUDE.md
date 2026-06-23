# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DuckyClaw is a Linux-oriented AI agent built on the TuyaOpen C SDK (PLATFORM_LINUX). It runs a Claw-style agent loop on Ubuntu, Raspberry Pi, and other Linux boards, communicates via IM channels, and executes MCP-style tools on the device.

## Build Commands

DuckyClaw builds as a TuyaOpen application. From the repo root:

```bash
# Initialize TuyaOpen environment (creates .venv, exports OPEN_SDK_ROOT)
cd TuyaOpen && . ./export.sh && cd ..

# Select board config (copies to app_default.config)
# Available configs are in config/ directory
cp config/Ubuntu.config app_default.config           # Linux x64 (default)
cp config/RaspberryPi.config app_default.config      # Raspberry Pi
cp config/DshanPi_A1.config app_default.config       # DshanPi A1 (ARM Linux)

# Build (from TuyaOpen directory, pointing to DuckyClaw as the app)
cd TuyaOpen
python3 tos.py build

# Output goes to dist/ directory
# For LINUX target: produces native ELF binary
```

To skip interactive platform prompts during builds:
```bash
mkdir -p .cache && touch .cache/.dont_prompt_update_platform
```

## Secrets Configuration

Copy `include/tuya_app_config_secrets.h.example` to `include/tuya_app_config_secrets.h` and fill in:
- `TUYA_PRODUCT_ID`, `TUYA_OPENSDK_UUID`, `TUYA_OPENSDK_AUTHKEY` (Tuya cloud credentials)
- `IM_SECRET_CHANNEL_MODE` and corresponding channel tokens (Feishu/Telegram/Discord)
- `CLAW_WS_AUTH_TOKEN`, `OPENCLAW_GATEWAY_*` (gateway config)

The secrets file is gitignored. Defaults live in `include/tuya_app_config.h`.

## Architecture

### Agent Loop (`agent/`)
The core is a synchronous outer+inner loop in `agent_loop.c`:
- **Outer loop**: blocks on `message_bus_pop_inbound()` waiting for user messages from any IM channel
- **Inner loop** (up to `TOOL_LOOP_MAX=10` iterations): sends prompt to cloud AI via `ai_agent_send_text()`, blocks on a semaphore until the AI turn completes, checks if a tool was called, and either loops (feeding tool result back) or forwards the final response to IM
- Synchronization: `ducky_claw_chat.c` receives streaming AI events and calls `agent_loop_notify_turn_done()` on `AI_USER_EVT_END`
- Tool results are captured by `__on_tool_executed` hook (registered via `ai_mcp_server_set_tool_exec_hook`)

### Context Builder (`agent/context_builder.c`)
Assembles the full system prompt each turn: static instructions, available tools list, personality (SOUL.md), user info (USER.md), long-term memory, recent daily notes, skills summary, and sliding-window conversation history (`s_history_json`, max 10 entries).

### IM Layer (`IM/`)
Unified messaging abstraction:
- `bus/message_bus.c` - thread-safe inbound/outbound message queue connecting IM channels to the agent loop
- `channels/` - Telegram, Discord, Feishu bot implementations (HTTP polling/webhook via proxy)
- `proxy/http_proxy.c` - TLS HTTP client for cloud IM API calls
- `cli/serial_cli.c` - local serial/CLI input channel

### MCP Tools (`tools/`)
Device-side tools registered via `tools_register.c`:
- `tool_cron.c` - scheduled tasks and reminders (cron_add, cron_list, cron_remove)
- `tool_files.c` - file operations (read_file, write_file, edit_file, list_dir, find_path)
- `tool_exec.c` - remote code execution (Raspberry Pi)
- `tool_openclaw_ctrl.c` - OpenClaw gateway control

### Memory (`memory/`)
- `memory_manager.c` - persistent memory on flash/SD: MEMORY.md (long-term), daily notes (YYYY-MM-DD.md), SOUL.md, USER.md
- `session_manager.c` - session lifecycle management

### Gateway (`gateway/`)
- `ws_server.c` - WebSocket server for local connections
- `acp_client.c` - ACP client connecting to OpenClaw gateway

### Skills (`skills/`)
Skills are `.md` files in the `skills/` directory. `skill_loader.c` scans and builds a summary for the system prompt. The agent reads full skill files via the `read_file` tool when needed.

### Cron Service (`cron_service/`)
Background scheduler that evaluates and fires cron jobs, separate from the MCP tool interface.

## Key Patterns

- **Platform abstraction**: Heavy use of `#if defined(ENABLE_X) && (ENABLE_X == 1)` for conditional compilation across platforms (WiFi, display, video, audio, external RAM)
- **Memory allocation**: Use `tal_malloc`/`tal_free` (or `tal_psram_malloc`/`tal_psram_free` when `ENABLE_EXT_RAM` is set) and `claw_malloc`/`claw_free` wrappers
- **Error handling**: Functions return `OPERATE_RET` (`OPRT_OK` = 0 on success); use `TUYA_CALL_ERR_RETURN` / `TUYA_CALL_ERR_LOG` macros
- **Threading**: TuyaOpen's `tal_thread_*`, `tal_mutex_*`, `tal_semaphore_*` APIs (not pthreads directly)
- **Configuration**: Kconfig system with `app_default.config`; board-specific configs in `config/`
- **TuyaOpen submodule**: `TuyaOpen/` is a git submodule containing the SDK, platform support, and ai_components

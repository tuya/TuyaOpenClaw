# TuyaOpenClaw Lua 模块移植实现文档

本文档记录 TuyaOpenClaw（基于 TuyaOpen C SDK 的硬件 AI Agent）中 Lua 5.5 沙盒运行时及硬件模块的实现方案,目的是让其它开发者能够：

1. 理解 TuyaOpenClaw 中 Lua 子系统的整体架构;
2. 复用现有沙盒/注册器模式给设备增加新的硬件模块（PWM、I2C、UART、ADC 等）;
3. 在其它 TuyaOpen 板型上重新启用本子系统。

## 0. 背景

[TuyaOpenClaw](https://github.com/tuya/TuyaOpenClaw)（[官网](https://tuyaopen.ai/zh/duckyclaw)）是 [TuyaOpen](https://tuyaopen.ai) 上的开源硬件 AI Agent,主战平台是 Tuya T5AI 开发板（也支持 ESP32、Linux、Raspberry Pi）。其核心是一个端侧 Agent 循环,通过 IM 通道（Telegram/Discord/Feishu）与用户交互,并通过 MCP 风格的工具协议在设备上执行操作。

为了让云端 LLM 能够"动态写一段小代码,在设备上运行后拿结果",TuyaOpenClaw 引入了 [esp-claw](https://esp-claw.com/zh-cn/)（参考实现）的 Lua 子系统设计：把 Lua 5.5 解释器嵌入设备,作为一个名为 `lua_run_script` 的 MCP 工具暴露给 LLM。沙盒里只开放安全的标准库子集 + 设备硬件模块（GPIO、延时等），让 LLM 既能做计算/字符串处理,也能直接控 IO、调时序。

与 esp-claw 不同的是,TuyaOpenClaw 的硬件 API 走 TuyaOpen 的 `tkl_*` / `tal_*` 抽象层,因此模块代码全部按 TuyaOpen 接口重写;架构（沙盒 + 模块注册器 + 三层目录）则与 esp-claw 一致。

## 1. 整体架构

```
                                 ┌────────────────────────────────────────┐
                                 │              Cloud LLM                 │
                                 │  (sees lua_run_script + skill summary) │
                                 └─────────────────┬──────────────────────┘
                                                   │  MCP tool call
                                                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│  tools/tool_lua.c       ──── lua_run_script MCP wrapper                  │
│       │                       (capture buf, timeout_ms, return string)   │
│       ▼                                                                  │
│  components/lua/port/lua_runtime.c                                       │
│       │  - claw_malloc-backed allocator (PSRAM-aware)                    │
│       │  - linit_sandbox.c ──→ luaL_openselectedlibs(safe subset + os)   │
│       │  - lua_module_load_all() ──→ register hardware modules           │
│       │  - print() override + count-based timeout hook + traceback       │
│       ▼                                                                  │
│  components/lua/modules/<name>/lua_module_<name>.c                       │
│       │  - luaopen_<name>(L)  → table of C functions                     │
│       │  - lua_module_<name>_register()  → 自注册到 registry             │
│       ▼                                                                  │
│  TuyaOpen tkl_* / tal_* APIs  (gpio, system, time, ...)                  │
└──────────────────────────────────────────────────────────────────────────┘
```

**三层职责**：

| 层 | 文件 | 职责 |
|----|------|------|
| MCP 工具层 | `tools/tool_lua.c` | 把 Lua 沙盒包装为一个 MCP 工具 `lua_run_script`,暴露给 LLM |
| 沙盒运行时 | `components/lua/port/` | 维护沙盒规则、模块加载、超时/traceback 等 |
| 硬件模块 | `components/lua/modules/<name>/` | 把 TuyaOpen 硬件 API 翻译成 Lua C 函数 |

## 2. 目录结构

```
TuyaOpenClaw/
├── tools/
│   └── tool_lua.{c,h}                # MCP 工具：lua_run_script
├── components/lua/
│   ├── CMakeLists.txt                # 条件编译入口
│   ├── Kconfig                       # 子模块开关
│   ├── lua/                          # Lua 5.5 上游源码（精简 — 见 §3.1）
│   ├── include/
│   │   └── luaconf.h                 # 上游默认配置
│   ├── port/                         # TuyaOpenClaw 集成层
│   │   ├── linit_sandbox.c           # 替换上游 linit.c：仅打开安全库
│   │   ├── lua_module_os_safe.c      # 安全 os 子集 (time/date)
│   │   ├── lua_module_registry.c     # 模块注册器
│   │   └── lua_runtime.c             # 沙盒生命周期管理（每次新建 lua_State）
│   ├── port_include/
│   │   ├── lua_module_registry.h
│   │   └── lua_runtime.h
│   └── modules/                      # 硬件模块
│       ├── gpio/
│       │   ├── lua_module_gpio.{h,c}
│       └── delay/
│           └── lua_module_delay.{h,c}
├── skills/
│   ├── skill_loader.{c,h}            # 把 docs/skills/*.md 摘要进 system prompt
└── docs/skills/                      # 面向 LLM 的 skill 文档（lua_run/lua_gpio/lua_delay）
```

**`components/lua/lua/`** 内容（vendored Lua 5.5）：

保留：`lapi.c` `lauxlib.c` `lbaselib.c` `lcode.c` `lcorolib.c` `lctype.c` `ldblib.c` `ldebug.c` `ldo.c` `ldump.c` `lfunc.c` `lgc.c` `llex.c` `lmathlib.c` `lmem.c` `lobject.c` `lopcodes.c` `lparser.c` `lstate.c` `lstring.c` `lstrlib.c` `ltable.c` `ltablib.c` `ltm.c` `lundump.c` `lutf8lib.c` `lvm.c` `lzio.c`

**故意不编入**：`liolib.c`（filesystem）、`loslib.c`（process/shell）、`loadlib.c`（dynamic linking）、`linit.c`（被 `port/linit_sandbox.c` 替换）、`lua.c`（独立解释器入口）、`onelua.c`（单文件构建）、`ltests.c`（测试套件）。

## 3. 关键实现

### 3.1 沙盒库子集（`port/linit_sandbox.c`）

替换上游 `linit.c`,只打开安全的标准库,并把自定义的 `os` 子集挂到 `os` 全局名下。Lua 自带的 `luaL_openlibs(L)` 宏展开成 `luaL_openselectedlibs(L, ~0, 0)`,因此只需重新定义 `luaL_openselectedlibs`：

```c
/* port/linit_sandbox.c —— 关键片段 */
static const luaL_Reg lua_safe_libs[] = {
    {LUA_GNAME,       luaopen_base},        /* print/pcall/pairs/... */
    {LUA_COLIBNAME,   luaopen_coroutine},
    {LUA_TABLIBNAME,  luaopen_table},
    {LUA_STRLIBNAME,  luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {NULL,            NULL}
};

LUALIB_API void luaL_openselectedlibs(lua_State *L, int load, int preload)
{
    (void)load; (void)preload;
    for (const luaL_Reg *lib = lua_safe_libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    luaL_requiref(L, "os", luaopen_os_safe, 1);  /* 安全 os 子集 */
    lua_pop(L, 1);
}
```

**沙盒里能用 / 不能用**：

| 类别 | 可用 | 不可用 |
|------|------|--------|
| 标准库 | base / string / table / math / utf8 / coroutine | io / package / require / debug / dofile / loadfile |
| os 子集 | os.time / os.date | os.execute / os.remove / os.rename / os.exit / os.getenv |
| 字节码 | 仅文本源（"t" 模式） | 预编译字节码被拒绝 |

### 3.2 安全 `os` 子集（`port/lua_module_os_safe.c`）

只暴露 `os.time` 和 `os.date`,底层走 TuyaOpen 时间 API：

```c
/* os.time(): 当前 Unix 时间戳;若传 table 则用 mktime 转换 */
static int lua_os_time(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, (lua_Integer)tal_time_get_posix());
        return 1;
    }
    /* 处理 {year, month, day, hour, min, sec, isdst} → mktime */
    ...
}

/* os.date(fmt, t): strftime + "*t" 输出 table */
static int lua_os_date(lua_State *L) { ... }

int luaopen_os_safe(lua_State *L) {
    static const luaL_Reg os_funcs[] = {
        {"time", lua_os_time},
        {"date", lua_os_date},
        {NULL,   NULL}
    };
    lua_newtable(L);
    luaL_setfuncs(L, os_funcs, 0);
    return 1;
}
```

### 3.3 模块注册器（`port/lua_module_registry.c`）

提供一组对硬件模块来说"自注册"的 API。模块在 app 启动时调用 `lua_module_register("gpio", luaopen_gpio)`,运行时每次新建 `lua_State` 会自动 `luaL_requiref` 全部已注册模块。

```c
/* port_include/lua_module_registry.h —— 接口 */
void lua_module_register(const char *name, lua_CFunction open_fn);
void lua_module_load_all(lua_State *L);

/* port/lua_module_registry.c —— 实现：静态数组 + 线性查找 */
typedef struct { const char *name; lua_CFunction open_fn; } lua_module_entry_t;
static lua_module_entry_t s_registry[LUA_MODULE_REGISTRY_MAX];  /* 默认 16 */
static size_t s_count = 0;

void lua_module_register(const char *name, lua_CFunction open_fn) {
    if (!name || !name[0] || !open_fn) return;
    for (size_t i = 0; i < s_count; i++)
        if (strcmp(s_registry[i].name, name) == 0) return;  /* idempotent */
    if (s_count < LUA_MODULE_REGISTRY_MAX)
        s_registry[s_count++] = (lua_module_entry_t){name, open_fn};
}

void lua_module_load_all(lua_State *L) {
    for (size_t i = 0; i < s_count; i++) {
        luaL_requiref(L, s_registry[i].name, s_registry[i].open_fn, 1);
        lua_pop(L, 1);
    }
}
```

特点：
- 静态数组,**不上锁、不分配堆**——注册只发生在 app 启动单线程阶段;
- 重名直接忽略（idempotent),便于多次 init 路径合并;
- 上限 `LUA_MODULE_REGISTRY_MAX = 16`,需要更多模块时编译期 `-D` 调高即可。

### 3.4 沙盒运行时（`port/lua_runtime.c`）

每次 `lua_run_script` 调用都走完整生命周期:

1. `lua_newstate` 用 `claw_malloc` 后备分配器（`ENABLE_EXT_RAM` 时走 PSRAM）;
2. `luaL_openlibs` 调入沙盒库（被 `linit_sandbox.c` 截胡）;
3. `lua_module_load_all` 把所有已注册硬件模块加进去;
4. 注册 per-call 上下文（输出缓冲 + 超时截止时间）到 Lua registry;
5. 把全局 `print` 替换为捕获闭包,写到调用方提供的输出缓冲;
6. `lua_sethook(LUA_MASKCOUNT, 100)` 安装计数 hook,每 ~100 条字节码检查一次墙钟超时;
7. `luaL_loadbufferx(..., "t")` 强制纯文本模式(拒绝预编译字节码);
8. `lua_pcall` 执行;若失败,用 `luaL_traceback` 拼出栈追踪后写入缓冲;
9. `lua_close`,返回 `OPRT_OK` / `OPRT_COM_ERROR`。

```c
/* port/lua_runtime.c —— 关键片段 */
OPERATE_RET lua_runtime_run_string(const char *source, uint32_t timeout_ms,
                                   char *out_buf, size_t out_buf_size)
{
    if (timeout_ms == 0) timeout_ms = CONFIG_LUA_DEFAULT_TIMEOUT_MS;

    lua_exec_ctx_t ctx = {
        .buf = out_buf, .size = out_buf_size, .len = 0, .truncated = 0,
        .deadline_ms = (uint64_t)tal_system_get_millisecond() + timeout_ms,
    };

    lua_State *L = lua_newstate(__lua_alloc, NULL, 0u);
    luaL_openlibs(L);                /* 经 linit_sandbox.c 拦截 */
    lua_module_load_all(L);          /* gpio / delay / ... */

    __ctx_install(L, &ctx);          /* 存到 LUA_REGISTRYINDEX */
    lua_pushlightuserdata(L, &ctx);
    lua_pushcclosure(L, __print_capture, 1);
    lua_setglobal(L, "print");
    lua_sethook(L, __timeout_hook, LUA_MASKCOUNT, 100);

    int status = luaL_loadbufferx(L, source, strlen(source),
                                  "=lua_run_script", "t");
    if (status == LUA_OK) status = lua_pcall(L, 0, 0, 0);

    if (status != LUA_OK) {
        luaL_traceback(L, L, lua_tostring(L, -1), 1);
        __out_append(&ctx, "ERROR: ", 7);
        __out_append(&ctx, lua_tostring(L, -1), strlen(lua_tostring(L, -1)));
        __out_append(&ctx, "\n", 1);
    }
    /* ... 处理 "no output" / "[output truncated]" */
    lua_close(L);
    return status == LUA_OK ? OPRT_OK : OPRT_COM_ERROR;
}
```

**claw_malloc 后备分配器**：

```c
static void *__lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    if (nsize == 0)        { if (ptr) claw_free(ptr); return NULL; }
    if (ptr == NULL)       { return claw_malloc(nsize); }
    /* realloc：alloc-copy-free（TuyaOpen 没有 PSRAM realloc） */
    void *np = claw_malloc(nsize);
    if (!np) return NULL;
    memcpy(np, ptr, osize < nsize ? osize : nsize);
    claw_free(ptr);
    return np;
}
```

### 3.5 GPIO 模块（`modules/gpio/lua_module_gpio.c`）

把 TuyaOpen `tkl_gpio_*` API 包成 Lua `gpio` 表。T5AI 无 INOUT 方向,且类型名为 `TUYA_GPIO_DRCT_E`、`TUYA_GPIO_OPENDRAIN`（不同于 ESP-IDF 的 `DIRECTION` / `OPEN_DRAIN`）：

```c
/* modules/gpio/lua_module_gpio.c —— mode 字符串映射 */
static bool __parse_mode(const char *s,
                         TUYA_GPIO_DRCT_E *direct,
                         TUYA_GPIO_MODE_E *mode)
{
    if (!strcmp(s, "input"))           { *direct=TUYA_GPIO_INPUT;  *mode=TUYA_GPIO_PULLUP;     return true; }
    if (!strcmp(s, "output"))          { *direct=TUYA_GPIO_OUTPUT; *mode=TUYA_GPIO_PUSH_PULL;  return true; }
    if (!strcmp(s, "input_output"))    { *direct=TUYA_GPIO_OUTPUT; *mode=TUYA_GPIO_PUSH_PULL;  return true; }
    if (!strcmp(s, "output_od"))       { *direct=TUYA_GPIO_OUTPUT; *mode=TUYA_GPIO_OPENDRAIN;  return true; }
    if (!strcmp(s, "input_output_od")) { *direct=TUYA_GPIO_OUTPUT; *mode=TUYA_GPIO_OPENDRAIN;  return true; }
    if (!strcmp(s, "disable"))         { *direct=TUYA_GPIO_INPUT;  *mode=TUYA_GPIO_FLOATING;   return true; }
    return false;
}

static int lua_gpio_set_level(lua_State *L)
{
    int pin   = (int)luaL_checkinteger(L, 1);
    int level = (int)luaL_checkinteger(L, 2);
    if (pin < 0 || pin >= 56)
        return luaL_error(L, "gpio: pin %d out of range (0-55)", pin);

    /* TuyaOpen 要求每次 write 前都 init —— 见 T5AI 引脚映射文档 */
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = level ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW,
    };
    if (tkl_gpio_init((TUYA_GPIO_NUM_E)pin, &cfg) != OPRT_OK)
        return luaL_error(L, "gpio: init failed for P%d", pin);
    if (tkl_gpio_write((TUYA_GPIO_NUM_E)pin, cfg.level) != OPRT_OK)
        return luaL_error(L, "gpio: write failed for P%d", pin);
    return 0;
}

/* lua_gpio_get_level / lua_gpio_set_direction 同理 */

int luaopen_gpio(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_gpio_set_direction); lua_setfield(L, -2, "set_direction");
    lua_pushcfunction(L, lua_gpio_set_level);     lua_setfield(L, -2, "set_level");
    lua_pushcfunction(L, lua_gpio_get_level);     lua_setfield(L, -2, "get_level");
    return 1;
}

void lua_module_gpio_register(void) {
    lua_module_register("gpio", luaopen_gpio);
}
```

**T5AI 引脚号** 与 [T5AI 引脚映射](https://www.tuyaopen.ai/zh/docs/hardware-specific/tuya-t5/t5ai-peripheral-mapping)一致:`TUYA_GPIO_NUM_0..TUYA_GPIO_NUM_55`,即 P0–P55。GPIO 模块按 `0..55` 验证。

### 3.6 Delay 模块（`modules/delay/lua_module_delay.c`）

毫秒延时走 `tal_system_sleep`(让出任务给调度器);微秒延时走 T5AI 平台特有的 `tkl_system_sleep_us`(忙等),并对标 esp-claw 的 `esp_rom_delay_us` 把上限设在 1_000_000 us:

```c
#define LUA_MODULE_DELAY_US_MAX 1000000u

static int lua_delay_sleep_ms(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    tal_system_sleep((uint32_t)ms);
    return 0;
}

static int lua_delay_sleep_us(lua_State *L) {
    lua_Integer us = luaL_checkinteger(L, 1);
    if (us < 0) us = 0;
    if ((uint64_t)us > LUA_MODULE_DELAY_US_MAX)
        return luaL_error(L,
            "delay: delay_us supports 0..%u only, use delay_ms for longer waits",
            (unsigned)LUA_MODULE_DELAY_US_MAX);
    tkl_system_sleep_us((uint32_t)us);
    return 0;
}

int luaopen_delay(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_delay_sleep_ms); lua_setfield(L, -2, "delay_ms");
    lua_pushcfunction(L, lua_delay_sleep_us); lua_setfield(L, -2, "delay_us");
    return 1;
}

void lua_module_delay_register(void) {
    lua_module_register("delay", luaopen_delay);
}
```

> `tkl_system_sleep_us` 当前**仅 T5AI 平台实现**(`platform/T5AI/tuyaos/tuyaos_adapter/src/system/tkl_system.c`)。其它板要启用 `delay_us`,需自行实现该 tkl 函数。`delay_ms` 走 `tal_system_sleep` 是跨平台的。

### 3.7 MCP 工具暴露（`tools/tool_lua.c`）

把整个 Lua 沙盒包装成一个 `lua_run_script` MCP 工具:

```c
TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
    "lua_run_script",
    "Execute an inline Lua 5.5 script on the device and return whatever it prints.\n"
    "Sandbox:\n"
    "- Standard library subset: base, string, table, math, utf8, coroutine.\n"
    "- 'os' subset: only os.time / os.date.\n"
    "- 'io', 'package'/'require', 'debug' ... NOT available.\n"
    "Hardware modules (when compiled in):\n"
    "- gpio.set_direction(pin, mode), gpio.set_level(pin, 0|1), gpio.get_level(pin).\n"
    "- delay.delay_ms(ms), delay.delay_us(us). delay_us is capped at 1_000_000.\n"
    "...",
    __tool_lua_run, NULL,
    MCP_PROP_STR("code", "Lua 5.5 source code (text only)."),
    MCP_PROP_INT_DEF_RANGE("timeout_ms", "Wall-clock budget (ms).",
                           CONFIG_LUA_DEFAULT_TIMEOUT_MS, 100, 60000)
));
```

工具 callback 流程:

```c
static OPERATE_RET __tool_lua_run(const MCP_PROPERTY_LIST_T *p,
                                  MCP_RETURN_VALUE_T *ret_val, void *ud)
{
    const char *code = __get_str_prop(p, "code");
    int timeout_ms   = CONFIG_LUA_DEFAULT_TIMEOUT_MS;
    __get_int_prop(p, "timeout_ms", &timeout_ms);

    char *buf = claw_malloc(CONFIG_LUA_OUTPUT_BUFFER_SIZE);
    OPERATE_RET rt = lua_runtime_run_string(code, (uint32_t)timeout_ms,
                                            buf, CONFIG_LUA_OUTPUT_BUFFER_SIZE);
    ai_mcp_return_value_set_str(ret_val, buf);   /* 总是回填,失败也回 ERROR: ... */
    claw_free(buf);
    return rt;
}
```

## 4. 配置（Kconfig）

```kconfig
# components/lua/Kconfig
menuconfig ENABLE_LUA
    bool "Enable embedded Lua 5.5 interpreter"
    default n

if ENABLE_LUA

config LUA_OUTPUT_BUFFER_SIZE
    int "Lua print/output capture buffer (bytes)"
    default 4096
    range 1024 32768

config LUA_DEFAULT_TIMEOUT_MS
    int "Default Lua execution timeout (ms)"
    default 3000
    range 100 60000

config ENABLE_LUA_MODULE_GPIO
    bool "Enable lua_module_gpio (GPIO control from Lua)"
    depends on ENABLE_LUA
    default n

config ENABLE_LUA_MODULE_DELAY
    bool "Enable lua_module_delay (blocking delays from Lua)"
    depends on ENABLE_LUA
    default n

endif # ENABLE_LUA
```

另外有顶层开关 `ENABLE_LUA_TOOL`(在 `tools/Kconfig`),控制 `lua_run_script` MCP 工具是否注册——分开是为了允许"只编 Lua 沙盒、不暴露给 LLM"的边缘场景。

**`app_default.config` 推荐设置**(T5AI):

```ini
CONFIG_ENABLE_LUA=y
CONFIG_ENABLE_LUA_TOOL=y
CONFIG_ENABLE_LUA_MODULE_GPIO=y
CONFIG_ENABLE_LUA_MODULE_DELAY=y
```

**`tools/tools_register.c` 中的注册**:

```c
#if defined(ENABLE_LUA_TOOL) && (ENABLE_LUA_TOOL == 1)
#include "tool_lua.h"
#endif
#if defined(ENABLE_LUA_MODULE_GPIO) && (ENABLE_LUA_MODULE_GPIO == 1)
#include "lua_module_gpio.h"
#endif
#if defined(ENABLE_LUA_MODULE_DELAY) && (ENABLE_LUA_MODULE_DELAY == 1)
#include "lua_module_delay.h"
#endif

static OPERATE_RET __ai_mcp_init(void *data) {
    /* ... 其它工具 ... */

    #if defined(ENABLE_LUA_TOOL) && (ENABLE_LUA_TOOL == 1)
    TUYA_CALL_ERR_LOG(tool_lua_register());
    #endif

    /* 硬件模块向 registry 注册 —— 必须发生在第一次 lua_run_script 之前 */
    #if defined(ENABLE_LUA_MODULE_GPIO) && (ENABLE_LUA_MODULE_GPIO == 1)
    lua_module_gpio_register();
    #endif
    #if defined(ENABLE_LUA_MODULE_DELAY) && (ENABLE_LUA_MODULE_DELAY == 1)
    lua_module_delay_register();
    #endif

    return rt;
}
```

## 5. 让 LLM 知道沙盒能用哪些模块

LLM 看到的"Lua 能力"通过两个途径合成:

### 5.1 MCP 工具 description（强信号）

`AI_MCP_TOOL_ADD("lua_run_script", "...", ...)` 的描述串本身,LLM 在系统级工具列表里第一眼就看到。要在这里把可用的标准库子集、`os` 子集、可用硬件模块及其 API 罗列清楚(详见 `tools/tool_lua.c:121-180`)。

### 5.2 Skills 摘要（弱信号 + 完整文档）

TuyaOpenClaw 的 `skill_loader` 会扫描设备 SD 卡 `/sdcard/skills/*.md`,把每个 `.md` 的标题+第一段聚合到 system prompt,并提示 LLM "use read_file to load full instructions"。

为此项目维护三份配套的 skill 文档:

| skill 文件 | 作用 |
|------------|------|
| `docs/skills/lua_run.md` | 给 LLM 的"何时用 / 怎么用 lua_run_script" 指南 |
| `docs/skills/lua_gpio.md` | gpio 模块完整 API + 例子 |
| `docs/skills/lua_delay.md` | delay 模块完整 API + 边界 |

并把精简版同步进 `skills/skill_loader.c` 的 `BUILTIN_LUA_RUN`/`BUILTIN_LUA_GPIO`/`BUILTIN_LUA_DELAY` 内置串中,首次启动 `skill_loader_init()` 会自动写到 `/sdcard/skills/lua_*.md`(已存在则跳过)。

参见 [hardware-skill 文档](https://tuyaopen.ai/zh/docs/duckyclaw/hardware-skill)。

## 6. Lua 端 API 参考

调用方式：通过 `lua_run_script` MCP 工具,把脚本放在 `code` 字段。

### 6.1 标准库子集

| 模块 | 主要函数 |
|------|----------|
| base | `print` `tostring` `tonumber` `pairs` `ipairs` `select` `error` `pcall` `xpcall` `type` `assert` ... |
| `string` | `format` `gsub` `match` `find` `sub` `rep` `lower` `upper` `byte` `char` ... |
| `table` | `insert` `remove` `concat` `sort` `unpack` |
| `math` | `floor` `ceil` `sqrt` `pi` `random` `randomseed` ... |
| `utf8` | `len` `char` `codepoint` `offset` |
| `coroutine` | `create` `resume` `yield` `status` |
| `os`(子集) | `os.time([table])` `os.date([fmt[, t]])` |

### 6.2 `gpio` 模块

| 函数 | 入参 | 返回 | 备注 |
|------|------|------|------|
| `gpio.set_direction(pin, mode)` | `pin: 0..55`, `mode: string` | — | mode: `"input"` / `"output"` / `"input_output"` / `"output_od"` / `"input_output_od"` / `"disable"` |
| `gpio.set_level(pin, level)` | `pin: 0..55`, `level: 0\|1` | — | 内部以 OUTPUT/PUSH_PULL 重 init |
| `gpio.get_level(pin)` | `pin: 0..55` | `0\|1` | 内部以 INPUT/PULLUP 重 init |

错误以 Lua error 形式抛出,被工具捕获后回传 `ERROR: <msg>` + traceback。

### 6.3 `delay` 模块

| 函数 | 入参 | 返回 | 备注 |
|------|------|------|------|
| `delay.delay_ms(ms)` | `ms >= 0` | — | `tal_system_sleep`,让出任务 |
| `delay.delay_us(us)` | `0 <= us <= 1_000_000` | — | `tkl_system_sleep_us`,忙等;**仅 T5AI 实现** |

负数被截为 0;超过 us 上限抛 `delay: delay_us supports 0..1000000 only` 错。

## 7. 使用示例

> 下例都是云端 LLM 通过 MCP 工具调用 `lua_run_script` 的请求体,设备返回的字符串紧随其后。

### 7.1 纯计算

```json
{"code": "local r=0 for i=1,100 do r=r+i end print(r)"}
```
返回 `5050`。

### 7.2 字符串/UTF-8 处理

```json
{"code": "local s='Hello, 你好' print(utf8.len(s), #s)"}
```
返回 `9	14`。

### 7.3 取当前时间

```json
{"code": "print(os.date('%Y-%m-%d %H:%M:%S'))"}
```
返回类似 `2026-05-07 12:34:56`。

### 7.4 点亮 P10 上的 LED

```json
{"code": "gpio.set_direction(10,'output') gpio.set_level(10,1) print('LED on')"}
```
返回 `LED on`。

### 7.5 读 P12 上的按键(active-low)

```json
{"code": "gpio.set_direction(12,'input') print(gpio.get_level(12)==0 and 'pressed' or 'released')"}
```

### 7.6 闪烁 5 次（gpio + delay 组合）

```json
{
  "code": "gpio.set_direction(10,'output') for i=1,5 do gpio.set_level(10,1) delay.delay_ms(100) gpio.set_level(10,0) delay.delay_ms(100) end print('blinked')",
  "timeout_ms": 5000
}
```
返回 `blinked`。注意延时算进 `timeout_ms`,默认 3000 ms 不够这里需要的 ~1000 ms 余量,显式抬到 5000 ms。

### 7.7 HC-SR04 触发脉冲

```json
{"code": "gpio.set_direction(5,'output') gpio.set_level(5,1) delay.delay_us(10) gpio.set_level(5,0) print('triggered')"}
```

### 7.8 错误演示

```json
{"code": "gpio.set_level(99, 1)"}
```
返回:
```
ERROR: [string "lua_run_script"]:1: gpio: pin 99 out of range (0-55)
stack traceback:
	[C]: in function 'gpio.set_level'
	[string "lua_run_script"]:1: in main chunk
	[C]: in ?
```

## 8. 添加新硬件模块的步骤（以 `pwm` 为例）

下面以"加一个 `pwm` 模块"为例展示完整流程。同样的步骤适用于 i2c/uart/adc 等:

### 8.1 创建模块源码

`components/lua/modules/pwm/lua_module_pwm.h`:

```c
#ifndef __LUA_MODULE_PWM_H__
#define __LUA_MODULE_PWM_H__
#include "lua_module_registry.h"
#ifdef __cplusplus
extern "C" {
#endif
int  luaopen_pwm(lua_State *L);
void lua_module_pwm_register(void);
#ifdef __cplusplus
}
#endif
#endif
```

`components/lua/modules/pwm/lua_module_pwm.c`:

```c
#include "lua_module_pwm.h"
#include "tkl_pwm.h"
#include "lauxlib.h"

static int lua_pwm_start(lua_State *L) {
    int chan = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    int duty = (int)luaL_checkinteger(L, 3);
    /* ... 调 tkl_pwm_init / tkl_pwm_start ... */
    return 0;
}

int luaopen_pwm(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_pwm_start); lua_setfield(L, -2, "start");
    /* lua_pushcfunction(L, lua_pwm_stop); lua_setfield(L, -2, "stop"); */
    return 1;
}

void lua_module_pwm_register(void) {
    lua_module_register("pwm", luaopen_pwm);
}
```

### 8.2 `components/lua/CMakeLists.txt` 加条件源

```cmake
if (CONFIG_ENABLE_LUA_MODULE_PWM STREQUAL "y")
    list(APPEND LUA_PORT_SRCS
        ${MODULE_PATH}/modules/pwm/lua_module_pwm.c
    )
endif()

target_include_directories(${EXAMPLE_LIB} PRIVATE
    # ...
    ${MODULE_PATH}/modules/pwm
)
```

### 8.3 `components/lua/Kconfig` 加开关

```kconfig
config ENABLE_LUA_MODULE_PWM
    bool "Enable lua_module_pwm (PWM control from Lua)"
    depends on ENABLE_LUA
    default n
```

### 8.4 `tools/tools_register.c` 调注册

```c
#if defined(ENABLE_LUA_MODULE_PWM) && (ENABLE_LUA_MODULE_PWM == 1)
#include "lua_module_pwm.h"
#endif
/* ... __ai_mcp_init() 内 ... */
#if defined(ENABLE_LUA_MODULE_PWM) && (ENABLE_LUA_MODULE_PWM == 1)
lua_module_pwm_register();
#endif
```

### 8.5 `app_default.config` 启用

```ini
CONFIG_ENABLE_LUA_MODULE_PWM=y
```

### 8.6 编写 skill 文档（让 LLM 知道）

- 在 `docs/skills/lua_pwm.md` 写完整 API+例子(参考 lua_gpio/lua_delay 的风格);
- 在 `skills/skill_loader.c` 加 `BUILTIN_LUA_PWM` 精简版 + 注册到 `s_builtins[]`;
- 在 `tools/tool_lua.c` 的 `lua_run_script` description 中追加一行说明 `pwm.*` API 概要。

## 9. 平台兼容性

| 平台 | Lua 沙盒 | gpio 模块 | delay_ms | delay_us |
|------|----------|-----------|----------|----------|
| Tuya T5AI | ✅ 已验证 | ✅ | ✅ | ✅ |
| ESP32 / ESP32-S3 | 应可用(沙盒不依赖平台 API) | 需确认 `tkl_gpio_*` 实现 | ✅ `tal_system_sleep` 跨平台 | ⚠️ 需自实现 `tkl_system_sleep_us` |
| Linux / Raspberry Pi | ✅ 沙盒可编 | ⚠️ T5AI 引脚号语义不直接适用 | ✅ | ⚠️ 需自实现 |

**注意点**:

1. `tkl_system_sleep_us` 当前**仅 T5AI** 平台实现,见 `TuyaOpen/platform/T5AI/tuyaos/tuyaos_adapter/src/system/tkl_system.c`。其它平台启用 `lua_module_delay` 会链接报错——需要补 tkl 实现,或改为忙等 + `tal_system_get_millisecond` 计数(精度差)。
2. `gpio` 模块的引脚号语义(0..55)是 T5AI native 编号。其它板需根据 `tkl_gpio_*` 在该平台的端口数调整 `GPIO_PIN_MAX`。
3. `claw_malloc` 在 `ENABLE_EXT_RAM=y` 时走 PSRAM,Lua state 体积通常 ≥ 50KB,建议在内存紧张的板子上启用 PSRAM。

## 10. 参考资料

- TuyaOpenClaw 项目主页: <https://tuyaopen.ai/zh/duckyclaw>
- TuyaOpenClaw 源码: <https://github.com/tuya/TuyaOpenClaw>
- Claw 平台: <https://claw.tuyasmart.com/>
- TuyaOpenClaw 硬件 Skill 指南: <https://tuyaopen.ai/zh/docs/duckyclaw/hardware-skill>
- T5AI 引脚映射: <https://www.tuyaopen.ai/zh/docs/hardware-specific/tuya-t5/t5ai-peripheral-mapping>
- esp-claw 主页: <https://esp-claw.com/zh-cn/>
- esp-claw lua_modules 参考(本移植的设计原型): <https://esp-claw.com/zh-cn/reference-cap/lua-modules/>
- Lua 5.5 官方文档: <https://www.lua.org/manual/5.5/>

# Lua Run

让代理在设备本地用一段内联 Lua 5.5 脚本完成"难以用 MCP 工具组合表达"的小型计算 / 数据变换 / 逻辑判断，并把 `print()` 的输出回传给模型。

> 这份文件既是给 LLM 读的能力说明，也可以原样写到运行时 `skills/` 目录（`/spiffs/skills/lua_run.md` 或 `/sdcard/skills/lua_run.md`），由 `skill_loader` 扫描后纳入 system prompt 摘要。

## When to use

- 需要做基本运算、字符串/表处理、UTF-8 处理，写 Lua 比把现有 MCP 工具串起来更直接。
- 需要根据若干变量做条件判断后只回传一个最终值。
- 需要把传入的 JSON 字符串（已由其它工具读到）解析后取一个字段——纯文本切分场景。
- 需要在脚本里直接读写 GPIO / 做毫秒/微秒级延时——板子上启用了相应模块时可用，详见 `lua_gpio` / `lua_delay` skill。

**不要**用它来：
- 做文件 / 网络 / 摄像头 / 显示控制——沙盒里不暴露这些库（GPIO/延时除外，见上）。
- 长时间循环（>3 秒）。超时会被强制中断，结果以 `ERROR: execution timed out` 返回。
- 在两次调用之间共享状态。每次调用都是全新 `lua_State`，全局变量、`require` 的模块统统消失。

## How to use

调用工具：`lua_run_script`

参数：

| 字段 | 类型 | 是否必需 | 说明 |
|------|------|----------|------|
| `code` | string | 必填 | Lua 5.5 源代码文本（不是字节码）。 |
| `timeout_ms` | integer | 可选 | 墙钟超时，毫秒。默认 3000。 |

返回：单个字符串。
- 成功且有输出：原样返回 `print()` 写的内容（多个值用 `\t` 分隔，行尾追加 `\n`，与 Lua 标准 print 行为一致）。
- 成功且没 print：返回 `Lua script completed with no output.`。
- 出错：先返回出错前已 print 的内容，再追加一行 `ERROR: <message>`。
- 输出超出设备配置的缓冲（默认 4096 字节）会被截断，并附 `[output truncated]` 标记。

## Sandbox

仅开放 Lua 5.5 标准库的安全子集：

- `_G`（base：`print` / `tostring` / `tonumber` / `pairs` / `ipairs` / `select` / `error` / `pcall` / `xpcall` / `type` / `assert`/...）
- `coroutine`
- `table`
- `string`
- `math`
- `utf8`
- `os`（**只有 `os.time` 和 `os.date` 子集**，没有 `os.execute` / `os.remove` / `os.rename` / `os.exit` 等危险接口）

**硬件模块**（按构建配置可选，运行时已自动注入到全局表）：

- `gpio`（`CONFIG_ENABLE_LUA_MODULE_GPIO=y` 时可用）：`gpio.set_direction` / `gpio.set_level` / `gpio.get_level`，详见 `lua_gpio` skill。
- `delay`（`CONFIG_ENABLE_LUA_MODULE_DELAY=y` 时可用）：`delay.delay_ms` / `delay.delay_us`，详见 `lua_delay` skill。

**不可用**：`io` / `package` / `require` / `debug` / `dofile` / `loadfile`、`os` 中除 `time`/`date` 之外的全部接口，以及加载预编译字节码（运行时强制 `"t"` 文本模式）。直接调用会报 `attempt to call a nil value (global 'io')` 这类错。

## Examples

### 例 1：纯算术

调用：

```json
{
  "code": "local r = 0 for i=1,100 do r = r + i end print(r)"
}
```

返回：

```
5050
```

### 例 2：字符串/UTF-8 处理

调用：

```json
{
  "code": "local s='Hello, 你好' print(utf8.len(s), #s)"
}
```

返回：

```
9	14
```

### 例 3：把表序列化为字符串

调用：

```json
{
  "code": "local t={1,2,3,'foo'} print(table.concat(t,'|'))"
}
```

返回：

```
1|2|3|foo
```

### 例 4：错误演示

调用：

```json
{
  "code": "print('before') error('boom')"
}
```

返回：

```
before
ERROR: [string \"chunk\"]:1: boom
```

### 例 5：自定义超时

调用（500 ms 内必须跑完）：

```json
{
  "code": "local t=0 for i=1,1e9 do t=t+i end print(t)",
  "timeout_ms": 500
}
```

返回（典型）：

```
ERROR: [string \"chunk\"]:1: execution timed out
```

## Tips

- **必须 `print()` 想要回传的值**——脚本内部计算后什么都没 print 的话，模型只会拿到 "completed with no output."。
- 多个值用 `print(a, b, c)` 一次写出比多次 `print` 更紧凑。
- 想把表完整回传，先序列化（如 `table.concat`、手写 JSON 编码或 `string.format`），不要直接 `print(t)`——只会得到 `table: 0x...`。
- 如果脚本本身可能抛错，包一层 `pcall`：`local ok, err = pcall(function() ... end) print(ok, err)`，这样工具仍以成功状态返回，让模型自己处理。

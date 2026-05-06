# Lua GPIO

让代理在设备本地通过 `lua_run_script` 直接读写 T5AI 板子的 GPIO 引脚——例如点亮一个 LED、读一个按键、生成简单的脉冲序列等。

> 这份文件既是给 LLM 读的能力说明，也可以原样写到运行时 `skills/` 目录（`/spiffs/skills/lua_gpio.md` 或 `/sdcard/skills/lua_gpio.md`），由 `skill_loader` 扫描后纳入 system prompt 摘要。

## When to use

- 需要直接操作板上 GPIO（点亮 LED、读按键、控制继电器/蜂鸣器/电机驱动板的使能脚等）。
- 需要做简单的位翻转 / 序列输出，写一段 Lua 比串多次单引脚 MCP 工具更直接。
- 需要"读一下当前电平再决定回什么"——条件判断后回传一个值给模型。

**不要**用它来：

- 做高速时序（>1 kHz 翻转）。Lua 解释器开销 + tkl 调用开销使得每次 `set_level` 大约百微秒级。需要纳秒级精度请考虑专门的 PWM/SPI/I2S 工具。
- 长时间循环阻塞（脚本墙钟超时默认 3 秒，超过即被强制中断）。需要长延时配合 `delay.delay_ms` 主动让出 CPU。
- 跨脚本共享引脚状态。每次 `lua_run_script` 都是新的 `lua_State`，但 GPIO 硬件电平会保留。

## How to use

调用工具：`lua_run_script`，在 `code` 字段里写 Lua 5.5 源代码。

可用 API（运行时已自动 `require` 进 `gpio` 全局表）：

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `gpio.set_direction(pin, mode)` | `pin: integer`, `mode: string` | 无 | 配置引脚方向 / 上下拉 / 推挽。任意 I/O 前可显式调用一次；下面的 `set_level`/`get_level` 也会内部按需重新 init。 |
| `gpio.set_level(pin, level)` | `pin: integer`, `level: 0\|1` | 无 | 输出高/低电平。每次调用都会以 `output / push_pull` 重新 init，因此对同一引脚调用 `set_level(p, 1)` 后再 `set_level(p, 0)` 是安全的。 |
| `gpio.get_level(pin)` | `pin: integer` | `0\|1` | 读取引脚当前电平。每次调用都会以 `input / pullup` 重新 init。 |

`pin` 是 T5AI 板原生引脚号，范围 `0..55`（即 P0–P55）。越界会以 `gpio: pin <n> out of range (0-55)` 抛错。

`mode` 字符串支持以下取值（其它字符串会以 `gpio: invalid mode 'xxx'` 抛错）：

| mode | 内部映射 (direct / mode) | 用途 |
|------|--------------------------|------|
| `"input"` | `INPUT / PULLUP` | 读按键、读外部数字传感器 |
| `"output"` | `OUTPUT / PUSH_PULL` | 驱动 LED / MOSFET / 继电器 enable |
| `"input_output"` | `OUTPUT / PUSH_PULL` | 与 `output` 等价（TuyaOpen 无 INOUT，沙盒为兼容 esp-claw 而保留这个别名） |
| `"output_od"` | `OUTPUT / OPEN_DRAIN` | 驱动 I2C / 1-Wire 等需开漏的总线脚 |
| `"input_output_od"` | `OUTPUT / OPEN_DRAIN` | 与 `output_od` 等价 |
| `"disable"` | `INPUT / FLOATING` | 浮空,把引脚"撒手不管" |

返回：来自 `lua_run_script`，单个字符串（`print()` 的内容；详见 `lua_run` skill）。GPIO 函数本身不返回值给 Lua（除 `get_level`），错误会作为 Lua error 被工具捕获并以 `ERROR: ...` 回传。

## Examples

### 例 1：点亮 P10 上的 LED

```json
{
  "code": "gpio.set_direction(10, 'output') gpio.set_level(10, 1) print('LED on')"
}
```

返回：

```
LED on
```

### 例 2：读一个按键并回传状态

板上按键接在 P12，按下时拉低（PULLUP 模式下，未按为 1，按下为 0）。

```json
{
  "code": "gpio.set_direction(12,'input') print(gpio.get_level(12)==0 and 'pressed' or 'released')"
}
```

返回（按下时）：

```
pressed
```

### 例 3：闪烁 5 次（结合 delay 模块）

需要同时启用 `lua_delay` skill 才能用 `delay.delay_ms`。

```json
{
  "code": "gpio.set_direction(10,'output') for i=1,5 do gpio.set_level(10,1) delay.delay_ms(100) gpio.set_level(10,0) delay.delay_ms(100) end print('blinked')"
}
```

返回：

```
blinked
```

### 例 4：错误演示——非法引脚号

```json
{
  "code": "gpio.set_level(99, 1)"
}
```

返回：

```
ERROR: [string "lua_run_script"]:1: gpio: pin 99 out of range (0-55)
stack traceback: ...
```

## Tips

- **没必要每次都 `set_direction`**——`set_level` / `get_level` 内部会按用途重新 init。只有当你想显式改成开漏 / 浮空时才需要先调 `set_direction`。
- **要回传状态，记得 `print`**——脚本算出 `gpio.get_level(...)` 后不 print 的话，模型只会拿到 "completed with no output."。
- **跨脚本不会复位电平**——`lua_run_script` 调用结束后 GPIO 硬件保持上次写入的电平。要关灯请显式 `gpio.set_level(pin, 0)`。
- **避免极短脉冲**——两次连续 `gpio.set_level` 之间的最小延时大约 100µs；要更短需用 PWM 或专用驱动。
- **板子 LED / 按键的引脚号**取决于具体板型；请先在 USER.md / 设备文档里确认，或先读 `device.json` 这类配置文件。

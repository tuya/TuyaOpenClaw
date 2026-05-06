# Lua Delay

让代理在 `lua_run_script` 沙盒里执行毫秒 / 微秒级阻塞延时——通常用于配合 `gpio` 输出脉冲序列、给外设留稳定时间、或在循环里限速读取传感器。

> 这份文件既是给 LLM 读的能力说明,也可以原样写到运行时 `skills/` 目录(`/spiffs/skills/lua_delay.md` 或 `/sdcard/skills/lua_delay.md`)，由 `skill_loader` 扫描后纳入 system prompt 摘要。

## When to use

- 在 GPIO 翻转 / I2C bit-bang / 传感器复位等场景之间需要确定的间隔。
- 在 Lua 循环里以稳定节奏采样（例如 100 ms 一次连续读 10 次）。
- 等外设（如 LCD reset、外置电源使能）稳定后再继续操作。

**不要**用它来：

- 让脚本 sleep 比脚本超时还久。`lua_run_script` 默认墙钟超时 3000 ms，超过会被强制中断。
- 替代后台任务调度（每 N 秒触发一次），那应该用 `cron_add`。
- 做高精度定时（< 几微秒）。`delay_us` 是阻塞自旋实现,实际抖动取决于中断和调度,精度大约 ±10 µs 以内。

## How to use

调用工具：`lua_run_script`,在 `code` 字段里写 Lua 5.5 源代码。

可用 API（运行时已自动 `require` 进 `delay` 全局表）：

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `delay.delay_ms(ms)` | `ms: integer >= 0` | 无 | 阻塞当前任务 `ms` 毫秒。底层走 `tal_system_sleep`，会让出 CPU 给其它任务。 |
| `delay.delay_us(us)` | `us: integer >= 0`, 上限 `1_000_000` | 无 | 阻塞当前任务 `us` 微秒。底层走 `tkl_system_sleep_us`,通常为忙等。超过 1 000 000 会以 `delay: delay_us supports 0..1000000 only, use delay_ms for longer waits` 抛错。 |

边界与行为：

- `ms`/`us` 为负数时会被截为 0,等价于"立即返回"。
- `delay_us` 上限 1 秒;一秒以上的等待请改用 `delay_ms` 让调度器跑其它任务。
- 这两个延时**计入**脚本的整体超时（默认 3000 ms）。在 `code` 里 `delay_ms(2500)` 后只剩 500 ms 跑别的逻辑;如果可能超出，调 `lua_run_script` 时记得显式抬高 `timeout_ms`。

## Examples

### 例 1：等 200 毫秒后再读传感器

```json
{
  "code": "delay.delay_ms(200) print('after 200ms')"
}
```

返回：

```
after 200ms
```

### 例 2：闪烁 LED 三次（结合 gpio 模块）

需要同时启用 `lua_gpio` skill 才能用 `gpio.set_level`。

```json
{
  "code": "gpio.set_direction(10,'output') for i=1,3 do gpio.set_level(10,1) delay.delay_ms(150) gpio.set_level(10,0) delay.delay_ms(150) end print('done')"
}
```

返回：

```
done
```

### 例 3：手动 bit-bang 一个微秒级脉冲

P5 输出一个 ~10µs 的高电平脉冲——典型 HC-SR04 触发用法。

```json
{
  "code": "gpio.set_direction(5,'output') gpio.set_level(5,1) delay.delay_us(10) gpio.set_level(5,0) print('triggered')"
}
```

返回：

```
triggered
```

### 例 4：超时演示

`delay_ms` 也会被脚本墙钟超时打断。下面这段在默认 3000 ms 超时下会跑完，但若把 `timeout_ms` 调到 500 ms 就会失败：

```json
{
  "code": "delay.delay_ms(2000) print('woke up')",
  "timeout_ms": 500
}
```

返回（典型）：

```
ERROR: [string "lua_run_script"]:1: execution timed out
stack traceback: ...
```

### 例 5：超出 `delay_us` 上限

```json
{
  "code": "delay.delay_us(2000000)"
}
```

返回：

```
ERROR: [string "lua_run_script"]:1: delay: delay_us supports 0..1000000 only, use delay_ms for longer waits
stack traceback: ...
```

## Tips

- **长延时一律用 `delay_ms`**——`delay_us` 是忙等，秒级使用会饿死同核其它任务。
- **延时算进超时**——总耗时（延时 + 计算）超过脚本 `timeout_ms` 会被强制中断；需要明确预留余量。
- **不要在循环里 sleep 1 ms 来"软等"** ——直接 `delay.delay_ms(N)` 一次到位,精度更稳。
- **配合 GPIO 序列**时,先把方向/初始电平设好,再用 `for` + `delay` 控制时序;避免在循环里反复调 `set_direction`。
- **跨平台限制**：当前 `delay_us` 仅在 T5AI 平台有实现;移植到其它板时需要确认 `tkl_system_sleep_us` 是否可用。

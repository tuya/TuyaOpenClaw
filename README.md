# VirtuaMate

<p align="center">
  <strong>不是"会动的模型"，是能陪你、能办事、能进化的 3D AI 生命体。</strong><br>
  一个把 <b>OpenClaw 式 Agent 灵魂</b>、<b>DuckyClaw 硬件路线</b> 和 <b>实时 VRM 数字人</b> 真正融合在一起的开源项目。
</p>

<p align="center">
  <a href="#-效果演示">效果演示</a> · <a href="#-功能亮点">功能亮点</a> · <a href="#-快速开始">快速开始</a> · <a href="#-参与贡献">参与贡献</a>
</p>

> [!WARNING]
> 项目仍在持续迭代中，接口和行为可能变化。欢迎提 Issue 或 PR 一起完善。

---

## 📸 效果演示

<p align="center">
  <img src="https://images.tuyacn.com/fe-static/docs/img/726e637b-665f-41a5-8170-e24da611bcfc.png" width="720" alt="VirtuaMate 主界面" />
</p>

<p align="center">
  <em>▲ 实时 3D Avatar 渲染 — 情绪驱动表情 · Toon 着色 · 地面阴影</em>
</p>


## 💡 为什么是 VirtuaMate

很多"数字人"只停留在展示层：会动但不会做事。  
很多"Agent"只停留在终端层：会做事但没有存在感。

VirtuaMate 直接把 **3D Avatar** 和 **可执行 Agent** 合在一起 —— **有灵魂，也有手脚**：

- 🎭 **她是 3D 的** — VRM 模型 + VRMA 动画 + 天空盒场景，运行在本地设备
- 💖 **她是有情绪的** — 对话流式输出中，实时文本情绪分析驱动表情变化
- 🛠️ **她是能行动的** — 通过 MCP 工具完成提醒、文件操作、PC 协作等任务
- 🧠 **她是有记忆的** — 长期记忆和日记存本地，不是一次性聊天窗口

---

## 🔗 VirtuaMate × DuckyClaw

VirtuaMate 基于 [DuckyClaw](https://github.com/tuya-open/DuckyClaw) 构建 —— 复用它的 Agent 内核、消息总线和 MCP 工具链，在上层加了一个 3D 伴侣。

```
 DuckyClaw（底座）           VirtuaMate（上层）
 ┌───────────────────┐      ┌───────────────────┐
 │  Agent Loop       │      │  VRM 实时渲染      │
 │  消息总线         │  ──► │  情绪 · 动作 · 口型 │
 │  MCP 工具链       │      │  天空盒 · 场景     │
 └───────────────────┘      └───────────────────┘
```

简单说：**DuckyClaw 让 Agent 能干活，VirtuaMate 让 Agent 有了"人"的存在感。**

---

## 🌟 功能亮点

### 🎨 3D Avatar 渲染与表现

| 特性 | 说明 |
|---|---|
| VRM 实时渲染 | `SDL2 + OpenGL + Assimp` 渲染链路，骨骼动画与材质 |
| 动作系统 | `.vrma` 动画文件，idle / one-shot 自动切换 |
| 表情系统 | 内置多种 Emotion — happy / sad / thinking / loving … |
| 口型同步 | TTS 音频驱动嘴型变化 |
| Toon 着色 | 卡通风格 cel shading + 柔边明暗过渡 |
| 天空盒场景 | 运行时加载 6 面贴图切换场景 |

### 🤖 AI Agent 与多轮工具循环

| 特性 | 说明 |
|---|---|
| Claw-style Agent Loop | 单轮消息内多次 LLM → tool → LLM 迭代 |
| 流式响应联动 | AI 文本回调实时刷新字幕、情绪和对话历史 |
| 多通道统一接入 | Telegram / Discord / Feishu / CLI 走统一 message bus |

### 🧰 MCP 工具能力（设备侧）

| 工具 | 接口 |
|---|---|
| 文件工具 | `read_file` / `write_file` / `edit_file` / `list_dir` / `find_path` |
| 时间与提醒 | `get_current_time` / `cron_add` / `cron_list` / `cron_remove` |
| Linux 执行 | `tool_exec`（Linux 平台可用） |
| PC 协作 | `openclaw_ctrl` / `pc_ctrl` / `openclaw.ctrl` |
| 3D Avatar | `avatar_play_animation` / `avatar_set_emotion` / `avatar_composite_action` |

### 📝 本地记忆与可塑人格

| 特性 | 路径 / 说明 |
|---|---|
| 长期记忆 | `/memory/MEMORY.md` |
| 每日笔记 | `/memory/daily/YYYY-MM-DD.md` |
| 人设与画像 | `SOUL.md`、`USER.md` 注入系统提示词 |
| 技能系统 | `skills/*.md` 自动汇总并注入 prompt |

---

## 🖥️ 部署平台

| 平台 | 状态 |
|---|---|
| Raspberry Pi 5（Debian / Ubuntu ARM64） | ✅ **已验证** — 推荐平台，需本机编译 |
| Linux x64 | ⚠️ 理论可运行，暂未测试 |
| 其他 TuyaOpen 板卡 | 可按 TuyaOpen 方式迁移 |

> [!IMPORTANT]
> 目前编译和运行**仅在 Raspberry Pi 上完成过完整测试**。项目依赖 TuyaOpen SDK 的本地工具链，暂不支持交叉编译，需要在目标设备上直接构建。其他平台欢迎社区补充测试反馈。

---

## 🚀 快速开始

### Step 1 — 拉取项目与子模块

```bash
git clone <your-fork-or-repo-url> VirtuaMate
cd VirtuaMate
git submodule update --init --recursive
```

### Step 2 — 安装渲染依赖（Raspberry Pi / Ubuntu）

```bash
sudo apt update
sudo apt install -y libsdl2-dev libglew-dev libassimp-dev
```

### Step 3 — 选择板级配置

```bash
cp config/RaspberryPi.config app_default.config
```

### Step 4 — 构建

```bash
cd TuyaOpen
. ./export.sh
cd ..
tos.py build
```

构建产物在 `dist/` 目录。

### Step 5 — 运行

```bash
./dist/VirtuaMate_1.0.0/VirtuaMate_1.0.0
```

启动后即可看到 3D Avatar 窗口。点击左上角 **☰** 打开设置面板。

<p align="center">
  <img src="https://images.tuyacn.com/fe-static/docs/img/38463359-971d-44c3-ba42-419120c53025.png" width="600" alt="启动运行效果" />
</p>

---

## 🎁 资源准备

`RaspberryPi.config` 默认资源路径：

| 资源 | 路径 |
|---|---|
| 模型 | `resources/models/avatar.vrm` |
| 动作目录 | `resources/animations/` |
| 场景父目录 | `resources/scenes/` |

**天空盒命名规则** — 每个场景目录放 6 张贴图，支持以下命名：

```
right / left / top / bottom / front / back
px / nx / py / ny / pz / nz
posx / negx / posy / negy / posz / negz
```

扩展名支持 `.jpg` `.jpeg` `.png` `.bmp` `.tga`

---

## 📁 目录结构

```
VirtuaMate/
├── agent/            # Agent loop + context builder
├── IM/               # Telegram / Discord / Feishu / CLI + message bus
├── tools/            # MCP tools (files / cron / exec / openclaw)
├── memory/           # MEMORY.md + daily notes + session
├── gateway/          # WebSocket / ACP gateway
├── skills/           # Markdown skills
├── src/
│   ├── tuya_app_main.c      # 应用入口
│   ├── ducky_claw_chat.c    # AI 流事件处理
│   ├── app_avatar_mcp.c     # 3D Avatar MCP 工具
│   └── vrm/                 # VRM 渲染、情绪、口型、天空盒
├── config/
│   └── RaspberryPi.config
└── TuyaOpen/                # TuyaOpen SDK（子模块）
```

---

## 🔮 高级玩法

| 目标 | 方法 |
|---|---|
| 改人设 | 调整 `SOUL.md`、`USER.md`，结合 `agent/context_builder.c` 注入策略 |
| 加动作 | `.vrma` 放进 `resources/animations`，在 `src/app_avatar_mcp.c` 补充动作名 |
| 加场景 | `resources/scenes/<name>/` 放入 6 面贴图，运行时切换 |
| 扩工具 | `tools/` 新增 MCP 工具，在 `tools/tools_register.c` 注册 |
| 扩能力 | 摄像头 / 传感器 / IoT — 复用 TuyaOpen + MCP 工具模式扩展 |

---

## 🎯 项目定位

VirtuaMate 不是"AI 套个皮肤"，而是一套**可定制、可运行、可执行、可持续进化**的 3D AI 伴侣系统。

> 给她一张脸、一个名字、一段性格，  
> 然后见证她从"能聊"变成"懂你、帮你、陪你"。 ✨

---

## 🤝 参与贡献

VirtuaMate 是一个开放的项目，欢迎任何人参与。

**特别欢迎以下方向的 PR：**

- 渲染效果改进（描边、后处理、光照模型等）
- 新动画 / 新表情 / 口型优化
- 新 MCP 工具（智能家居控制、日程管理、浏览器操作…）
- 新平台适配（macOS、Windows、更多嵌入式板卡）
- 文档完善、教程编写、翻译
- Bug 修复和性能优化

**流程：** Fork → 创建分支 `feat/your-feature` → 提交改动 → 发起 PR

> 不确定从哪里开始？查看 [Issues](../../issues) 中带有 `good first issue` 标签的任务，或直接开个 Issue 聊聊想法。

---

## 📜 License

本项目基于 [Apache-2.0 license](LICENSE) 开源。

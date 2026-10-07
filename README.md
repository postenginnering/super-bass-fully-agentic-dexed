# Super Bass Fully Agentic Dexed

Super Bass Fully Agentic Dexed（SBFAD）是一款开源 FM 合成器，提供 Windows x64 和 Apple Silicon macOS 原生 Standalone / VST3。它基于 Dexed，并加入中文自然语言 Agent：输入“做一个空灵、慢起音、余音较长但不能无限延音的 pad”，Agent 会读取当前音色、修改参数、试听并用自然语言说明结果。

界面只保留“发送、回退、保存为预设”三个主要操作。中文输入可直接按回车发送；“回退”恢复到本次用户输入之前的完整音色状态。除非当前请求明确要求，Agent 会检查并避免无限延音。

## 使用方法

1. 打开“生成”页面，在设置中选择协议、服务地址和模型，并保存自己的 API Key。
2. 在对话框描述想要的声音，按回车或“发送”。
3. 不满意时点“回退”；满意时点“保存为预设”。

支持 OpenAI Responses API 和 OpenAI Chat Completions 兼容接口。默认配置是 OpenAI Responses；DeepSeek 使用 Chat Completions、服务地址 `https://api.deepseek.com`，模型名可填写账户可用的模型。本功能已用 DeepSeek Flash 真实请求回归。不同兼容服务对流式输出和工具调用的实现可能不同，应先使用界面里的连接测试。

API Key 存在系统凭据存储或当前进程内存中，不会写入预设、`synth.md`、聊天上下文或测试日志。仓库和发布包不附带 API Key。

## 自动选择性记忆

每轮对话结束后，后台整理器会选择性更新本机的 `synth.md`，用户不需要下达“记住”指令。

- “我一般偏好温暖柔和的 pad”属于明确长期偏好，可在首次出现时写入。
- “这次做一个明亮的 pad”属于一次性要求，只进入候选观察；两个不同轮次重复表达后才会晋升。
- 否定、修正和删除会更新同一主题；无关个人信息、路径和凭据会被拒绝。
- 所有预设共享长期音色偏好；每个预设拥有独立连续对话，切换预设会同时切换聊天记录。
- 对话超过 12 轮、未压缩内容超过 160 KiB 或接近请求预算时，后台压缩旧内容，同时保留最近 8 个完整轮次。

本机文件位置：

- Windows：`%APPDATA%\Super Bass Fully Agentic Dexed\synth.md`
- macOS：`~/Library/Application Support/Super Bass Fully Agentic Dexed/synth.md`
- 同目录的 `memory-state.json` 保存尚未晋升的一次性偏好候选。
- 同目录的 `contexts/` 保存每个预设的压缩上下文，`preset-index.json` 保存预设身份和 `.syx` 内容指纹。

删除 `synth.md` 和 `memory-state.json` 可清除全局偏好；删除 `contexts/` 与 `preset-index.json` 可清除所有预设对话和本机身份关联。关闭 Standalone 和 DAW 后再删除，避免其他实例随后写回。

详见 [本地记忆与预设上下文](Documentation/SynthMemory.md)。

## 预设、上下文与分享

`.dexedpreset` 会携带当前音色以及脱敏后的预设对话上下文。上下文经过 GZIP 压缩和 SHA-256 完整性校验，解压上限为 2 MiB；损坏或超限的上下文会被丢弃，音色本身仍可加载。

传统 `.syx` 格式保持不变，不写入聊天或偏好。本机通过规范化音色内容指纹恢复已有预设身份；显式复制预设会创建独立上下文。

分享 `.dexedpreset` 前仍应自行检查内容。过滤器能识别已知凭据、私钥、带密码 URL、本机路径和 Tailscale 地址，但不能保证识别全部个人信息或未知秘密格式。

## 模型数据流与隐私

发送主请求时，所选模型会收到当前请求、全局 `synth.md` 偏好、当前预设的摘要和受预算限制的最近对话，以及执行声音设计所需的工具结果。主回答结束后，后台可能再发送不带合成器工具的偏好整理请求；达到阈值时还会发送一次上下文压缩请求。后台请求不能修改合成器参数。

存储内容始终作为不可信数据传入，不能授权工具、保存文件或无限延音。当前用户请求拥有最高优先级。

## 构建

依赖 CMake 3.24+、Git 子模块，以及 Windows 的 Visual Studio 2022 C++ 工具或 macOS 的 Xcode。完整命令见 [构建说明](Documentation/BuildingAgenticDexed.md)。

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64 `
  -DAGENTIC_DEXED_BUILD_TESTS=ON -DAGENTIC_DEXED_COPY_PLUGIN_AFTER_BUILD=OFF
cmake --build build/windows --config Release --target AgenticDexedTests AgenticDexed_VST3 AgenticDexed_Standalone
ctest --test-dir build/windows -C Release --output-on-failure
```

Apple Silicon 构建必须显式设置 `-DCMAKE_OSX_ARCHITECTURES=arm64`。发布前在对应原生机器上运行测试与 pluginval；Windows 构建不能代替 Apple Silicon 验证。GitHub Actions 当前保持停用，验证以本机报告为准。

## 分支与许可

- `main`：项目入口。
- `windows`：Windows x64 源码与构建说明。
- `apple-silicon-macos`：Apple Silicon 原生源码与构建说明。
- `feature/local-synth-memory`：自动记忆与每预设上下文功能分支。
- `release`：经验证的构建产物、校验值与报告。

本项目采用 GPL-3.0。参见 [LICENSE](LICENSE)、[上游归属](UPSTREAM.md) 和 [第三方声明](THIRD_PARTY_NOTICES.md)。

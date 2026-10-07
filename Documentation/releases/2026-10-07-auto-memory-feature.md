# 自动记忆与每预设连续上下文候选说明

日期：2026-10-07

分支：`feature/local-synth-memory`

## 这次新增了什么

- AI 会在每轮正常对话结束后，于后台选择性整理长期音色偏好，不要求用户主动说“记住”。
- 所有预设共享本机 `synth.md`，每个预设同时拥有独立对话、摘要和稳定身份。
- `.dexedpreset` 可以携带脱敏后的连续上下文；`.syx` 保持原格式，通过音色指纹在本机恢复对应上下文。
- 长对话会自动压缩旧轮次，并保留最近 8 个完整轮次继续交流。
- 回退以本次用户输入为边界，一次恢复该轮开始前的完整音色。
- 偏好整理和上下文压缩没有合成器工具权限，不能在后台改动声音。

## Windows 验证结果

- Release Tests、Standalone、VST3 构建通过。
- CTest 8/8 通过，共 21,554 项断言，0 失败。
- pluginval strictness 8 通过，覆盖 44.1/48/96 kHz 与 1/32/64/512/1024 采样块。
- 真实 DeepSeek `LiveMemory` 26 项断言通过，确认全局偏好、新预设隔离、上下文压缩和后台零参数写入。
- Standalone 使用隔离配置完成高 DPI 人工冒烟：产品名称统一，页面完整展示，预设切换与生成页无崩溃。

## 当前 Windows 二进制校验

| 产物 | SHA-256 |
| --- | --- |
| `AgenticDexedTests.exe` | `b5091b7bfc1f0eef38d15bbd265d217e37ff717c5b9d9747dd0a7a563edce7f0` |
| `Super Bass Fully Agentic Dexed.exe` | `2d99d3dafe1846d0c7919bce99d967a64ae8ae42003e2f43ae08f0138515f361` |
| `Super Bass Fully Agentic Dexed.vst3` | `2e6dba1a2b9c2f5329be928d0ba459fc3249c384f4ef61b3982c808b8ce4c740` |

这些哈希对应本轮本机验证产物，不代表尚未制作的安装包。

## 安全审计

- 扫描当前受控文件、完整 Git 历史、验证日志、最新三个二进制和仓库内两个压缩包。
- 用户会话中曾提供的密钥在 UTF-8、UTF-16LE 和 Git 历史搜索中均为 0 命中。
- 通用凭据规则只命中专门测试脱敏行为的假密钥、假私钥和假私人路径夹具；未发现 Bearer 凭据。
- 仓库自带发布扫描器通过。本机未安装 `gitleaks`，因此没有把它列为已执行项目。

## 仍待完成的原生验证

Apple Silicon 必须基于这次功能重新执行原生 ARM 构建、CTest、pluginval、真实模型、保存与重启往返。此前的 Mac 报告早于自动记忆功能，不作为本候选的通过证据。

GitHub Actions 当前关闭；本候选以本地验证日志为准。

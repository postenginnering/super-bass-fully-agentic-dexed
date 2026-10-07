# 发布验证矩阵

本页记录 `feature/local-synth-memory` 当前候选的实际验证结果。Windows 结果不能替代 Apple Silicon 原生验证；早于自动记忆功能的 Mac 报告不作为本候选证据。

| 范围 | 平台 | 状态 | 证据 |
| --- | --- | --- | --- |
| Release 构建：Tests、Standalone、VST3 | Windows x64 | 通过 | Visual Studio Release 构建完成，无失败目标 |
| 全量 CTest | Windows x64 | 通过 | 8/8 测试，21,554 项断言，0 失败，187.83 秒 |
| VST3 宿主验证 | Windows x64 | 通过 | pluginval strictness 8；44.1/48/96 kHz；block size 1/32/64/512/1024；结果 `SUCCESS` |
| 真实 DeepSeek 自动记忆 | Windows x64 | 通过 | `LiveMemory` 26 项断言，0 失败，16.145 秒；无合成器参数写入，临时数据已清理 |
| Standalone 人工冒烟 | Windows x64 | 通过 | 隔离配置启动；名称统一；高 DPI 2576×1588；切换预设与生成页无崩溃；页面无需滑动菜单 |
| 中文输入、回车发送 | Windows x64 | 自动化通过 | 原生 CJK/Emoji 往返和 Enter 提交测试通过；本轮桌面自动化受前台焦点策略限制，未计作人工通过 |
| 三个主操作 | Windows x64 | 通过 | 原生组件始终包含发送、回退、保存为预设；无可回退请求时回退为禁用状态 |
| 请求级回退 | Windows x64 | 通过 | 一次回退以单个原子事务恢复到本次用户输入前的完整音色；跨编辑器重建覆盖 |
| 每预设连续上下文 | Windows x64 | 通过 | 全部预设具有独立 UUID、历史、摘要；保存、导入、复制、重命名、移动、重启回归通过 |
| 自动选择性偏好 | Windows x64 | 通过 | 后台整理、两轮晋升、无效响应保留、取消延后、关闭清理回归通过 |
| 上下文压缩 | Windows x64 | 通过 | 超过阈值后压缩旧轮次并保留最近 8 个完整轮次；版本冲突不会覆盖新内容 |
| 敏感信息审计 | Windows x64 | 通过 | 当前树、完整 Git 历史、日志、二进制及压缩包解包扫描；已披露密钥 UTF-8/UTF-16LE 均为 0 命中；发布扫描器通过 |
| Release 构建、CTest、pluginval、真实模型、保存重启 | macOS arm64 | 待验证 | 必须使用包含本功能的新原生 ARM 包生成报告 |

当前机器没有安装 `gitleaks`。审计使用仓库发布扫描器、精确字节扫描、Git 全历史搜索和通用凭据模式完成；通用规则命中的内容仅位于脱敏测试夹具。

GitHub Actions 当前关闭，因此不将远端工作流状态列为验证证据。当前结论是 Windows 原生候选已通过本地回归；跨平台发布仍等待 Apple Silicon 新报告。

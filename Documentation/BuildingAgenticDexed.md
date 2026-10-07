# 构建 Super Bass Fully Agentic Dexed

项目生成 Windows x64 Standalone / VST3，以及 macOS 原生 Standalone / VST3 / AU。默认不会复制插件到系统目录。

## 准备源码

需要 CMake 3.24+、Git，以及对应平台工具链。克隆后初始化固定版本的子模块：

```bash
git submodule update --init --recursive
```

## Windows x64

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”工作负载，然后运行：

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64 `
  -DAGENTIC_DEXED_BUILD_TESTS=ON `
  -DAGENTIC_DEXED_COPY_PLUGIN_AFTER_BUILD=OFF
cmake --build build/windows --config Release --parallel 2 `
  --target AgenticDexedTests AgenticDexed_VST3 AgenticDexed_Standalone
ctest --test-dir build/windows -C Release --output-on-failure
```

产物：

- `build/windows/Source/AgenticDexed_artefacts/Release/Standalone/Super Bass Fully Agentic Dexed.exe`
- `build/windows/Source/AgenticDexed_artefacts/Release/VST3/Super Bass Fully Agentic Dexed.vst3`

## Apple Silicon macOS

安装 Xcode 和命令行工具。必须显式指定 `arm64`，以免交付 Rosetta / Intel 构建：

```bash
cmake -S . -B build/macos-arm64 -G Xcode \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DAGENTIC_DEXED_BUILD_TESTS=ON \
  -DAGENTIC_DEXED_COPY_PLUGIN_AFTER_BUILD=OFF
cmake --build build/macos-arm64 --config Release --parallel 2 \
  --target AgenticDexedTests AgenticDexed_VST3 AgenticDexed_AU AgenticDexed_Standalone
ctest --test-dir build/macos-arm64 -C Release --output-on-failure
```

产物位于 `build/macos-arm64/Source/AgenticDexed_artefacts/Release/`。交付前检查：

```bash
file 'Standalone/Super Bass Fully Agentic Dexed.app/Contents/MacOS/Super Bass Fully Agentic Dexed'
file 'VST3/Super Bass Fully Agentic Dexed.vst3/Contents/MacOS/Super Bass Fully Agentic Dexed'
file 'AU/Super Bass Fully Agentic Dexed.component/Contents/MacOS/Super Bass Fully Agentic Dexed'
```

三个原生二进制都必须显示 `arm64`。AU 还必须通过：

```bash
bash ./scripts/validate-au.sh \
  --component-path './build/macos-arm64/Source/AgenticDexed_artefacts/Release/AU/Super Bass Fully Agentic Dexed.component' \
  --output-dir ./build/macos-arm64/validation/au
```

脚本会临时安装 AU、运行 `auval -v aumu AgDx Agnt`，然后恢复原有同名组件。未签名包在隔离属性存在时可能显示“已损坏”；正式分发应完成签名、公证与 stapling，内部测试包则需在明确来源可信后处理隔离属性。

## 本机验证

构建成功后运行完整 CTest，再使用固定版本的 pluginval strictness 8 检查 VST3：

```powershell
./scripts/validate-plugin.ps1 `
  -PluginPath "./build/windows/Source/AgenticDexed_artefacts/Release/VST3/Super Bass Fully Agentic Dexed.vst3" `
  -Strictness 8 -OutputDirectory ./build/validation
```

真实 DeepSeek 自动记忆回归是付费、显式启用的测试：

```powershell
$env:DEEPSEEK_API_KEY = '<本机临时提供，不要写入脚本或仓库>'
./build/windows/Tests/Release/AgenticDexedTests.exe --filter LiveMemory
Remove-Item Env:DEEPSEEK_API_KEY
```

也可使用应用已保存在系统凭据存储中的 `agent.model`。测试不打印 Key，并使用临时目录，结束后自动删除。

macOS 发布包中的 `Run-Full-Regression.command` 会依次运行本机回归、pluginval、真实声音设计和自动记忆测试，并生成不含源码与凭据的报告压缩包。

## 安装与 CI 状态

将 `AGENTIC_DEXED_COPY_PLUGIN_AFTER_BUILD` 设为 `ON` 可在构建后复制 VST3/AU 到当前用户插件目录；默认 `OFF`。

仓库保留 `.github/workflows` 作为可审查的构建定义。GitHub Actions 平时保持停用，只在明确授权的 Release 构建期间临时启用；发布完成后再次关闭。

#!/bin/zsh
cd -- "${0:A:h}" || exit 1
results="Mac-regression-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$results" || exit 1
report="$results/summary.txt"
finish() {
    local result_code="$1"
    printf '\nExit status: %s\n' "$result_code" >> "$report"
    tar --exclude "$results/pluginval-tool" -czf "$results.tar.gz" "$results"
    printf '\n报告已保存：%s/%s.tar.gz\n' "$PWD" "$results"
    if [[ "$result_code" == 0 ]]; then
        printf '自动检查已通过。还请打开应用检查中文输入，并在 DAW 中试听 VST3。\n'
    else
        printf '检查尚未全部通过。请将报告发回，不必自行排查。\n'
    fi
    printf '按回车关闭。\n'
    read -r
    exit "$result_code"
}
printf 'Super Bass Fully Agentic Dexed — R6 responsive UI native Mac regression\n' > "$report"
if [[ "$(uname -m)" != arm64 ]]; then
    printf '请使用原生 Apple Silicon 终端运行，不能使用 Rosetta。\n' | tee -a "$report"
    finish 2
fi
sw_vers >> "$report"
shasum -a 256 ./AgenticDexedTests './Super Bass Fully Agentic Dexed.app/Contents/MacOS/Super Bass Fully Agentic Dexed' './Super Bass Fully Agentic Dexed.vst3/Contents/MacOS/Super Bass Fully Agentic Dexed' >> "$report"
printf 'Source-only audit and build-script checks run on the build checkout; this package contains runtime data, not source.\n' >> "$report"

printf '1/4 正在运行本机回归测试…\n'
./AgenticDexedTests --portable > "$results/runtime.log" 2>&1
runtime_result=$?
grep 'SUMMARY:' "$results/runtime.log" >> "$report"
if [[ -d build/macos/workbench-render ]]; then
    cp -R build/macos/workbench-render "$results/界面检查"
fi
[[ "$runtime_result" == 0 ]] || finish "$runtime_result"

printf '2/4 正在校验随包的官方插件检查工具…\n'
validate_plugin() {
mkdir -p "$results/pluginval-tool"
local tool_zip="tools/pluginval_macOS.zip"
if [[ ! -f "$tool_zip" ]]; then
    printf 'Bundled pluginval archive is missing\n' >> "$report"
    return 2
fi
expected='3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f'
actual="$(shasum -a 256 "$tool_zip" | awk '{print $1}')"
if [[ "$actual" != "$expected" ]]; then
    printf 'Official pluginval archive checksum mismatch\n' >> "$report"
    return 2
fi
unzip -oq "$tool_zip" -d "$results/pluginval-tool" || return 2
validator="$results/pluginval-tool/pluginval.app/Contents/MacOS/pluginval"
chmod +x "$validator"
arch -arm64 "$validator" --strictness-level 8 --timeout-ms 120000 \
    --sample-rates '44100,48000,96000' --block-sizes '1,32,64,512,1024' \
    --output-dir "$PWD/$results/pluginval" \
    --validate "$PWD/Super Bass Fully Agentic Dexed.vst3" > "$results/pluginval-console.log" 2>&1
local plugin_result=$?
printf 'VST3 pluginval exit: %s\n' "$plugin_result" >> "$report"
[[ "$plugin_result" == 0 ]] || return "$plugin_result"
if ! grep -Rq 'SUCCESS' "$results/pluginval"; then
    printf 'Pluginval did not produce successful validation evidence\n' >> "$report"
    return 2
fi
return 0
}
validate_plugin
plugin_result=$?

printf '3/4 正在使用已保存的密钥检查真实模型调用和整轮回退…\n'
./AgenticDexedTests --portable --filter LiveAgent > "$results/live-agent.log" 2>&1
live_result=$?
grep 'SUMMARY:' "$results/live-agent.log" >> "$report"
printf '4/4 正在检查自动偏好记忆和每预设上下文压缩…\n'
./AgenticDexedTests --portable --filter LiveMemory > "$results/live-memory.log" 2>&1
memory_result=$?
grep -E 'SUMMARY:|ELAPSED_MS:' "$results/live-memory.log" >> "$report"
if [[ "$plugin_result" != 0 ]]; then
    finish "$plugin_result"
fi
if [[ "$live_result" != 0 ]]; then
    finish "$live_result"
fi
finish "$memory_result"

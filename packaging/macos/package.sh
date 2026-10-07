#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
x86_build=""
arm_build=""
output_root="${repo_root}/dist/macos"
version="1.0.1"
include_standalone=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --x86-build) x86_build="$2"; shift 2 ;;
        --arm-build) arm_build="$2"; shift 2 ;;
        --output-root) output_root="$2"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        --skip-standalone) include_standalone=0; shift ;;
        --unsigned) shift ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ ! -d "${x86_build}" || ! -d "${arm_build}" ]]; then
    echo "Both --x86-build and --arm-build must name existing build directories" >&2
    exit 2
fi
for tool in lipo ditto pkgbuild; do
    command -v "${tool}" >/dev/null || { echo "Required macOS tool is missing: ${tool}" >&2; exit 2; }
done

x86_artifacts="${x86_build}/Source/AgenticDexed_artefacts/Release"
arm_artifacts="${arm_build}/Source/AgenticDexed_artefacts/Release"
x86_vst3="${x86_artifacts}/VST3/Super Bass Fully Agentic Dexed.vst3"
arm_vst3="${arm_artifacts}/VST3/Super Bass Fully Agentic Dexed.vst3"
x86_au="${x86_artifacts}/AU/Super Bass Fully Agentic Dexed.component"
arm_au="${arm_artifacts}/AU/Super Bass Fully Agentic Dexed.component"
if [[ ! -d "${x86_vst3}" || ! -d "${arm_vst3}" ||
      ! -d "${x86_au}" || ! -d "${arm_au}" ]]; then
    echo "Both architecture-specific VST3 and AU bundles are required" >&2
    exit 2
fi

output_root="$(mkdir -p "${output_root}" && cd "${output_root}" && pwd)"
stage_name="Super-Bass-Fully-Agentic-Dexed-${version}-macos-universal"
stage="${output_root}/${stage_name}"
case "${stage}" in "${output_root}"/*) ;; *) echo "Unsafe stage path" >&2; exit 2 ;; esac
rm -rf "${stage}"
mkdir -p "${stage}/VST3"
ditto "${arm_vst3}" "${stage}/VST3/Super Bass Fully Agentic Dexed.vst3"
lipo -create \
    "${x86_vst3}/Contents/MacOS/Super Bass Fully Agentic Dexed" \
    "${arm_vst3}/Contents/MacOS/Super Bass Fully Agentic Dexed" \
    -output "${stage}/VST3/Super Bass Fully Agentic Dexed.vst3/Contents/MacOS/Super Bass Fully Agentic Dexed"

mkdir -p "${stage}/AU"
ditto "${arm_au}" "${stage}/AU/Super Bass Fully Agentic Dexed.component"
lipo -create \
    "${x86_au}/Contents/MacOS/Super Bass Fully Agentic Dexed" \
    "${arm_au}/Contents/MacOS/Super Bass Fully Agentic Dexed" \
    -output "${stage}/AU/Super Bass Fully Agentic Dexed.component/Contents/MacOS/Super Bass Fully Agentic Dexed"

if [[ ${include_standalone} -eq 1 ]]; then
    x86_app="${x86_artifacts}/Standalone/Super Bass Fully Agentic Dexed.app"
    arm_app="${arm_artifacts}/Standalone/Super Bass Fully Agentic Dexed.app"
    if [[ ! -d "${x86_app}" || ! -d "${arm_app}" ]]; then
        echo "Standalone bundles are required unless --skip-standalone is used" >&2
        exit 2
    fi
    mkdir -p "${stage}/Standalone"
    ditto "${arm_app}" "${stage}/Standalone/Super Bass Fully Agentic Dexed.app"
    lipo -create \
        "${x86_app}/Contents/MacOS/Super Bass Fully Agentic Dexed" \
        "${arm_app}/Contents/MacOS/Super Bass Fully Agentic Dexed" \
        -output "${stage}/Standalone/Super Bass Fully Agentic Dexed.app/Contents/MacOS/Super Bass Fully Agentic Dexed"
fi

cp "${repo_root}/LICENSE" "${stage}/LICENSE"
cp "${repo_root}/THIRD_PARTY_NOTICES.md" "${stage}/THIRD_PARTY_NOTICES.md"
cp "${repo_root}/README.md" "${stage}/README.md"
git_commit="$(git -C "${repo_root}" rev-parse HEAD)"
cmake \
    "-DOUTPUT_FILE=${stage}/manifest.json" \
    "-DVERSION=${version}" \
    "-DPLATFORM=macos" \
    "-DARCHITECTURES=x86_64,arm64" \
    "-DGIT_COMMIT=${git_commit}" \
    -P "${repo_root}/packaging/PackageManifest.cmake"

{
    lipo -info "${stage}/VST3/Super Bass Fully Agentic Dexed.vst3/Contents/MacOS/Super Bass Fully Agentic Dexed"
    lipo -info "${stage}/AU/Super Bass Fully Agentic Dexed.component/Contents/MacOS/Super Bass Fully Agentic Dexed"
    if [[ -d "${stage}/Standalone/Super Bass Fully Agentic Dexed.app" ]]; then
        lipo -info "${stage}/Standalone/Super Bass Fully Agentic Dexed.app/Contents/MacOS/Super Bass Fully Agentic Dexed"
    fi
} | tee "${stage}/architecture.txt"
grep -q x86_64 "${stage}/architecture.txt"
grep -q arm64 "${stage}/architecture.txt"

portable="${output_root}/${stage_name}.zip"
rm -f "${portable}"
ditto -c -k --keepParent "${stage}" "${portable}"

pkgroot="${output_root}/.pkgroot"
rm -rf "${pkgroot}"
mkdir -p "${pkgroot}/Library/Audio/Plug-Ins/VST3"
mkdir -p "${pkgroot}/Library/Audio/Plug-Ins/Components"
ditto "${stage}/VST3/Super Bass Fully Agentic Dexed.vst3" \
      "${pkgroot}/Library/Audio/Plug-Ins/VST3/Super Bass Fully Agentic Dexed.vst3"
ditto "${stage}/AU/Super Bass Fully Agentic Dexed.component" \
      "${pkgroot}/Library/Audio/Plug-Ins/Components/Super Bass Fully Agentic Dexed.component"
if [[ -d "${stage}/Standalone/Super Bass Fully Agentic Dexed.app" ]]; then
    mkdir -p "${pkgroot}/Applications"
    ditto "${stage}/Standalone/Super Bass Fully Agentic Dexed.app" \
          "${pkgroot}/Applications/Super Bass Fully Agentic Dexed.app"
fi
pkgbuild --root "${pkgroot}" \
    --identifier com.agenticdexed.AgenticDexed.pkg \
    --version "${version}" \
    "${output_root}/${stage_name}.pkg"
rm -rf "${pkgroot}"
printf '%s\n' "Created ${portable}" "Created ${output_root}/${stage_name}.pkg"

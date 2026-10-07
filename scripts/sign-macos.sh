#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
stage=""
output_root=""
version="1.0.1"
dry_run=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stage-root) stage="$2"; shift 2 ;;
        --output-root) output_root="$2"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

require_secret() {
    local name="$1"
    if [[ -z "${!name:-}" ]]; then
        echo "missing signing credential: ${name}" >&2
        exit 2
    fi
}
require_secret MACOS_APPLICATION_IDENTITY
require_secret MACOS_INSTALLER_IDENTITY
require_secret MACOS_NOTARY_PROFILE

if [[ ! -d "${stage}" ]]; then
    echo "Release stage does not exist: ${stage}" >&2
    exit 2
fi
output_root="${output_root:-$(dirname "${stage}")}"
if [[ ${dry_run} -eq 1 ]]; then
    echo "Dry run: macOS stage is ready for signing"
    exit 0
fi

for tool in codesign ditto pkgbuild xcrun; do
    command -v "${tool}" >/dev/null || { echo "Required macOS signing tool is missing: ${tool}" >&2; exit 2; }
done

entitlements="${repo_root}/packaging/macos/entitlements.plist"
vst3="${stage}/VST3/Super Bass Fully Agentic Dexed.vst3"
au="${stage}/AU/Super Bass Fully Agentic Dexed.component"
codesign --force --timestamp --options runtime --entitlements "${entitlements}" \
    --sign "${MACOS_APPLICATION_IDENTITY}" "${vst3}/Contents/MacOS/Super Bass Fully Agentic Dexed"
codesign --force --timestamp --options runtime --entitlements "${entitlements}" \
    --sign "${MACOS_APPLICATION_IDENTITY}" "${vst3}"
codesign --force --timestamp --options runtime --entitlements "${entitlements}" \
    --sign "${MACOS_APPLICATION_IDENTITY}" "${au}/Contents/MacOS/Super Bass Fully Agentic Dexed"
codesign --force --timestamp --options runtime --entitlements "${entitlements}" \
    --sign "${MACOS_APPLICATION_IDENTITY}" "${au}"

app="${stage}/Standalone/Super Bass Fully Agentic Dexed.app"
if [[ -d "${app}" ]]; then
    codesign --force --timestamp --options runtime --entitlements "${entitlements}" \
        --sign "${MACOS_APPLICATION_IDENTITY}" "${app}/Contents/MacOS/Super Bass Fully Agentic Dexed"
    codesign --force --timestamp --options runtime --entitlements "${entitlements}" \
        --sign "${MACOS_APPLICATION_IDENTITY}" "${app}"
fi

stage_name="$(basename "${stage}")"
portable="${output_root}/${stage_name}.zip"
rm -f "${portable}"
ditto -c -k --keepParent "${stage}" "${portable}"
xcrun notarytool submit "${portable}" --keychain-profile "${MACOS_NOTARY_PROFILE}" --wait
xcrun stapler staple "${vst3}"
xcrun stapler staple "${au}"
if [[ -d "${app}" ]]; then xcrun stapler staple "${app}"; fi
rm -f "${portable}"
ditto -c -k --keepParent "${stage}" "${portable}"

pkgroot="${output_root}/.signed-pkgroot"
case "${pkgroot}" in "${output_root}"/*) ;; *) echo "Unsafe package root" >&2; exit 2 ;; esac
rm -rf "${pkgroot}"
mkdir -p "${pkgroot}/Library/Audio/Plug-Ins/VST3"
mkdir -p "${pkgroot}/Library/Audio/Plug-Ins/Components"
ditto "${vst3}" "${pkgroot}/Library/Audio/Plug-Ins/VST3/Super Bass Fully Agentic Dexed.vst3"
ditto "${au}" "${pkgroot}/Library/Audio/Plug-Ins/Components/Super Bass Fully Agentic Dexed.component"
if [[ -d "${app}" ]]; then
    mkdir -p "${pkgroot}/Applications"
    ditto "${app}" "${pkgroot}/Applications/Super Bass Fully Agentic Dexed.app"
fi
package="${output_root}/${stage_name}.pkg"
rm -f "${package}"
pkgbuild --root "${pkgroot}" \
    --identifier com.agenticdexed.AgenticDexed.pkg \
    --version "${version}" \
    --sign "${MACOS_INSTALLER_IDENTITY}" \
    "${package}"
rm -rf "${pkgroot}"
xcrun notarytool submit "${package}" --keychain-profile "${MACOS_NOTARY_PROFILE}" --wait
xcrun stapler staple "${package}"
echo "Signed, notarized, and stapled macOS release artifacts"

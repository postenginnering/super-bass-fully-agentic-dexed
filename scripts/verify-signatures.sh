#!/usr/bin/env bash
set -euo pipefail

release_directory="${1:-}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "${release_directory}" || ! -d "${release_directory}" ]]; then
    echo "Release directory does not exist: ${release_directory}" >&2
    exit 2
fi

stage="$(find "${release_directory}" -maxdepth 1 -type d -name 'Super-Bass-Fully-Agentic-Dexed-*-macos-universal' -print -quit)"
package="$(find "${release_directory}" -maxdepth 1 -type f -name 'Super-Bass-Fully-Agentic-Dexed-*-macos-universal.pkg' -print -quit)"
if [[ -z "${stage}" || -z "${package}" ]]; then
    echo "unsigned artifact: macOS stage or package is missing" >&2
    exit 3
fi

vst3="${stage}/VST3/Super Bass Fully Agentic Dexed.vst3"
if ! codesign --verify --deep --strict --verbose=2 "${vst3}" >/dev/null 2>&1; then
    echo "unsigned artifact: ${vst3}" >&2
    exit 3
fi
xcrun stapler validate "${vst3}" >/dev/null
lipo -verify_arch x86_64 arm64 "${vst3}/Contents/MacOS/Super Bass Fully Agentic Dexed"

au="${stage}/AU/Super Bass Fully Agentic Dexed.component"
if ! codesign --verify --deep --strict --verbose=2 "${au}" >/dev/null 2>&1; then
    echo "unsigned artifact: ${au}" >&2
    exit 3
fi
xcrun stapler validate "${au}" >/dev/null
lipo -verify_arch x86_64 arm64 "${au}/Contents/MacOS/Super Bass Fully Agentic Dexed"
bash "${script_dir}/validate-au.sh" --component-path "${au}" \
    --output-dir "${release_directory}/validation/au"

app="${stage}/Standalone/Super Bass Fully Agentic Dexed.app"
if [[ -d "${app}" ]]; then
    codesign --verify --deep --strict --verbose=2 "${app}" >/dev/null 2>&1 || {
        echo "unsigned artifact: ${app}" >&2; exit 3;
    }
    spctl --assess --type execute --verbose=2 "${app}" >/dev/null
    xcrun stapler validate "${app}" >/dev/null
    lipo -verify_arch x86_64 arm64 "${app}/Contents/MacOS/Super Bass Fully Agentic Dexed"
fi

pkgutil --check-signature "${package}" >/dev/null || {
    echo "unsigned artifact: ${package}" >&2; exit 3;
}
spctl --assess --type install --verbose=2 "${package}" >/dev/null
xcrun stapler validate "${package}" >/dev/null
echo "Verified signed, notarized, stapled Universal macOS artifacts"

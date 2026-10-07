#!/usr/bin/env bash
set -euo pipefail

component=""
output_dir=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --component-path) component="$2"; shift 2 ;;
        --output-dir) output_dir="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "${component}" || ! -d "${component}" ]]; then
    echo "AU component does not exist: ${component}" >&2
    exit 2
fi
for tool in auval ditto lipo; do
    command -v "${tool}" >/dev/null || { echo "Required AU validation tool is missing: ${tool}" >&2; exit 2; }
done

output_dir="${output_dir:-$(pwd)/build/au-validation}"
mkdir -p "${output_dir}"
binary="${component}/Contents/MacOS/Super Bass Fully Agentic Dexed"
[[ -f "${binary}" ]] || { echo "AU executable is missing: ${binary}" >&2; exit 2; }
lipo -archs "${binary}" | tee "${output_dir}/architecture.log"

components_dir="${HOME}/Library/Audio/Plug-Ins/Components"
installed="${components_dir}/Super Bass Fully Agentic Dexed.component"
temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/sbfad-au-validation.XXXXXX")"
backup="${temporary_root}/existing-component-backup"
mkdir -p "${components_dir}"
restore_component() {
    rm -rf "${installed}"
    if [[ -d "${backup}" ]]; then ditto "${backup}" "${installed}"; fi
    rm -rf "${temporary_root}"
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
}
trap restore_component EXIT
if [[ -d "${installed}" ]]; then
    ditto "${installed}" "${backup}"
fi
rm -rf "${installed}"
ditto "${component}" "${installed}"
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
auval -v aumu AgDx Agnt 2>&1 | tee "${output_dir}/auval.log"
grep -q "AU VALIDATION SUCCEEDED" "${output_dir}/auval.log"
echo "Validated Super Bass Fully Agentic Dexed AU"

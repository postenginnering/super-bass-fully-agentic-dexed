cmake_minimum_required(VERSION 3.24)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(READ "${REPOSITORY_ROOT}/.github/workflows/release.yml" workflow)
string(REPLACE "\r\n" "\n" workflow "${workflow}")

function(assert_workflow_contains expected)
    string(FIND "${workflow}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Release workflow is missing: ${expected}")
    endif()
endfunction()

assert_workflow_contains("tar -czf \"agentic-dexed-macos-\${{ matrix.arch }}.tar.gz\" -C release-arch .")
assert_workflow_contains("path: agentic-dexed-macos-\${{ matrix.arch }}.tar.gz")
assert_workflow_contains("tar -xzf build-x86/agentic-dexed-macos-x86_64.tar.gz -C build-x86/release-arch")
assert_workflow_contains("tar -xzf build-arm/agentic-dexed-macos-arm64.tar.gz -C build-arm/release-arch")
assert_workflow_contains("AgenticDexedTests AgenticDexed_VST3 AgenticDexed_AU AgenticDexed_Standalone")
assert_workflow_contains("Release/AU")
assert_workflow_contains("bash ./scripts/validate-au.sh")
assert_workflow_contains("-DPACKAGE_ROOT=./dist/macos")
assert_workflow_contains("name: agentic-dexed-macos-universal-validation")
assert_workflow_contains("path: dist/macos/validation/**")
assert_workflow_contains("-P scripts/SanitizeValidationLogs.cmake")

foreach(script IN ITEMS
        packaging/macos/package.sh
        scripts/sign-macos.sh
        scripts/verify-signatures.sh)
    file(READ "${REPOSITORY_ROOT}/${script}" script_contents)
    if(NOT script_contents MATCHES "Super Bass Fully Agentic Dexed\\.component")
        message(FATAL_ERROR "${script} does not handle the AU component")
    endif()
endforeach()

file(READ "${REPOSITORY_ROOT}/packaging/macos/package.sh" package_script)
if(NOT package_script MATCHES "Library/Audio/Plug-Ins/Components")
    message(FATAL_ERROR "macOS package does not install the AU component in Components")
endif()

file(READ "${REPOSITORY_ROOT}/scripts/verify-signatures.sh" verification_script)
if(NOT verification_script MATCHES "lipo -verify_arch x86_64 arm64.*component"
   OR NOT verification_script MATCHES "validate-au\\.sh")
    message(FATAL_ERROR "macOS signature verification does not validate AU architecture and registration")
endif()

string(FIND "${workflow}" "-P scripts/SanitizeValidationLogs.cmake" sanitize_position)
string(FIND "${workflow}" "Archive sanitized validation logs" archive_position)
string(FIND "${workflow}" "bash ./scripts/scan-release.sh" scan_position)
if(sanitize_position GREATER archive_position OR archive_position GREATER scan_position)
    message(FATAL_ERROR "Validation logs must be sanitized before archiving and release scanning")
endif()

set(FIXTURE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/release-log-sanitize-fixture")
file(REMOVE_RECURSE "${FIXTURE_ROOT}")
file(MAKE_DIRECTORY "${FIXTURE_ROOT}")
set(work_segment "work")
set(windows_runner_root "D:\\a")
file(WRITE "${FIXTURE_ROOT}/pluginval.log"
    "mac=/Users/runner/${work_segment}/agentic-dexed/agentic-dexed/build/plugin.vst3\n"
    "linux=/home/runner/${work_segment}/agentic-dexed/agentic-dexed/build/plugin.vst3\n"
    "windows=${windows_runner_root}\\agentic-dexed\\agentic-dexed\\build\\plugin.vst3\n"
    "keep=validation completed\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DLOG_ROOT=${FIXTURE_ROOT}"
        -P "${REPOSITORY_ROOT}/scripts/SanitizeValidationLogs.cmake"
    RESULT_VARIABLE sanitize_result
    OUTPUT_VARIABLE sanitize_output
    ERROR_VARIABLE sanitize_error)
if(NOT sanitize_result EQUAL 0)
    file(REMOVE_RECURSE "${FIXTURE_ROOT}")
    message(FATAL_ERROR "Log sanitizer failed: ${sanitize_output}${sanitize_error}")
endif()
file(READ "${FIXTURE_ROOT}/pluginval.log" sanitized)
file(REMOVE_RECURSE "${FIXTURE_ROOT}")
if(sanitized MATCHES "/(home|Users)/runner/work/" OR sanitized MATCHES "[A-Za-z]:\\\\a\\\\")
    message(FATAL_ERROR "Log sanitizer retained a CI workspace path")
endif()
if(NOT sanitized MATCHES "<ci-workspace>/build/plugin.vst3"
   OR NOT sanitized MATCHES "keep=validation completed")
    message(FATAL_ERROR "Log sanitizer removed useful validation content")
endif()

message(STATUS "Super Bass Fully Agentic Dexed release workflow verified")

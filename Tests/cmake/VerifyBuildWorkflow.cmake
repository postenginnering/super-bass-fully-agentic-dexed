cmake_minimum_required(VERSION 3.24)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(DEFINED WORKFLOW_REVISION)
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" show "${WORKFLOW_REVISION}:.github/workflows/build.yml"
        WORKING_DIRECTORY "${REPOSITORY_ROOT}"
        RESULT_VARIABLE workflow_result
        OUTPUT_VARIABLE workflow
        ERROR_VARIABLE workflow_error
    )
    if(NOT workflow_result EQUAL 0)
        message(FATAL_ERROR "Could not read workflow at ${WORKFLOW_REVISION}: ${workflow_error}")
    endif()
else()
    file(READ "${REPOSITORY_ROOT}/.github/workflows/build.yml" workflow)
endif()

string(REPLACE "\r\n" "\n" workflow "${workflow}")

function(assert_workflow_contains expected)
    string(FIND "${workflow}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Build workflow is missing: ${expected}")
    endif()
endfunction()

assert_workflow_contains("  windows-x64:\n")
assert_workflow_contains("    runs-on: windows-2022\n")
assert_workflow_contains("  macos-arm64:\n")
assert_workflow_contains("    runs-on: macos-15\n")
assert_workflow_contains("          -DCMAKE_OSX_ARCHITECTURES=arm64\n")
assert_workflow_contains("--target AgenticDexedTests AgenticDexed_VST3 AgenticDexed_Standalone\n")
assert_workflow_contains("--target AgenticDexedTests AgenticDexed_VST3 AgenticDexed_AU AgenticDexed_Standalone\n")
assert_workflow_contains("Release/AU")
assert_workflow_contains("bash ./scripts/validate-au.sh")
assert_workflow_contains("          tar -czf agentic-dexed-macos-arm64.tar.gz\n")
assert_workflow_contains("          path: agentic-dexed-macos-arm64.tar.gz\n")

# The published product supports Windows x64 and native Apple Silicon only.
string(FIND "${workflow}" "-DCMAKE_OSX_ARCHITECTURES=x86_64" intel_position)
if(NOT intel_position EQUAL -1)
    message(FATAL_ERROR "macOS workflow must build the native Apple Silicon product")
endif()

# AU is an Apple-only target. Keep it out of the Windows job even when the
# macOS job is expanded with new plugin formats.
string(FIND "${workflow}" "  macos-arm64:\n" macos_job_position)
if(macos_job_position EQUAL -1)
    message(FATAL_ERROR "Could not locate the macOS job boundary")
endif()
string(SUBSTRING "${workflow}" 0 ${macos_job_position} windows_job)
if(windows_job MATCHES "AgenticDexed_AU")
    message(FATAL_ERROR "Windows build must not request the Apple-only AU target")
endif()
message(STATUS "Super Bass Fully Agentic Dexed build workflow verified")

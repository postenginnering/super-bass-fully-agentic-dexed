cmake_minimum_required(VERSION 3.24)

function(assert_equal variable expected)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "Expected ${variable} to be defined")
    endif()

    if(NOT "${${variable}}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "Expected ${variable}='${expected}', got '${${variable}}'")
    endif()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/AgenticDexedProduct.cmake")

assert_equal(AGENTIC_DEXED_PRODUCT_NAME "Super Bass Fully Agentic Dexed")
assert_equal(AGENTIC_DEXED_TARGET_NAME "AgenticDexed")
assert_equal(AGENTIC_DEXED_BUNDLE_ID "com.agenticdexed.AgenticDexed")
assert_equal(AGENTIC_DEXED_PLUGIN_CODE "AgDx")
assert_equal(AGENTIC_DEXED_MANUFACTURER_CODE "Agnt")
if(APPLE)
    assert_equal(AGENTIC_DEXED_FORMATS "Standalone;VST3;AU")
else()
    assert_equal(AGENTIC_DEXED_FORMATS "Standalone;VST3")
endif()
assert_equal(AGENTIC_DEXED_COPY_PLUGIN_AFTER_BUILD "OFF")

message(STATUS "Super Bass Fully Agentic Dexed product identity verified")

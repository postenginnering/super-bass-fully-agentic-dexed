include_guard(GLOBAL)

set(AGENTIC_DEXED_PRODUCT_NAME "Super Bass Fully Agentic Dexed")
set(AGENTIC_DEXED_TARGET_NAME "AgenticDexed")
set(AGENTIC_DEXED_BUNDLE_ID "com.agenticdexed.AgenticDexed")
set(AGENTIC_DEXED_PLUGIN_CODE "AgDx")
set(AGENTIC_DEXED_MANUFACTURER_CODE "Agnt")
set(AGENTIC_DEXED_FORMATS Standalone VST3)
if(APPLE)
    list(APPEND AGENTIC_DEXED_FORMATS AU)
endif()

# JUCE's RC generator only depends on its icon by default. Track product info
# too, otherwise an incremental rename leaves Windows Details/Task Manager stale.
function(agentic_refresh_windows_resource_identity target)
    if(WIN32)
        get_target_property(generated_dir ${target} JUCE_GENERATED_SOURCES_DIRECTORY)
        get_target_property(info_file ${target} JUCE_INFO_FILE)
        add_custom_command(OUTPUT "${generated_dir}/${target}_resources.rc"
            APPEND DEPENDS "${info_file}")
    endif()
endfunction()

option(
    AGENTIC_DEXED_COPY_PLUGIN_AFTER_BUILD
    "Copy Super Bass Fully Agentic Dexed into the system plug-in directory after building"
    OFF
)

set(
    CMAKE_OSX_DEPLOYMENT_TARGET
    "11.0"
    CACHE STRING
    "Minimum supported macOS version"
    FORCE
)

file(READ "${CMAKE_CURRENT_LIST_DIR}/../../README.md" readme)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../Documentation/SynthMemory.md" memory_doc)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../Documentation/BuildingAgenticDexed.md" build_doc)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../CHANGELOG.zh-CN.md" changelog)

set(combined "${readme}\n${memory_doc}\n${build_doc}\n${changelog}")
foreach(required IN ITEMS
        "自动选择性记忆"
        "所有预设"
        "每个预设"
        "synth.md"
        ".dexedpreset"
        ".syx"
        "模型数据流"
        "清除"
        "隐私")
    string(FIND "${combined}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Memory documentation is missing: ${required}")
    endif()
endforeach()

foreach(section IN ITEMS "### 新增" "### 变更" "### 安全" "### 验证")
    string(FIND "${changelog}" "${section}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Chinese changelog is missing: ${section}")
    endif()
endforeach()

foreach(stale IN ITEMS
        "The model can call `update_synth_memory`"
        "No extra summarization API request is made")
    string(FIND "${combined}" "${stale}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "Memory documentation contains obsolete behavior: ${stale}")
    endif()
endforeach()

message(STATUS "Automatic memory documentation verified")

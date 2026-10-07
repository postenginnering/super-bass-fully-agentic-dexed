juce_add_console_app(AgenticDexedTests
    PRODUCT_NAME "Super Bass Fully Agentic Dexed Tests"
)
agentic_refresh_windows_resource_identity(AgenticDexedTests)

target_sources(AgenticDexedTests PRIVATE
    TestMain.cpp
    ProcessorBaselineTests.cpp
    ParameterRegistryTests.cpp
    ParameterCoverageTests.cpp
    SynthSnapshotTests.cpp
    PatchTransactionTests.cpp
    TransactionHistoryTests.cpp
    AtomicParameterStoreTests.cpp
    StateCompatibilityTests.cpp
    SysexCompatibilityTests.cpp
    SseParserTests.cpp
    JsonAccessTests.cpp
    HttpTransportTests.cpp
    LocalHttpTestServer.cpp
    ResponsesClientTests.cpp
    ChatCompletionsClientTests.cpp
    CredentialStoreTests.cpp
    AgentPreferencesTests.cpp
    SynthMemoryTests.cpp
    ConversationSerializationTests.cpp
    SensitiveDataFilterTests.cpp
    PresetIdentityTests.cpp
    ConversationContextStoreTests.cpp
    AgentToolSchemaTests.cpp
    AgentToolDispatcherTests.cpp
    AuditionAnalyzerTests.cpp
    AgentSessionTests.cpp
    AgentLifecycleTests.cpp
    WorkbenchFoundationTests.cpp
    ParameterPageRouterTests.cpp
    SoundPageTests.cpp
    ModulationPageTests.cpp
    EffectsPageTests.cpp
    PresetLibraryServiceTests.cpp
    PresetPageTests.cpp
    SystemSettingsServiceTests.cpp
    SystemPageTests.cpp
    ParameterControlBindingTests.cpp
    AgentPanelTests.cpp
    AgentSettingsPanelTests.cpp
    GeneratePageTests.cpp
    WorkbenchShellTests.cpp
    MainEditorTests.cpp
    EditorSizingTests.cpp
    WorkbenchAccessibilityTests.cpp
    WorkbenchRenderTests.cpp
    WorkbenchUpdateBudgetTests.cpp
    UiAccessibilityTests.cpp
    UiRenderTests.cpp
    UiUpdateBudgetTests.cpp
    AgentEndToEndTests.cpp
    RealtimeSafetyTests.cpp
)

target_include_directories(AgenticDexedTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Source
    $<TARGET_PROPERTY:AgenticDexed,INCLUDE_DIRECTORIES>
)

target_compile_definitions(AgenticDexedTests PRIVATE
    AGENTIC_DEXED_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    JUCE_MODAL_LOOPS_PERMITTED=1
)

target_link_libraries(AgenticDexedTests PRIVATE
    AgenticDexed
    juce::juce_core
	juce::juce_cryptography
    juce::juce_events
)

set_target_properties(AgenticDexedTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
)

add_test(NAME agentic-dexed-unit COMMAND AgenticDexedTests)
add_test(NAME agentic-dexed-product-identity
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyProductIdentity.cmake)
set_tests_properties(agentic-dexed-unit PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME agentic-dexed-build-workflow
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBuildWorkflow.cmake
)

add_test(
    NAME agentic-dexed-release-workflow
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyReleaseWorkflow.cmake
)

add_test(
    NAME agentic-dexed-release-scanner
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyReleaseScanner.cmake
)

add_test(
    NAME agentic-dexed-macos-deployment-target
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyMacDeploymentTarget.cmake
)

add_test(
    NAME agentic-dexed-macos-source-compatibility
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyMacSourceCompatibility.cmake
)

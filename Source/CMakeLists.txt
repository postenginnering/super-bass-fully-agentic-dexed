set(BaseTargetName ${AGENTIC_DEXED_TARGET_NAME})

message(STATUS "Building ${AGENTIC_DEXED_PRODUCT_NAME} in formats: ${AGENTIC_DEXED_FORMATS}")

set(agentic_dexed_vst3_auto_manifest TRUE)
if(CMAKE_CROSSCOMPILING)
    set(agentic_dexed_vst3_auto_manifest FALSE)
endif()

juce_add_plugin("${BaseTargetName}"
        VERSION "1.0.1"
        ICON_BIG "../assets/ui/dexedIcon.png"
        ICON_SMALL "../assets/ui/dexedIcon.png"
        COMPANY_NAME "Super Bass Fully Agentic Dexed"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        PLUGIN_MANUFACTURER_CODE ${AGENTIC_DEXED_MANUFACTURER_CODE}
        PLUGIN_CODE ${AGENTIC_DEXED_PLUGIN_CODE}
        FORMATS ${AGENTIC_DEXED_FORMATS}
        PRODUCT_NAME "${AGENTIC_DEXED_PRODUCT_NAME}"
        DESCRIPTION "Super Bass Fully Agentic Dexed FM Synth"
        BUNDLE_ID "${AGENTIC_DEXED_BUNDLE_ID}"
        COPY_PLUGIN_AFTER_BUILD ${AGENTIC_DEXED_COPY_PLUGIN_AFTER_BUILD}
        VST3_AUTO_MANIFEST ${agentic_dexed_vst3_auto_manifest}
)

juce_generate_juce_header(${BaseTargetName})
agentic_refresh_windows_resource_identity(${BaseTargetName})

include_directories(../libs/MTS-ESP/Client)

target_sources(${BaseTargetName} PRIVATE
	EngineMkI.cpp
	EngineOpl.cpp
	PluginData.cpp
	PluginEditor.cpp
	PluginFx.cpp
	PluginParam.cpp
	PluginProcessor.cpp
	ui/WorkbenchTheme.cpp
	ui/WorkbenchTheme.h
	ui/WorkbenchLookAndFeel.cpp
	ui/WorkbenchLookAndFeel.h
	ui/WorkbenchControls.cpp
	ui/WorkbenchControls.h
	ui/WorkspacePage.h
	ui/WorkbenchHeader.cpp
	ui/WorkbenchHeader.h
	ui/PatchHeader.cpp
	ui/PatchHeader.h
	ui/WorkspaceTabs.cpp
	ui/WorkspaceTabs.h
	ui/PageHost.cpp
	ui/PageHost.h
	ui/WorkbenchStatusBar.cpp
	ui/WorkbenchStatusBar.h
	ui/ParameterPageRouter.cpp
	ui/ParameterPageRouter.h
	ui/OverlayHost.cpp
	ui/OverlayHost.h
	ui/UiOperationResult.h
	ui/ParameterControlBinding.cpp
	ui/ParameterControlBinding.h
	ui/ParameterTooltip.cpp
	ui/ParameterTooltip.h
	ui/AlgorithmGraph.cpp
	ui/AlgorithmGraph.h
	ui/OperatorSummaryStrip.cpp
	ui/OperatorSummaryStrip.h
	ui/SoundPage.cpp
	ui/SoundPage.h
	ui/ModulationMatrix.cpp
	ui/ModulationMatrix.h
	ui/ModulationPage.cpp
	ui/ModulationPage.h
	ui/FilterResponseDisplay.cpp
	ui/FilterResponseDisplay.h
	ui/EffectsPage.cpp
	ui/EffectsPage.h
	ui/presets/PresetLibraryService.cpp
	ui/presets/PresetLibraryService.h
	ui/presets/ProgramGrid.cpp
	ui/presets/ProgramGrid.h
	ui/presets/PresetPage.cpp
	ui/presets/PresetPage.h
	ui/system/SystemSettingsService.cpp
	ui/system/SystemSettingsService.h
	ui/system/TuningTableView.cpp
	ui/system/TuningTableView.h
	ui/system/MidiLearnOverlay.cpp
	ui/system/MidiLearnOverlay.h
	ui/system/SystemPage.cpp
	ui/system/SystemPage.h
	ui/EnvelopeDisplay.cpp
	ui/EnvelopeDisplay.h
	ui/AgentConsole.cpp
	ui/AgentConsole.h
	ui/ChangeSetView.cpp
	ui/ChangeSetView.h
	ui/HistoryView.cpp
	ui/HistoryView.h
	ui/AgentPanel.cpp
	ui/AgentPanel.h
	ui/GeneratePage.cpp
	ui/GeneratePage.h
	ui/SecretField.cpp
	ui/SecretField.h
	ui/AgentSettingsPanel.cpp
	ui/AgentSettingsPanel.h
	ui/AuditionBar.cpp
	ui/AuditionBar.h
	ui/MainEditor.cpp
	ui/MainEditor.h
	state/ParameterRegistry.cpp
	state/ParameterRegistry.h
	state/ParameterDefinition.h
	state/ParameterValue.h
	state/ParameterCoverage.cpp
	state/ParameterCoverage.h
	state/SynthSnapshot.h
	state/ISynthStateBackend.h
	state/DexedParameterBackend.cpp
	state/DexedParameterBackend.h
	state/SynthStateService.cpp
	state/SynthStateService.h
	state/PatchTransactionService.cpp
	state/PatchTransactionService.h
	state/TransactionHistory.cpp
	state/TransactionHistory.h
	state/AtomicParameterStore.cpp
	state/AtomicParameterStore.h
	state/RealtimeSynthState.h
	agent/AgentLimits.h
	agent/AgentTypes.h
	agent/SseParser.cpp
	agent/SseParser.h
	agent/JsonAccess.cpp
	agent/JsonAccess.h
	agent/http/HttpTypes.h
	agent/http/IHttpTransport.h
	agent/http/BaseUrlPolicy.cpp
	agent/http/BaseUrlPolicy.h
	agent/http/JuceHttpTransport.cpp
	agent/http/JuceHttpTransport.h
	agent/model/ModelTypes.h
	agent/model/IModelClient.h
	agent/model/ModelClientSupport.cpp
	agent/model/ModelClientSupport.h
	agent/model/ResponsesClient.cpp
	agent/model/ResponsesClient.h
	agent/model/ChatCompletionsClient.cpp
	agent/model/ChatCompletionsClient.h
	agent/AgentPreferences.cpp
	agent/AgentPreferences.h
	agent/AgentController.cpp
	agent/AgentController.h
	agent/session/AgentPrompt.cpp
	agent/memory/SynthMemory.cpp
	agent/memory/SynthMemory.h
	agent/context/ConversationTypes.h
	agent/context/ConversationSerialization.cpp
	agent/context/ConversationSerialization.h
	agent/session/AgentPrompt.h
	agent/session/AgentSession.cpp
	agent/session/AgentSession.h
	security/SecureSecret.h
	security/CredentialStore.cpp
	security/CredentialStore.h
	agent/tools/AgentToolSchemas.cpp
	agent/tools/AgentToolSchemas.h
	agent/tools/AgentToolTypes.h
	agent/tools/AgentToolDispatcher.cpp
	agent/tools/AgentToolDispatcher.h
	audition/AuditionTypes.h
	audition/IAuditionService.h
	audition/OfflinePatchRenderer.cpp
	audition/OfflinePatchRenderer.h
	audition/AuditionAnalyzer.cpp
	audition/AuditionAnalyzer.h
	SysexComm.cpp
        msfa/dx7note.cc
        msfa/env.cc
        msfa/exp2.cc
        msfa/fm_core.cc
        msfa/fm_op_kernel.cc
        msfa/freqlut.cc
        msfa/lfo.cc
        msfa/pitchenv.cc
        msfa/sin.cc
        msfa/tuning.cc
        msfa/porta.cpp

    # until MTS-ESP has is own cmake project
    ../libs/MTS-ESP/Client/libMTSClient.cpp
)

if(WIN32)
    target_sources(${BaseTargetName} PRIVATE security/CredentialStore_windows.cpp)
    target_link_libraries(${BaseTargetName} PRIVATE Advapi32)
elseif(APPLE)
    target_sources(${BaseTargetName} PRIVATE security/CredentialStore_mac.mm)
    find_library(AGENTIC_DEXED_SECURITY_FRAMEWORK Security REQUIRED)
    target_link_libraries(${BaseTargetName} PRIVATE ${AGENTIC_DEXED_SECURITY_FRAMEWORK})
endif()

if(DEXED_NO_ALSA)
    set(DEXED_ALSA FALSE)
    message(STATUS "Building Dexed without ALSA support")
else()
    set(DEXED_ALSA TRUE)
endif()

target_compile_definitions(${BaseTargetName} PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_JACK=1
    JUCE_ALSA=$<IF:$<BOOL:${DEXED_ALSA}>,1,0>
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    # JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1     # REVERT BACK SINCE THERE IS AN ISSUE WITH JUCE 7.0.0
)

target_link_libraries(${BaseTargetName} 
PRIVATE
    clap_juce_extensions
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_plugin_client
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    tuning-library
    DexedResources
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
)

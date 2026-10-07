/**
 *
 * Copyright (c) 2013-2025 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#ifndef PLUGINPROCESSOR_H_INCLUDED
#define PLUGINPROCESSOR_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"

#include "clap-juce-extensions/clap-juce-extensions.h"

#include "msfa/controllers.h"
#include "msfa/dx7note.h"
#include "msfa/lfo.h"
#include "msfa/synth.h"
#include "msfa/fm_core.h"
#include "msfa/tuning.h"
#include "PluginParam.h"
#include "PluginData.h"
#include "PluginFx.h"
#include "SysexComm.h"
#include "EngineMkI.h"
#include "EngineOpl.h"
#include "state/AtomicParameterStore.h"
#include "state/RealtimeSynthState.h"
#include "ui/UiOperationResult.h"

#include <limits>
#include <atomic>
#include <map>
#include <vector>

namespace agentic_dexed
{
class DexedParameterBackend;
class ParameterRegistry;
class SynthStateService;
}

namespace agentic_dexed::agent { class AgentController; }

struct AgenticEditorPreferences
{
    int width = 1280;
    int height = 760;
    int scalePercent = 100;
    bool agentExpanded = true;
    bool keyboardExpanded = true;
    bool reducedMotion = false;
    int selectedPage = 0;
};

struct ProcessorVoice {
    int channel;
    int midi_note;
    int velocity;
    bool keydown;
    bool sustained;
    bool live;
    int32_t keydown_seq;

    int mpePitchBend;
    Dx7Note *dx7_note;
};

enum DexedEngineResolution {
    DEXED_ENGINE_MODERN,
    DEXED_ENGINE_MARKI,
    DEXED_ENGINE_OPL
};

/// Maximum allowed size for SCL and KBM files.
/// (COMMENT: Since none of the 5175 .scl files in the Scala archive
/// at https://www.huygens-fokker.org/scala/downloads.html#scales
/// exceed 6 KB (in 25th Mar 2024), a maximum size of 16 KB appears
/// to be a practical choice.)
const int MAX_SCL_KBM_FILE_SIZE = 16384;

//==============================================================================
/**
*/
class DexedAudioProcessor  : public AudioProcessor, public AsyncUpdater, public MidiInputCallback, public clap_juce_extensions::clap_properties
{
    static const int MAX_ACTIVE_NOTES = 16;
    ProcessorVoice voices[MAX_ACTIVE_NOTES];
    int currentNote;

    // The original DX7 had one single LFO. Later units had an LFO per note.
    Lfo lfo;

    bool sustain;
    bool monoMode;
    
    // Extra buffering for when GetSamples wants a buffer not a multiple of N
    float extra_buf[N];
    int extra_buf_size;

    int currentProgram;
    
    /**
     * The last time the state was save, to be able to bypass a VST host bug.
     */
    long lastStateSave;
    
    /**
     * Plugin fx (the filter)
     */
    PluginFx fx;

    /**
     * This flag is used in the audio thread to know if the voice has changed
     * and needs to be updated.
     */
    bool refreshVoice;
    bool normalizeDxVelocity;
    bool sendSysexChange;
    
    void processMidiMessage(const MidiMessage *msg);
    void keydown(uint8_t chan, uint8_t pitch, uint8_t velo);
    void keyup(uint8_t, uint8_t pitch, uint8_t velo);
    
    /**
     * this is called from the Audio thread to tell
     * to update the UI / hostdata 
     */
    void handleAsyncUpdate() override;
    void initCtrl(bool backgroundOnly);
    void setupBuiltinCart();

	MidiMessage* nextMidi,*midiMsg;
	bool hasMidiMessage;
    int midiEventPos;
	bool getNextEvent(MidiBuffer::Iterator* iter,const int samplePos);
    
    void handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message) override;
    uint32_t engineType;
    
    FmCore engineMsfa;
    EngineMkI engineMkI;
    EngineOpl engineOpl;

    void resolvAppDir();
    
    void unpackOpSwitch(char packOpValue);
    void packOpSwitch();

    float zoomFactor = 1;

    std::unique_ptr<agentic_dexed::ParameterRegistry> agenticParameterRegistry_;
    std::unique_ptr<agentic_dexed::AtomicParameterStore> agenticParameterStore_;
    std::unique_ptr<agentic_dexed::DexedParameterBackend> agenticParameterBackend_;
    std::unique_ptr<agentic_dexed::SynthStateService> agenticStateService_;
    std::unique_ptr<agentic_dexed::agent::AgentController> agenticAgentController_;
    agentic_dexed::RealtimeSynthState realtimeSynthState_;
    uint64_t appliedRealtimeRevision_ { std::numeric_limits<uint64_t>::max() };
    double engineSampleRate_ { 48'000.0 };
    static thread_local bool suppressAtomicHostWrite_;
    std::unique_ptr<Logger> ownedDebugLogger_;

    void consumeRealtimeStateAtBlockBoundary() noexcept;
    void publishLegacyStateToRealtimeStore();
    void applyEngineTypeToLegacy(int engine);

public :
    // in MIDI units (0x4000 is neutral)
    Controllers controllers;
    StringArray programNames;
    Cartridge currentCart;
    uint8_t data[161];

    SysexComm sysexComm;
    VoiceStatus voiceStatus;
    File activeFileCartridge;
    
    bool forceRefreshUI;
    float vuSignal;
    double vuDecayFactor = 0.999361; // (for 48 kHz sampling rate)
    bool showKeyboard;
    AgenticEditorPreferences agenticEditorPreferences;
    int getEngineType() const;
    void setEngineType(int rs);
    void publishPerformanceConfiguration(
        const Controllers& configuration, int selectedEngine);

    float agenticHostParameterNormalized(int hostIndex) const;
    void setAgenticHostParameterNormalized(int hostIndex, float normalized);
    agentic_dexed::AtomicBatchResult publishAgenticRealtimeBatch(
        const std::vector<agentic_dexed::NormalizedChange>& hostChanges,
        const agentic_dexed::RealtimeAuxiliaryChanges& auxiliary);
    agentic_dexed::AtomicBatchResult tryPublishAgenticRealtimeBatch(
        uint64_t baseRevision,
        const std::vector<agentic_dexed::NormalizedChange>& hostChanges,
        const agentic_dexed::RealtimeAuxiliaryChanges& auxiliary) noexcept;
    bool readAgenticRealtimeState(
        agentic_dexed::RealtimeSynthState& destination) const noexcept;
    agentic_dexed::AtomicParameterStore& atomicParameterStore() noexcept;
    const agentic_dexed::AtomicParameterStore& atomicParameterStore() const noexcept;
    bool hasAtomicParameterStore() const noexcept
    {
        return agenticParameterStore_ != nullptr;
    }
    const Controllers& agenticControllers() const noexcept { return controllers; }
    Controllers& agenticControllers() noexcept { return controllers; }
    bool agenticVelocityNormalizationEnabled() const noexcept { return normalizeDxVelocity; }
    void setAgenticVelocityNormalizationEnabled(bool enabled) noexcept { normalizeDxVelocity = enabled; }
    std::string agenticPatchName() const;
    void setAgenticPatchName(const std::string& name);
    const std::string& agenticSclData() const noexcept { return currentSCLData; }
    const std::string& agenticKbmData() const noexcept { return currentKBMData; }
    bool agenticTuningDataIsValid(
        const std::string& sclData, const std::string& kbmData) const;
    bool setAgenticTuningData(
        const std::string& sclData, const std::string& kbmData);
    agentic_dexed::SynthStateService& synthStateService() noexcept;
    const agentic_dexed::SynthStateService& synthStateService() const noexcept;
    const agentic_dexed::ParameterRegistry& parameterRegistry() const noexcept;
    agentic_dexed::agent::AgentController& agentController() noexcept;
    const agentic_dexed::agent::AgentController& agentController() const noexcept;
    bool hasAgentController() const noexcept { return agenticAgentController_ != nullptr; }
    bool lastAgentContextImportSucceeded() const noexcept
    {
        return lastAgentContextImportOk_.load(std::memory_order_acquire);
    }
    
    HashMap<int, Ctrl*> mappedMidiCC;
    std::map<int, std::string> agenticMidiCCMappings;
    
    Array<Ctrl*> ctrl;

    OperatorCtrl opCtrl[6];
    std::unique_ptr<CtrlDX> pitchEgRate[4];
    std::unique_ptr<CtrlDX> pitchEgLevel[4];
    std::unique_ptr<CtrlDX> pitchModSens;
    std::unique_ptr<CtrlDX> algo;
    std::unique_ptr<CtrlDX> oscSync;
    std::unique_ptr<CtrlDX> feedback;
    std::unique_ptr<CtrlDX> lfoRate;
    std::unique_ptr<CtrlDX> lfoDelay;
    std::unique_ptr<CtrlDX> lfoAmpDepth;
    std::unique_ptr<CtrlDX> lfoPitchDepth;
    std::unique_ptr<CtrlDX> lfoWaveform;
    std::unique_ptr<CtrlDX> lfoSync;
    std::unique_ptr<CtrlDX> transpose;

    std::unique_ptr<CtrlFloat> fxCutoff;
    std::unique_ptr<CtrlFloat> fxReso;
    std::unique_ptr<CtrlFloat> output;
    std::unique_ptr<Ctrl> tune;
    std::unique_ptr<Ctrl> monoModeCtrl;

    void loadCartridge(Cartridge &cart);
    void setDxValue(int offset, int v);

    //==============================================================================
    explicit DexedAudioProcessor(bool backgroundOnly = false);
    ~DexedAudioProcessor();

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (AudioSampleBuffer& buffer, MidiBuffer& midiMessages) override;
    void panic();
    bool isMonoMode() {
        return monoMode;
    }
    void setMonoMode(bool mode);
    
    void copyToClipboard(int srcOp);
    void pasteOpFromClipboard(int destOp);
    void pasteEnvFromClipboard(int destOp);
    void sendCurrentSysexProgram();
    void sendCurrentSysexCartridge();
    agentic_dexed::ui::UiOperationResult sendSysexCartridge(File cart);
    
    //==============================================================================
    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    void updateUI();
    bool peekVoiceStatus();
    int updateProgramFromSysex(const uint8 *rawdata);
    void setupStartupCart();
    
    //==============================================================================
    const String getName() const override;
    int getNumParameters() override;
    float getParameter (int index) override;
    void setParameter (int index, float newValue) override;
    const String getParameterName (int index) override;
    const String getParameterText (int index) override;
    String getParameterID (int index) override;

    const String getInputChannelName (int channelIndex) const override;
    const String getOutputChannelName (int channelIndex) const override;
    bool isInputChannelStereoPair (int index) const override;
    bool isOutputChannelStereoPair (int index) const override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool silenceInProducesSilenceOut() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const String getProgramName (int index) override;
    void changeProgramName(int index, const String& newName) override;
    void resetToInitVoice() ;
    
    //==============================================================================
    void getStateInformation (MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    
    // this is kept up to date with the midi messages that arrive, and the UI component
    // registers with it so it can represent the incoming messages
    MidiKeyboardState keyboardState;
    void unbindUI();

    void loadPreference();
    void savePreference();
    
    static File dexedAppDir;
    static File dexedCartDir;

    Value lastCCUsed;
    int lastActiveVoice = 0;

    MTSClient *mtsClient;
    std::shared_ptr<TuningState> synthTuningState;

    // holds the previous working tuning state;
    // used to restore tuning state when there was a problem 
    // with loading/applying a new .SCL and/or .KBM file 
    std::shared_ptr<TuningState> synthTuningStateLast;

    // Load a file
    agentic_dexed::ui::UiOperationResult applySCLTuning(File sclf);
    agentic_dexed::ui::UiOperationResult applyKBMMapping(File kbmf);

    // Load from text
    agentic_dexed::ui::UiOperationResult applySCLTuning(std::string scld);
    agentic_dexed::ui::UiOperationResult applyKBMMapping(std::string kbmd);
    
    void retuneToStandard();
    void resetTuning(std::shared_ptr<TuningState> t);
    int tuningTranspositionShift();
    
    std::string currentSCLData = "";
    std::string currentKBMData = "";
    void setZoomFactor(float factor);
    float getZoomFactor() {
        return zoomFactor;
    }    
private:
    std::atomic_bool lastAgentContextImportOk_ { true };
    int chooseNote(uint8_t pitch);
    int32_t nextKeydownSeq;;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DexedAudioProcessor)

};

#endif  // PLUGINPROCESSOR_H_INCLUDED

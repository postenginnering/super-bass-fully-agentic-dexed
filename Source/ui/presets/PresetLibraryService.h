#pragma once

#include "../../PluginData.h"
#include "../../agent/context/PresetIdentityService.h"
#include "../UiOperationResult.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class DexedAudioProcessor;

namespace agentic_dexed::ui
{
struct PresetSlot
{
    int index {};
    juce::String name;
    bool selected {};
    bool current {};
};

struct Dx7NamePreview
{
    juce::String requested;
    juce::String normalized;
    std::array<uint8_t, 10> bytes {};
    bool changed {};
};

class PresetLibraryService final
{
public:
    using Completion = std::function<void(UiOperationResult)>;

    explicit PresetLibraryService(
        DexedAudioProcessor&,
        std::shared_ptr<agent::context::PresetIdentityService> identities = {});
    ~PresetLibraryService();

    std::vector<PresetSlot> activeSlots() const;
    std::vector<PresetSlot> browserSlots() const;
    const juce::Array<juce::File>& recentCartridges() const noexcept;
    const juce::File& userDirectory() const noexcept { return userDirectory_; }
    const juce::File& browserFile() const noexcept { return browserFile_; }

    UiOperationResult setUserDirectory(const juce::File&);
    UiOperationResult refreshBrowser();
    UiOperationResult openBrowserCartridge(const juce::File&);
    void openBrowserCartridgeAsync(juce::File, Completion);
    UiOperationResult activateActiveSlot(int);
    UiOperationResult activateBrowserSlot(int);
    UiOperationResult copyBrowserToActive(int source, int destination);
    UiOperationResult moveActiveSlot(int source, int destination);
    Dx7NamePreview previewDx7Name(juce::String requested) const;
    UiOperationResult renameActiveSlot(int, const Dx7NamePreview&);
    UiOperationResult storeCurrentProgram(int destination,
                                          const Dx7NamePreview&);
    UiOperationResult initializeCurrentProgram();
    void adoptCurrentAgentContext();
    UiOperationResult createActiveCartridge();
    UiOperationResult saveActiveCartridge(const juce::File&, bool overwrite);
    void saveActiveCartridgeAsync(juce::File, bool overwrite, Completion);

private:
    static bool validSlot(int index) noexcept { return index >= 0 && index < 32; }
    static UiOperationResult success(juce::String message);
    static UiOperationResult failure(juce::String message,
                                     bool overwrite = false);
    UiOperationResult commitBrowserCartridge(const juce::File&, const Cartridge&);
    void remember(const juce::File&);
    std::optional<agent::context::PresetActivation> activationForActiveSlot(int);
    std::optional<agent::context::PresetActivation> freshActivation();
    bool activateAgentContext(const agent::context::PresetActivation&);
    void captureCurrentFingerprint();
    void cancelAgentRequest();

    DexedAudioProcessor& processor_;
    std::shared_ptr<agent::context::PresetIdentityService> identities_;
    Cartridge browserCart_;
    bool hasBrowserCart_ {};
    int browserSelection_ { -1 };
    juce::File browserFile_;
    juce::File userDirectory_;
    juce::Array<juce::File> recentCartridges_;
    std::shared_ptr<std::atomic_bool> alive_ {
        std::make_shared<std::atomic_bool>(true)
    };
    juce::ThreadPool ioPool_ { juce::ThreadPool::Options {}
        .withNumberOfThreads(1)
        .withThreadName("Preset cartridge I/O") };
};
}

#pragma once

#include <JuceHeader.h>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentic_dexed::agent::context {

using PresetId = std::string;
using CanonicalVoice = std::array<std::uint8_t, 155>;

struct PresetActivation {
    PresetId presetId;
    std::string fingerprint;
    bool created = false;
};

class PresetIdentityService {
public:
    explicit PresetIdentityService(juce::File indexFile);

    PresetActivation activateVoice(const CanonicalVoice& voice);
    std::optional<PresetId> createPreset();
    bool adoptPreset(const PresetId& presetId, const CanonicalVoice& voice);
    bool updateFingerprint(const PresetId& presetId, const CanonicalVoice& voice);
    std::optional<PresetId> clonePreset(const PresetId& source);
    std::optional<PresetId> clonedFrom(const PresetId& presetId) const;

    bool bindSlot(int slot, const PresetId& presetId);
    bool moveSlot(int source, int destination);
    bool renamePreset(const PresetId& presetId) const;
    std::optional<PresetId> presetForSlot(int slot) const;

    juce::var exportIndex() const;
    bool importIndex(const juce::var& value);
    bool save() const;

    static std::string fingerprint(const CanonicalVoice& voice);

private:
    struct Record {
        PresetId clonedFrom;
        std::vector<std::string> fingerprints;
    };

    struct State {
        std::unordered_map<PresetId, Record> records;
        std::unordered_map<std::string, PresetId> fingerprintToPreset;
        std::map<int, PresetId> slots;
    };

    static std::optional<State> parseIndex(const juce::var& value);
    juce::var exportIndexUnlocked() const;
    bool saveUnlocked() const;
    void loadFromDisk();

    juce::File indexFile_;
    mutable std::mutex mutex_;
    State state_;
};

} // namespace agentic_dexed::agent::context

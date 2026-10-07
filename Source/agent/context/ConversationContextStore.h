#pragma once

#include "ConversationSerialization.h"
#include "PresetIdentityService.h"
#include "../memory/SynthMemory.h"

#include <JuceHeader.h>
#include <string>
#include <string_view>

namespace agentic_dexed::agent::context {

struct ContextLoadResult {
    bool ok = true;
    bool exists = false;
    PresetConversationContext context;
    std::string error;
};

enum class ContextCommitStatus { committed, conflict, error };

struct ContextCommitResult {
    ContextCommitStatus status = ContextCommitStatus::error;
    std::uint64_t currentVersion = 0;
    std::string error;
};

class ConversationContextStore {
public:
    explicit ConversationContextStore(juce::File rootDirectory = defaultRoot());

    static juce::File defaultRoot();
    ContextLoadResult load(const PresetId&) const;
    ContextCommitResult commit(PresetConversationContext, std::uint64_t expectedVersion,
                               std::string_view exactCredential = {});
    ContextCommitResult appendTurn(const PresetId&, ConversationTurn,
                                   std::uint64_t expectedVersion,
                                   std::string_view exactCredential = {});

    memory::MemoryResult loadPreferences(std::string_view exactCredential = {}) const;
    memory::MemoryResult applyPreferenceDiff(const memory::ValidatedPreferenceDiff&,
                                             std::string_view exactCredential = {});

private:
    juce::File contextFile(const PresetId&) const;
    ContextLoadResult loadUnlocked(const PresetId&, const juce::File&) const;
    ContextCommitResult writeUnlocked(PresetConversationContext, const juce::File&) const;
    static bool validPresetId(const PresetId&);
    static bool sanitize(PresetConversationContext&, std::string_view exactCredential,
                         std::string& error);

    juce::File rootDirectory_;
    memory::SynthMemory preferences_;
};

} // namespace agentic_dexed::agent::context

#pragma once

#include "ConversationTypes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <optional>
#include <string>

namespace agentic_dexed::agent::context {

inline constexpr std::array<char, 8> kPortableContextMagic {
    'S', 'B', 'F', 'A', 'D', 'C', 'T', 'X'
};
inline constexpr int kPortableContextEnvelopeVersion = 1;

struct PortableContextResult {
    bool ok = false;
    std::optional<PresetConversationContext> context;
    std::string error;
};

juce::MemoryBlock encodePortableContext(const PresetConversationContext& context);
PortableContextResult decodePortableContext(const juce::MemoryBlock& encoded);
std::string portableContextSha256(const juce::MemoryBlock& encoded);

} // namespace agentic_dexed::agent::context

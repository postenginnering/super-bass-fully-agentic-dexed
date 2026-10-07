#pragma once

#include "ConversationTypes.h"
#include <JuceHeader.h>
#include <optional>
#include <string>
#include <string_view>

namespace agentic_dexed::agent::context {

struct ContextParseResult {
    std::optional<PresetConversationContext> context;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return context.has_value(); }
};

juce::var serializeContext(const PresetConversationContext& context);
ContextParseResult parseContext(const juce::var& value);
ContextParseResult parseContextJson(std::string_view bytes);

} // namespace agentic_dexed::agent::context

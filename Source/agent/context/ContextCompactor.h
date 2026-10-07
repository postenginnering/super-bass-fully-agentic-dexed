#pragma once

#include "ConversationTypes.h"
#include "../model/ModelTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentic_dexed::agent::context {

inline constexpr std::size_t kContextCompactionTriggerBytes = 160u * 1024u;
inline constexpr std::size_t kContextCompactionRecentTurns = 8u;

struct ContextCompactionResult {
    bool ok = false;
    std::optional<PresetConversationContext> context;
    std::string error;
};

class ContextCompactor {
public:
    static bool shouldCompact(const PresetConversationContext&);
    static std::vector<model::ModelMessage> buildRequest(
        const PresetConversationContext&);
    static ContextCompactionResult parseResponse(
        std::string_view response,
        const PresetConversationContext& source,
        std::string_view exactCredential = {});
};

} // namespace agentic_dexed::agent::context

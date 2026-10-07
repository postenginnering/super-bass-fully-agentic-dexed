#pragma once

#include "SynthMemory.h"
#include "../context/ConversationTypes.h"
#include "../model/ModelTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentic_dexed::agent::memory {

struct PreferenceCurationResult {
    bool ok = false;
    std::optional<ValidatedPreferenceDiff> diff;
    std::string error;
};

class PreferenceCurator {
public:
    static std::vector<model::ModelMessage> buildRequest(
        const context::ConversationTurn& turn,
        std::string_view existingPreferences);

    static PreferenceCurationResult parseResponse(
        std::string_view response,
        const context::ConversationTurn& turn,
        std::string_view exactCredential = {});
};

} // namespace agentic_dexed::agent::memory

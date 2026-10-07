#pragma once

#include <string>
#include <string_view>

namespace agentic_dexed::agent::memory {

struct SanitizedText {
    std::string text;
    bool redacted = false;
    std::string diagnostic;
};

class SensitiveDataFilter {
public:
    static SanitizedText sanitizeForContext(
        std::string_view input, std::string_view exactCredential = {});
    static bool acceptPreference(
        std::string_view input, std::string_view exactCredential = {});
};

} // namespace agentic_dexed::agent::memory

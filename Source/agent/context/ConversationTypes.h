#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agentic_dexed::agent::context {

inline constexpr int kPresetContextVersion = 1;
inline constexpr std::size_t kMaximumMessageBytes = 48u * 1024u;
inline constexpr std::size_t kMaximumSummaryBytes = 24u * 1024u;
inline constexpr std::size_t kMaximumContextBytes = 2u * 1024u * 1024u;
inline constexpr std::size_t kMaximumRecentTurns = 32u;

enum class ConversationRole { user, assistant, toolCall, toolResult };
enum class TurnTerminalState { completed, failed, cancelled };

struct ConversationMessage {
    ConversationRole role = ConversationRole::user;
    std::string text;
    std::string callId;
    std::string toolName;
    bool success = true;
};

struct TransactionSummary {
    std::string transactionId;
    std::string summary;
    std::uint64_t resultingRevision = 0;
};

struct ConversationTurn {
    std::string turnId;
    TurnTerminalState terminalState = TurnTerminalState::completed;
    std::vector<ConversationMessage> messages;
    std::vector<TransactionSummary> transactions;
};

struct PresetConversationContext {
    int version = kPresetContextVersion;
    std::string presetId;
    std::uint64_t revision = 0;
    std::string summary;
    std::vector<ConversationTurn> recentTurns;
    std::vector<std::string> fingerprintAliases;
    std::int64_t updatedAtUnixMs = 0;
};

class PresetContextView {
public:
    PresetContextView() = default;
    explicit PresetContextView(std::shared_ptr<const PresetConversationContext> context)
        : context_(std::move(context)) {}

    [[nodiscard]] bool empty() const noexcept { return context_ == nullptr; }
    [[nodiscard]] const PresetConversationContext& get() const { return *context_; }
    [[nodiscard]] const PresetConversationContext* operator->() const noexcept { return context_.get(); }
    [[nodiscard]] std::shared_ptr<const PresetConversationContext> snapshot() const noexcept { return context_; }

private:
    std::shared_ptr<const PresetConversationContext> context_;
};

} // namespace agentic_dexed::agent::context

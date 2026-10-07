#pragma once

#include "ConversationContextStore.h"
#include "PortablePresetContext.h"
#include "../model/ModelTypes.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentic_dexed::agent::context {

inline constexpr std::size_t kManagedRequestBudgetBytes = 224u * 1024u;
inline constexpr std::size_t kProtocolReserveBytes = 32u * 1024u;
inline constexpr std::size_t kMinimumRecentTurns = 8u;

struct NaturalHistoryEntry {
    ConversationRole role = ConversationRole::user;
    std::string text;
};

struct ContextBuildResult {
    bool ok = true;
    std::string error;
    std::vector<model::ModelMessage> messages;
    std::vector<NaturalHistoryEntry> history;
    PresetContextView context;
    std::size_t assembledBytes = 0;
    bool needsCompaction = false;
};

struct TerminalTurn {
    PresetId presetId;
    std::uint64_t expectedVersion = 0;
    ConversationTurn turn;
    model::ProviderConfig provider;
    std::string credentialId;
};

class ITurnSink {
public:
    virtual ~ITurnSink() = default;
    virtual void onTurnFinished(TerminalTurn turn) = 0;
};

class ContextManager final : public ITurnSink {
public:
    explicit ContextManager(std::shared_ptr<ConversationContextStore> store);

    ContextBuildResult buildRequestContext(
        const PresetId& presetId,
        std::string currentRequest,
        std::string primarySystemPrompt = {}) const;
    PresetContextView loadConversation(const PresetId& presetId) const;
    PresetContextView cachedConversation(const PresetId& presetId) const;
    juce::MemoryBlock portableSnapshot(const PresetId& presetId) const;
    bool installPortableContext(PresetConversationContext context,
                                juce::MemoryBlock encoded);
    bool persistConversation(PresetConversationContext context);
    void onTurnFinished(TerminalTurn turn) override;

    static std::size_t modelMessageBytes(const model::ModelMessage& message);

private:
    void cache(PresetConversationContext context,
               std::optional<juce::MemoryBlock> encoded = std::nullopt) const;

    std::shared_ptr<ConversationContextStore> store_;
    mutable std::mutex cacheMutex_;
    mutable std::unordered_map<PresetId,
        std::pair<std::shared_ptr<const PresetConversationContext>,
                  std::shared_ptr<const juce::MemoryBlock>>> cache_;
};

} // namespace agentic_dexed::agent::context

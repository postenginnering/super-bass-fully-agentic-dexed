#pragma once

#include "../AgentPreferences.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agentic_dexed::agent::model { class IModelClient; }
namespace agentic_dexed::agent::tools { class AgentToolDispatcher; }
namespace agentic_dexed::security { class ICredentialStore; }
namespace agentic_dexed::agent::memory { class SynthMemory; }
namespace agentic_dexed::agent::context {
class ContextManager;
class ITurnSink;
class PresetContextView;
}

namespace agentic_dexed::agent::session
{
enum class AgentSessionState
{
    idle,
    requesting,
    streaming,
    executingTool,
    awaitingConfirmation,
    completed,
    cancelled,
    failed
};

enum class AgentTranscriptKind
{
    user,
    assistant,
    toolCall,
    toolResult,
    status
};

struct AgentTranscriptEntry
{
    AgentTranscriptKind kind = AgentTranscriptKind::status;
    std::string text;
    std::string callId;
    std::string toolName;
    bool success = true;
};

struct AgentTransactionSummary
{
    std::string transactionId;
    std::string reason;
    std::string status;
    uint64_t baseRevision = 0;
    uint64_t resultingRevision = 0;
};

struct AgentSessionSnapshot
{
    AgentSessionState state = AgentSessionState::idle;
    std::string streamingText;
    std::string finalText;
    std::string errorCode;
    std::string errorMessage;
    std::string pendingProposalId;
    int toolIterations = 0;
    bool limitReached = false;
    std::vector<AgentTranscriptEntry> transcript;
    std::vector<AgentTransactionSummary> transactions;
};

struct UserAgentRequest
{
    std::string prompt;
    std::string presetId;
    std::string credentialId = "agent.model";
    AgentPreferences preferences;
};

class AgentSessionListener
{
public:
    virtual ~AgentSessionListener() = default;
    virtual void agentSessionChanged(const AgentSessionSnapshot& snapshot) = 0;
};

class AgentSession
{
public:
    AgentSession(
        model::IModelClient& modelClient,
        tools::AgentToolDispatcher& toolDispatcher,
        security::ICredentialStore& credentialStore,
        std::shared_ptr<memory::SynthMemory> memory = {},
        std::shared_ptr<context::ContextManager> contextManager = {},
        std::shared_ptr<context::ITurnSink> turnSink = {});
    ~AgentSession();

    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;

    void start(UserAgentRequest request);
    void loadConversation(context::PresetContextView view);
    void confirmProposal(std::string proposalId);
    void cancel() noexcept;

    void addListener(AgentSessionListener* listener);
    void removeListener(AgentSessionListener* listener);

    [[nodiscard]] AgentSessionSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

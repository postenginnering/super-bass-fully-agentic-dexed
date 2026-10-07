#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <juce_core/juce_core.h>

namespace agentic_dexed
{
class ParameterRegistry;
class SynthStateService;
}

namespace agentic_dexed::security
{
class ICredentialStore;
class CredentialSession;
}

namespace agentic_dexed::agent::http
{
class IHttpTransport;
class IRequestHandle;
}

namespace agentic_dexed::agent::model
{
struct ProviderConfig;
}

namespace agentic_dexed::agent::session
{
class AgentSession;
class AgentSessionListener;
struct AgentSessionSnapshot;
struct UserAgentRequest;
}

namespace agentic_dexed::agent
{
struct ConnectionTestResult
{
    bool success = false;
    std::string sanitizedMessage;
    std::chrono::milliseconds latency { 0 };
};

using ConnectionTestCallback = std::function<void(ConnectionTestResult)>;
using SaveRequestCallback = std::function<bool(std::string validatedName)>;

class AgentController
{
public:
    AgentController(
        const ParameterRegistry& registry,
        SynthStateService& stateService);
    AgentController(
        const ParameterRegistry& registry,
        SynthStateService& stateService,
        std::unique_ptr<http::IHttpTransport> transport,
        std::unique_ptr<security::ICredentialStore> credentialStore);
    ~AgentController();

    AgentController(const AgentController&) = delete;
    AgentController& operator=(const AgentController&) = delete;

    [[nodiscard]] session::AgentSession& session() noexcept;
    [[nodiscard]] const session::AgentSession& session() const noexcept;
    [[nodiscard]] security::CredentialSession& credentials() noexcept;

    void start(session::UserAgentRequest request);
    [[nodiscard]] juce::MemoryBlock portableContextSnapshot() const;
    bool importPortableContext(const juce::MemoryBlock& encoded);
    void resetPortableContext();
    void cancel() noexcept;
    // Restore a whole user turn, including all intermediate tool commits.
    [[nodiscard]] bool canRollbackRequest() const;
    bool rollbackLastRequest();
    [[nodiscard]] session::AgentSessionSnapshot snapshot() const;

    void attachEditor(
        session::AgentSessionListener* listener,
        SaveRequestCallback saveRequest);
    void detachEditor(session::AgentSessionListener* listener);
    [[nodiscard]] bool editorAttached() const noexcept;

    std::unique_ptr<http::IRequestHandle> testConnection(
        const model::ProviderConfig& provider,
        ConnectionTestCallback callback);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

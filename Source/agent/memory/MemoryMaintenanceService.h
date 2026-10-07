#pragma once

#include "../context/ContextManager.h"
#include "../model/IModelClient.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace agentic_dexed::security {
class ICredentialStore;
}

namespace agentic_dexed::agent::memory {

class MemoryMaintenanceService final : public context::ITurnSink {
public:
    using DiagnosticCallback = std::function<void(std::string)>;

    MemoryMaintenanceService(
        model::IModelClient& modelClient,
        security::ICredentialStore& credentialStore,
        std::shared_ptr<context::ConversationContextStore> store,
        std::shared_ptr<context::ContextManager> contextManager);
    ~MemoryMaintenanceService() override;

    MemoryMaintenanceService(const MemoryMaintenanceService&) = delete;
    MemoryMaintenanceService& operator=(const MemoryMaintenanceService&) = delete;

    void onTurnFinished(context::TerminalTurn turn) override;
    void enqueue(context::TerminalTurn turn);
    void shutdown() noexcept;
    bool waitUntilIdle(std::chrono::milliseconds timeout);
    void setDiagnosticCallback(DiagnosticCallback callback);

private:
    void workerLoop();
    void process(context::TerminalTurn turn);
    bool runModel(const model::ProviderConfig& provider,
                  std::string_view authorization,
                  std::vector<model::ModelMessage> messages,
                  std::string& response);
    void diagnostic(std::string message);

    model::IModelClient& modelClient_;
    security::ICredentialStore& credentialStore_;
    std::shared_ptr<context::ConversationContextStore> store_;
    std::shared_ptr<context::ContextManager> contextManager_;

    std::mutex mutex_;
    std::condition_variable workCondition_;
    std::condition_variable idleCondition_;
    std::deque<context::TerminalTurn> queue_;
    std::deque<context::TerminalTurn> deferredCancelled_;
    std::shared_ptr<CancellationSource> activeCancellation_;
    std::unique_ptr<http::IRequestHandle> activeHandle_;
    DiagnosticCallback diagnosticCallback_;
    bool processing_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace agentic_dexed::agent::memory

#include "MemoryMaintenanceService.h"

#include "PreferenceCurator.h"
#include "../context/ContextCompactor.h"
#include "../../security/CredentialStore.h"

#include <algorithm>
#include <atomic>

namespace agentic_dexed::agent::memory {
namespace {

struct ModelResponseState {
    std::mutex mutex;
    std::condition_variable condition;
    std::string text;
    bool done = false;
    bool ok = false;
};

constexpr std::size_t kMaximumMaintenanceResponseBytes = 64u * 1024u;

} // namespace

MemoryMaintenanceService::MemoryMaintenanceService(
    model::IModelClient& modelClient,
    security::ICredentialStore& credentialStore,
    std::shared_ptr<context::ConversationContextStore> store,
    std::shared_ptr<context::ContextManager> contextManager)
    : modelClient_(modelClient), credentialStore_(credentialStore),
      store_(std::move(store)), contextManager_(std::move(contextManager)),
      worker_([this] { workerLoop(); })
{
}

MemoryMaintenanceService::~MemoryMaintenanceService()
{
    shutdown();
}

void MemoryMaintenanceService::onTurnFinished(context::TerminalTurn turn)
{
    if (contextManager_ != nullptr)
        contextManager_->onTurnFinished(turn);
    if (turn.turn.terminalState != context::TurnTerminalState::cancelled)
        enqueue(std::move(turn));
}

void MemoryMaintenanceService::enqueue(context::TerminalTurn turn)
{
    if (turn.presetId.empty() || turn.turn.turnId.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        queue_.push_back(std::move(turn));
    }
    workCondition_.notify_one();
}

void MemoryMaintenanceService::shutdown() noexcept
{
    std::unique_ptr<http::IRequestHandle> handle;
    std::shared_ptr<CancellationSource> cancellation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && !worker_.joinable())
            return;
        stopping_ = true;
        queue_.clear();
        cancellation = activeCancellation_;
        handle = std::move(activeHandle_);
    }
    if (cancellation != nullptr)
        cancellation->requestCancellation();
    if (handle != nullptr)
        handle->cancel();
    workCondition_.notify_all();
    idleCondition_.notify_all();
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeHandle_.reset();
        activeCancellation_.reset();
        processing_ = false;
    }
    idleCondition_.notify_all();
}

bool MemoryMaintenanceService::waitUntilIdle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCondition_.wait_for(lock, timeout, [this] {
        return queue_.empty() && !processing_;
    });
}

void MemoryMaintenanceService::setDiagnosticCallback(DiagnosticCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    diagnosticCallback_ = std::move(callback);
}

void MemoryMaintenanceService::workerLoop()
{
    for (;;) {
        context::TerminalTurn turn;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workCondition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
                break;
            turn = std::move(queue_.front());
            queue_.pop_front();
            processing_ = true;
        }
        try {
            process(std::move(turn));
        } catch (...) {
            diagnostic("Memory maintenance skipped after an internal error");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            processing_ = false;
            if (queue_.empty())
                idleCondition_.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processing_ = false;
    }
    idleCondition_.notify_all();
}

void MemoryMaintenanceService::process(context::TerminalTurn turn)
{
    if (store_ == nullptr || contextManager_ == nullptr)
        return;
    auto credential = credentialStore_.load(turn.credentialId);
    if (!credential.ok()) {
        diagnostic("Memory maintenance skipped because the model credential is unavailable");
        return;
    }

    const auto preferences = store_->loadPreferences(credential.secret.view());
    if (preferences.ok) {
        std::string curatorResponse;
        const auto messages = PreferenceCurator::buildRequest(
            turn.turn, preferences.text.toStdString());
        if (runModel(turn.provider, credential.secret.view(), messages, curatorResponse)) {
            const auto curated = PreferenceCurator::parseResponse(
                curatorResponse, turn.turn, credential.secret.view());
            if (curated.ok && curated.diff.has_value()
                && curated.diff->operation != PreferenceOperation::none)
            {
                const auto applied = store_->applyPreferenceDiff(
                    *curated.diff, credential.secret.view());
                diagnostic(applied.ok ? "Synth preferences updated in the background"
                                      : "Synth preference update was skipped");
            }
        }
    }

    const auto loaded = store_->load(turn.presetId);
    if (loaded.ok && loaded.exists && context::ContextCompactor::shouldCompact(loaded.context)) {
        std::string compactResponse;
        if (runModel(turn.provider, credential.secret.view(),
                     context::ContextCompactor::buildRequest(loaded.context),
                     compactResponse))
        {
            auto compacted = context::ContextCompactor::parseResponse(
                compactResponse, loaded.context, credential.secret.view());
            if (compacted.ok && compacted.context.has_value()) {
                auto replacement = std::move(*compacted.context);
                const auto committed = store_->commit(
                    replacement, loaded.context.revision, credential.secret.view());
                if (committed.status == context::ContextCommitStatus::committed) {
                    replacement.revision = committed.currentVersion;
                    contextManager_->adoptPersistedConversation(std::move(replacement));
                    diagnostic("Preset conversation context compacted in the background");
                }
            }
        }
    }
    credential.secret.clear();
}

bool MemoryMaintenanceService::runModel(
    const model::ProviderConfig& provider,
    std::string_view authorization,
    std::vector<model::ModelMessage> messages,
    std::string& response)
{
    auto state = std::make_shared<ModelResponseState>();
    auto cancellation = std::make_shared<CancellationSource>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return false;
        activeCancellation_ = cancellation;
    }

    model::ModelRequest request;
    request.provider = provider;
    request.requestId = "memory-maintenance-" + juce::Uuid().toString().toStdString();
    request.authorization = authorization;
    request.messages = std::move(messages);
    request.cancellation = cancellation->token();

    std::unique_ptr<http::IRequestHandle> handle;
    try {
        handle = modelClient_.start(request, [state](const model::ModelEvent& event) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->done)
                return;
            if (const auto* delta = std::get_if<model::ModelTextDelta>(&event)) {
                if (state->text.size() + delta->text.size() > kMaximumMaintenanceResponseBytes) {
                    state->done = true;
                    state->ok = false;
                    state->condition.notify_all();
                    return;
                }
                state->text += delta->text;
            } else if (std::holds_alternative<model::ModelCompleted>(event)) {
                state->done = true;
                state->ok = true;
                state->condition.notify_all();
            } else if (std::holds_alternative<model::ModelFailed>(event)
                       || std::holds_alternative<model::ModelToolCallReady>(event)) {
                state->done = true;
                state->ok = false;
                state->condition.notify_all();
            }
        });
    } catch (...) {
        handle.reset();
    }
    if (handle == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        activeCancellation_.reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            handle->cancel();
            activeCancellation_.reset();
            return false;
        }
        activeHandle_ = std::move(handle);
    }

    bool stopped = false;
    const auto timeout = std::clamp(provider.responseTimeout,
        std::chrono::milliseconds(1000), std::chrono::milliseconds(120000));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        for (;;) {
            if (state->done)
                break;
            state->condition.wait_for(lock, std::chrono::milliseconds(50));
            {
                std::lock_guard<std::mutex> serviceLock(mutex_);
                stopped = stopping_;
            }
            if (stopped || std::chrono::steady_clock::now() >= deadline) {
                stopped = true;
                break;
            }
        }
        if (state->done && state->ok)
            response = state->text;
        else
            stopped = true;
    }
    std::unique_ptr<http::IRequestHandle> finishedHandle;
    std::shared_ptr<CancellationSource> finishedCancellation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finishedHandle = std::move(activeHandle_);
        finishedCancellation = std::move(activeCancellation_);
    }
    if (stopped && finishedCancellation != nullptr)
        finishedCancellation->requestCancellation();
    if (stopped && finishedHandle != nullptr)
        finishedHandle->cancel();
    return !stopped;
}

void MemoryMaintenanceService::diagnostic(std::string message)
{
    if (message.size() > 160)
        message.resize(160);
    DiagnosticCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = diagnosticCallback_;
    }
    if (callback)
        callback(std::move(message));
}

} // namespace agentic_dexed::agent::memory

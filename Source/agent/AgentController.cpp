#include "AgentController.h"

#include "AgentLimits.h"
#include "context/ContextManager.h"
#include "context/PortablePresetContext.h"
#include "model/ChatCompletionsClient.h"
#include "model/ResponsesClient.h"
#include "memory/MemoryMaintenanceService.h"
#include "http/JuceHttpTransport.h"
#include "session/AgentSession.h"
#include "tools/AgentToolDispatcher.h"
#include "../audition/AuditionAnalyzer.h"
#include "../security/CredentialStore.h"
#include "../state/SynthStateService.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace agentic_dexed::agent
{
namespace
{
class RoutingModelClient final : public model::IModelClient
{
public:
    RoutingModelClient(
        model::ResponsesClient& responses,
        model::ChatCompletionsClient& chat)
        : responses_(responses), chat_(chat)
    {
    }

    std::unique_ptr<http::IRequestHandle> start(
        const model::ModelRequest& request,
        model::ModelEventCallback callback) override
    {
        if (request.provider.protocol == model::ProviderProtocol::chatCompletions)
            return chat_.start(request, std::move(callback));
        return responses_.start(request, std::move(callback));
    }

private:
    model::ResponsesClient& responses_;
    model::ChatCompletionsClient& chat_;
};

class ConnectionState : public std::enable_shared_from_this<ConnectionState>
{
public:
    explicit ConnectionState(ConnectionTestCallback callback)
        : callback_(std::move(callback)), started_(std::chrono::steady_clock::now())
    {
    }

    void setHandle(std::unique_ptr<http::IRequestHandle> handle)
    {
        bool cancelImmediately = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (terminal_ || suppressed_)
                cancelImmediately = true;
            else
                handle_ = std::move(handle);
        }
        if (cancelImmediately && handle != nullptr)
            handle->cancel();
    }

    void finish(bool success, std::string message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (terminal_ || suppressed_)
                return;
            terminal_ = true;
        }
        if (message.size() > limits::maxProtocolErrorMessageBytes)
            message.resize(limits::maxProtocolErrorMessageBytes);
        ConnectionTestResult result {
            success, std::move(message),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_)
        };
        const auto weak = weak_from_this();
        auto deliver = [weak, result = std::move(result)]() mutable {
            const auto state = weak.lock();
            if (state == nullptr)
                return;
            ConnectionTestCallback callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex_);
                if (state->suppressed_ || state->delivered_)
                    return;
                state->delivered_ = true;
                callback = state->callback_;
            }
            if (callback)
                callback(std::move(result));
        };
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            juce::MessageManager::callAsync(deliver);
        else
            deliver();
    }

    void cancel(bool suppressCallback) noexcept
    {
        cancellation_.requestCancellation();
        std::unique_ptr<http::IRequestHandle> handle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            suppressed_ = suppressed_ || suppressCallback;
            terminal_ = true;
            handle = std::move(handle_);
        }
        if (handle != nullptr)
            handle->cancel();
    }

    CancellationToken cancellationToken() const noexcept
    {
        return cancellation_.token();
    }

private:
    std::mutex mutex_;
    ConnectionTestCallback callback_;
    std::unique_ptr<http::IRequestHandle> handle_;
    std::chrono::steady_clock::time_point started_;
    bool terminal_ = false;
    bool suppressed_ = false;
    bool delivered_ = false;
    CancellationSource cancellation_;
};

class ConnectionHandle final : public http::IRequestHandle
{
public:
    explicit ConnectionHandle(std::shared_ptr<ConnectionState> state)
        : state_(std::move(state))
    {
    }

    ~ConnectionHandle() override { state_->cancel(true); }
    void cancel() noexcept override { state_->cancel(true); }

private:
    std::shared_ptr<ConnectionState> state_;
};

std::unique_ptr<http::IHttpTransport> validTransport(
    std::unique_ptr<http::IHttpTransport> transport)
{
    return transport != nullptr ? std::move(transport)
                                : std::make_unique<http::JuceHttpTransport>();
}

std::unique_ptr<security::ICredentialStore> validCredentialStore(
    std::unique_ptr<security::ICredentialStore> store)
{
    if (store != nullptr)
        return store;
    auto platform = security::createPlatformCredentialStore();
    return platform != nullptr ? std::move(platform)
                               : std::make_unique<security::MemoryCredentialStore>();
}
}

class AgentController::Impl final : public tools::ISavePatchDelegate
{
public:
    Impl(
        const ParameterRegistry& registry,
        SynthStateService& stateService,
        std::unique_ptr<http::IHttpTransport> transport,
        std::unique_ptr<security::ICredentialStore> credentialStore)
        : stateService_(stateService), transport_(validTransport(std::move(transport))),
          responses_(*transport_), chat_(*transport_), router_(responses_, chat_),
          persistentCredentials_(validCredentialStore(std::move(credentialStore))),
          credentials_(*persistentCredentials_),
          dispatcher_(registry, stateService, audition_, *this),
          contextStore_(std::make_shared<context::ConversationContextStore>()),
          contextManager_(std::make_shared<context::ContextManager>(contextStore_)),
          memoryMaintenance_(std::make_shared<memory::MemoryMaintenanceService>(
              router_, credentials_, contextStore_, contextManager_)),
          session_(router_, dispatcher_, credentials_, {}, contextManager_, memoryMaintenance_),
          connectionWorker_([this] { connectionWorkerLoop(); })
    {
        currentPresetId_ = juce::Uuid().toString().toStdString();
        context::PresetConversationContext empty;
        empty.presetId = currentPresetId_;
        const auto encoded = context::encodePortableContext(empty);
        contextManager_->installPortableContext(std::move(empty), encoded);
    }

    ~Impl() override
    {
        session_.cancel();
        std::vector<std::shared_ptr<ConnectionState>> active;
        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            for (const auto& connection : connections_)
                if (const auto state = connection.lock())
                    active.push_back(std::move(state));
            connections_.clear();
        }
        for (const auto& state : active)
            state->cancel(true);
        {
            std::lock_guard<std::mutex> lock(connectionQueueMutex_);
            connectionStopping_ = true;
            connectionQueue_.clear();
        }
        connectionQueueCondition_.notify_one();
        if (connectionWorker_.joinable())
            connectionWorker_.join();
        {
            std::lock_guard<std::mutex> lock(editorMutex_);
            saveRequest_ = {};
            editorAttached_ = false;
        }
    }

    tools::SavePatchResult requestSave(std::string_view validatedName) override
    {
        SaveRequestCallback callback;
        {
            std::lock_guard<std::mutex> lock(editorMutex_);
            if (!editorAttached_ || !saveRequest_)
                return { false, "No editor is attached to authorize a save" };
            callback = saveRequest_;
        }
        try
        {
            const auto queued = callback(std::string(validatedName));
            return { queued, queued ? "Save queued" : "Save request was not queued" };
        }
        catch (...)
        {
            return { false, "Save request was not queued" };
        }
    }

    void attachEditor(
        session::AgentSessionListener* listener,
        SaveRequestCallback callback)
    {
        if (listener != nullptr)
            session_.addListener(listener);
        std::lock_guard<std::mutex> lock(editorMutex_);
        saveRequest_ = std::move(callback);
        editorAttached_ = true;
    }

    void detachEditor(session::AgentSessionListener* listener)
    {
        if (listener != nullptr)
            session_.removeListener(listener);
        {
            std::lock_guard<std::mutex> lock(editorMutex_);
            saveRequest_ = {};
            editorAttached_ = false;
        }
        session_.cancel();
    }

    bool editorAttached() const noexcept
    {
        std::lock_guard<std::mutex> lock(editorMutex_);
        return editorAttached_;
    }

    void prepareRequest(session::UserAgentRequest& request)
    {
        std::lock_guard<std::mutex> lock(presetMutex_);
        if (request.presetId.empty())
            request.presetId = currentPresetId_;
        else
            currentPresetId_ = request.presetId;
    }

    juce::MemoryBlock portableContextSnapshot() const
    {
        std::string presetId;
        {
            std::lock_guard<std::mutex> lock(presetMutex_);
            presetId = currentPresetId_;
        }
        return contextManager_->portableSnapshot(presetId);
    }

    bool importPortableContext(const juce::MemoryBlock& encoded)
    {
        auto decoded = context::decodePortableContext(encoded);
        if (!decoded.ok || !decoded.context.has_value())
            return false;
        auto context = std::move(*decoded.context);
        const auto presetId = context.presetId;
        if (!contextManager_->installPortableContext(context, encoded))
            return false;
        {
            std::lock_guard<std::mutex> lock(presetMutex_);
            currentPresetId_ = presetId;
        }
        session_.loadConversation(contextManager_->cachedConversation(presetId));
        enqueueConnection([manager = contextManager_, context = std::move(context)]() mutable {
            manager->persistConversation(std::move(context));
        });
        return true;
    }

    void resetPortableContext()
    {
        context::PresetConversationContext empty;
        empty.presetId = juce::Uuid().toString().toStdString();
        importPortableContext(context::encodePortableContext(empty));
    }

    bool activatePreset(context::PresetActivation activation)
    {
        if (juce::Uuid(juce::String(activation.presetId)).isNull())
            return false;

        session_.cancel();
        auto view = contextManager_->cachedConversation(activation.presetId);
        if (view.empty())
            view = contextManager_->loadConversation(activation.presetId);
        if (view.empty())
            return false;

        if (!activation.fingerprint.empty()
            && std::find(view->fingerprintAliases.begin(),
                         view->fingerprintAliases.end(), activation.fingerprint)
                == view->fingerprintAliases.end())
        {
            auto updated = *view.snapshot();
            updated.fingerprintAliases.push_back(std::move(activation.fingerprint));
            const auto encoded = context::encodePortableContext(updated);
            if (!contextManager_->installPortableContext(updated, encoded))
                return false;
            enqueueConnection([manager = contextManager_, updated = std::move(updated)]() mutable {
                manager->persistConversation(std::move(updated));
            });
            view = contextManager_->cachedConversation(activation.presetId);
        }

        {
            std::lock_guard<std::mutex> lock(presetMutex_);
            currentPresetId_ = activation.presetId;
        }
        session_.loadConversation(view);
        return true;
    }

    bool clonePresetContext(const context::PresetId& source,
                            const context::PresetId& destination)
    {
        if (source.empty() || destination.empty() || source == destination
            || juce::Uuid(juce::String(destination)).isNull())
            return false;
        auto sourceView = contextManager_->cachedConversation(source);
        if (sourceView.empty())
            sourceView = contextManager_->loadConversation(source);
        if (sourceView.empty())
            return false;
        auto cloned = *sourceView.snapshot();
        cloned.presetId = destination;
        cloned.revision = 0;
        cloned.updatedAtUnixMs = 0;
        cloned.fingerprintAliases.clear();
        const auto encoded = context::encodePortableContext(cloned);
        if (!contextManager_->installPortableContext(cloned, encoded))
            return false;
        enqueueConnection([manager = contextManager_, cloned = std::move(cloned)]() mutable {
            manager->persistConversation(std::move(cloned));
        });
        return true;
    }

    bool movePresetContext(const context::PresetId& source,
                           const context::PresetId& destination)
    {
        if (source == destination)
            return true;
        if (!clonePresetContext(source, destination))
            return false;
        bool wasCurrent = false;
        {
            std::lock_guard<std::mutex> lock(presetMutex_);
            wasCurrent = currentPresetId_ == source;
        }
        return !wasCurrent || activatePreset({ destination, {}, false });
    }

    context::PresetContextView currentConversation() const
    {
        std::string presetId;
        {
            std::lock_guard<std::mutex> lock(presetMutex_);
            presetId = currentPresetId_;
        }
        auto view = contextManager_->cachedConversation(presetId);
        return view.empty() ? contextManager_->loadConversation(presetId) : view;
    }

    std::unique_ptr<http::IRequestHandle> testConnection(
        const model::ProviderConfig& provider,
        ConnectionTestCallback callback)
    {
        auto state = std::make_shared<ConnectionState>(std::move(callback));
        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            connections_.erase(
                std::remove_if(
                    connections_.begin(), connections_.end(),
                    [](const auto& item) { return item.expired(); }),
                connections_.end());
            connections_.push_back(state);
        }
        auto publicHandle = std::make_unique<ConnectionHandle>(state);
        enqueueConnection([this, state, provider] { startConnection(state, provider); });
        return publicHandle;
    }

    void startConnection(
        const std::shared_ptr<ConnectionState>& state,
        const model::ProviderConfig& provider)
    {
        if (state->cancellationToken().isCancellationRequested())
            return;
        auto loaded = credentials_.load("agent.model");
        if (!loaded.ok() || loaded.secret.empty())
        {
            state->finish(false, "No model credential is available for this provider");
            return;
        }

        model::ModelRequest request;
        request.provider = provider;
        request.requestId = "agent-connection-"
            + std::to_string(nextConnectionId_.fetch_add(1, std::memory_order_relaxed));
        request.authorization = loaded.secret.view();
        request.messages = {
            { "system", "Reply with OK and do not call tools.", {}, {} },
            { "user", "Connection test", {}, {} }
        };
        request.cancellation = state->cancellationToken();
        const auto weak = std::weak_ptr<ConnectionState>(state);
        try
        {
            auto modelHandle = router_.start(
                request,
                [weak](const model::ModelEvent& event) {
                    const auto current = weak.lock();
                    if (current == nullptr)
                        return;
                    if (std::holds_alternative<model::ModelCompleted>(event))
                        current->finish(true, "Connection succeeded");
                    else if (const auto* failure = std::get_if<model::ModelFailed>(&event))
                        current->finish(false, failure->error.message);
                    else if (std::holds_alternative<model::ModelToolCallReady>(event))
                        current->finish(false, "Connection test returned an unexpected tool call");
                });
            if (modelHandle == nullptr)
                state->finish(false, "Connection test did not create a request");
            else
                state->setHandle(std::move(modelHandle));
        }
        catch (...)
        {
            state->finish(false, "Connection test could not be started");
        }
        loaded.secret.clear();
    }

    void enqueueConnection(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(connectionQueueMutex_);
            if (connectionStopping_)
                return;
            connectionQueue_.push_back(std::move(task));
        }
        connectionQueueCondition_.notify_one();
    }

    void connectionWorkerLoop()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(connectionQueueMutex_);
                connectionQueueCondition_.wait(lock, [this] {
                    return connectionStopping_ || !connectionQueue_.empty();
                });
                if (connectionStopping_)
                    return;
                task = std::move(connectionQueue_.front());
                connectionQueue_.pop_front();
            }
            task();
        }
    }

    SynthStateService& stateService_;
    mutable std::mutex checkpointMutex_;
    std::deque<SynthSnapshot> requestCheckpoints_;
    std::unique_ptr<http::IHttpTransport> transport_;
    model::ResponsesClient responses_;
    model::ChatCompletionsClient chat_;
    RoutingModelClient router_;
    std::unique_ptr<security::ICredentialStore> persistentCredentials_;
    security::CredentialSession credentials_;
    audition::AuditionAnalyzer audition_;
    tools::AgentToolDispatcher dispatcher_;
    std::shared_ptr<context::ConversationContextStore> contextStore_;
    std::shared_ptr<context::ContextManager> contextManager_;
    std::shared_ptr<memory::MemoryMaintenanceService> memoryMaintenance_;
    session::AgentSession session_;

    mutable std::mutex presetMutex_;
    std::string currentPresetId_;

    mutable std::mutex editorMutex_;
    SaveRequestCallback saveRequest_;
    bool editorAttached_ = false;

    std::mutex connectionMutex_;
    std::vector<std::weak_ptr<ConnectionState>> connections_;
    std::atomic_uint64_t nextConnectionId_ { 1 };
    std::mutex connectionQueueMutex_;
    std::condition_variable connectionQueueCondition_;
    std::deque<std::function<void()>> connectionQueue_;
    bool connectionStopping_ = false;
    std::thread connectionWorker_;
};

AgentController::AgentController(
    const ParameterRegistry& registry,
    SynthStateService& stateService)
    : AgentController(registry, stateService,
                      std::make_unique<http::JuceHttpTransport>(),
                      security::createPlatformCredentialStore())
{
}

AgentController::AgentController(
    const ParameterRegistry& registry,
    SynthStateService& stateService,
    std::unique_ptr<http::IHttpTransport> transport,
    std::unique_ptr<security::ICredentialStore> credentialStore)
    : impl_(std::make_unique<Impl>(
          registry, stateService, std::move(transport), std::move(credentialStore)))
{
}

AgentController::~AgentController() = default;

session::AgentSession& AgentController::session() noexcept { return impl_->session_; }
const session::AgentSession& AgentController::session() const noexcept { return impl_->session_; }
security::CredentialSession& AgentController::credentials() noexcept { return impl_->credentials_; }

void AgentController::start(session::UserAgentRequest request)
{
    impl_->prepareRequest(request);
    {
        std::lock_guard<std::mutex> lock(impl_->checkpointMutex_);
        impl_->requestCheckpoints_.push_back(impl_->stateService_.snapshot({}));
        if (impl_->requestCheckpoints_.size() > 32)
            impl_->requestCheckpoints_.pop_front();
    }
    impl_->session_.start(std::move(request));
}

juce::MemoryBlock AgentController::portableContextSnapshot() const
{
    return impl_->portableContextSnapshot();
}

bool AgentController::importPortableContext(const juce::MemoryBlock& encoded)
{
    return impl_->importPortableContext(encoded);
}

void AgentController::resetPortableContext()
{
    impl_->resetPortableContext();
}

bool AgentController::activatePreset(context::PresetActivation activation)
{
    return impl_->activatePreset(std::move(activation));
}

bool AgentController::clonePresetContext(
    const context::PresetId& source, const context::PresetId& destination)
{
    return impl_->clonePresetContext(source, destination);
}

bool AgentController::movePresetContext(
    const context::PresetId& source, const context::PresetId& destination)
{
    return impl_->movePresetContext(source, destination);
}

context::PresetContextView AgentController::currentConversation() const
{
    return impl_->currentConversation();
}

void AgentController::cancel() noexcept { impl_->session_.cancel(); }

bool AgentController::canRollbackRequest() const
{
    std::lock_guard<std::mutex> lock(impl_->checkpointMutex_);
    return !impl_->requestCheckpoints_.empty();
}

bool AgentController::rollbackLastRequest()
{
    using State = session::AgentSessionState;
    const auto state = snapshot().state;
    if (state != State::completed && state != State::failed
        && state != State::cancelled && state != State::idle)
        return false;
    std::lock_guard<std::mutex> lock(impl_->checkpointMutex_);
    if (impl_->requestCheckpoints_.empty()) return false;
    const auto current = impl_->stateService_.snapshot({});
    PatchRequest restore;
    restore.transactionId = "$ui.request.rollback." + juce::Uuid().toString().toStdString();
    restore.baseRevision = current.revision;
    restore.reason = "Restore state before user request";
    restore.source = PatchSource::ui;
    for (const auto& [id, value] : impl_->requestCheckpoints_.back().values)
        if (current.values.at(id) != value)
            restore.operations.push_back({ id, value });
    if (!restore.operations.empty()
        && impl_->stateService_.submit(restore).status != PatchStatus::committed)
        return false; // Keep the checkpoint if a concurrent host edit wins.
    impl_->requestCheckpoints_.pop_back();
    return true;
}

session::AgentSessionSnapshot AgentController::snapshot() const
{
    return impl_->session_.snapshot();
}

void AgentController::attachEditor(
    session::AgentSessionListener* listener,
    SaveRequestCallback saveRequest)
{
    impl_->attachEditor(listener, std::move(saveRequest));
}

void AgentController::detachEditor(session::AgentSessionListener* listener)
{
    impl_->detachEditor(listener);
}

bool AgentController::editorAttached() const noexcept { return impl_->editorAttached(); }

std::unique_ptr<http::IRequestHandle> AgentController::testConnection(
    const model::ProviderConfig& provider,
    ConnectionTestCallback callback)
{
    return impl_->testConnection(provider, std::move(callback));
}
}

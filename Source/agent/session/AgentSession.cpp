#include "AgentSession.h"

#include "AgentPrompt.h"
#include "../memory/SensitiveDataFilter.h"
#include "../context/ContextManager.h"
#include "../AgentLimits.h"
#include "../JsonAccess.h"
#include "../model/IModelClient.h"
#include "../tools/AgentToolDispatcher.h"
#include "../tools/AgentToolSchemas.h"
#include "../../security/CredentialStore.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace agentic_dexed::agent::session
{
namespace
{
constexpr std::size_t maxPromptBytes = 32u * 1024u;
constexpr std::size_t maxStreamingTextBytes = 48u * 1024u;
constexpr std::size_t maxTranscriptBytes = 128u * 1024u;
constexpr std::size_t maxTranscriptEntries = 128;
constexpr std::size_t maxSingleToolOutputBytes = 128u * 1024u;
constexpr std::size_t maxContextBytes = limits::maxRequestBytes;
constexpr std::size_t maxTransactionSummaries = 32;

bool isTerminal(AgentSessionState state)
{
    return state == AgentSessionState::completed
        || state == AgentSessionState::cancelled
        || state == AgentSessionState::failed;
}

std::string json(const juce::var& value)
{
    return juce::JSON::toString(value, true).toStdString();
}

juce::var errorObject(const char* code, const char* message)
{
    auto* error = new juce::DynamicObject();
    error->setProperty("code", code);
    error->setProperty("message", message);
    auto* root = new juce::DynamicObject();
    root->setProperty("error", juce::var(error));
    return juce::var(root);
}

std::optional<std::string> propertyString(const juce::var& value, const char* name)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr || !object->hasProperty(name))
        return std::nullopt;
    const auto property = object->getProperty(name);
    return property.isString() ? std::optional<std::string>(property.toString().toStdString())
                               : std::nullopt;
}

uint64_t propertyRevision(const juce::var& value, const char* name)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr || !object->hasProperty(name))
        return 0;
    const auto property = object->getProperty(name);
    if (!property.isInt() && !property.isInt64())
        return 0;
    const auto revision = static_cast<int64_t>(property);
    return revision >= 0 ? static_cast<uint64_t>(revision) : 0;
}

std::size_t messageSize(const model::ModelMessage& message)
{
    std::size_t result = message.role.size() + message.text.size() + 32;
    if (message.reasoningContent) result += message.reasoningContent->size();
    for (const auto& call : message.toolCalls)
        result += call.callId.size() + call.name.size() + call.arguments.size() + 64;
    if (message.toolCall)
        result += message.toolCall->callId.size() + message.toolCall->name.size()
            + message.toolCall->arguments.size() + 64;
    if (message.toolResult)
        result += message.toolResult->callId.size() + message.toolResult->output.size() + 48;
    return result;
}

std::string persistentText(std::string_view text, std::string_view credential)
{
    auto sanitized = memory::SensitiveDataFilter::sanitizeForContext(text, credential).text;
    if (sanitized.size() <= context::kMaximumMessageBytes)
        return sanitized;
    auto value = juce::String::fromUTF8(sanitized.data(), static_cast<int>(sanitized.size()));
    while (value.getNumBytesAsUTF8() > static_cast<int>(context::kMaximumMessageBytes)
           && value.isNotEmpty())
        value = value.dropLastCharacters(std::max(1, value.length() / 16));
    return value.toStdString();
}

struct PendingConfirmation
{
    std::string transactionId;
    std::string callId;
    std::vector<model::ModelToolCallReady> remainingCalls;
};

class ListenerBridge : public std::enable_shared_from_this<ListenerBridge>
{
public:
    void add(AgentSessionListener* listener)
    {
        if (listener == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end())
            listeners_.push_back(listener);
    }

    void remove(AgentSessionListener* listener)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listeners_.erase(
            std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
    }

    void publish(AgentSessionSnapshot snapshot)
    {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.empty()
                && pending_.back().state == AgentSessionState::streaming
                && snapshot.state == AgentSessionState::streaming)
                pending_.back() = std::move(snapshot);
            else
            {
                if (pending_.size() == 64)
                    pending_.pop_front();
                pending_.push_back(std::move(snapshot));
            }
            if (!notificationScheduled_)
            {
                notificationScheduled_ = true;
                schedule = true;
            }
        }
        if (!schedule)
            return;

        const auto weak = weak_from_this();
        const auto notify = [weak]() {
            const auto bridge = weak.lock();
            if (bridge == nullptr || !bridge->active_.load(std::memory_order_acquire))
                return;
            std::vector<AgentSessionListener*> listeners;
            std::deque<AgentSessionSnapshot> snapshots;
            {
                std::lock_guard<std::mutex> lock(bridge->mutex_);
                listeners = bridge->listeners_;
                snapshots.swap(bridge->pending_);
                bridge->notificationScheduled_ = false;
            }
            for (const auto& snapshot : snapshots)
                for (auto* listener : listeners)
                    if (listener != nullptr)
                        listener->agentSessionChanged(snapshot);
        };
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            juce::MessageManager::callAsync(notify);
        else
            notify();
    }

    void deactivate()
    {
        active_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        listeners_.clear();
    }

private:
    std::atomic_bool active_ { true };
    std::mutex mutex_;
    std::vector<AgentSessionListener*> listeners_;
    std::deque<AgentSessionSnapshot> pending_;
    bool notificationScheduled_ = false;
};
}

class AgentSession::Impl
{
public:
    Impl(
        model::IModelClient& modelClient,
        tools::AgentToolDispatcher& toolDispatcher,
        security::ICredentialStore& credentialStore,
        std::shared_ptr<memory::SynthMemory>,
        std::shared_ptr<context::ContextManager> contextManager,
        std::shared_ptr<context::ITurnSink> turnSink)
        : modelClient_(modelClient), toolDispatcher_(toolDispatcher),
          credentialStore_(credentialStore), contextManager_(std::move(contextManager)),
          turnSink_(std::move(turnSink)), listenerBridge_(std::make_shared<ListenerBridge>()),
          callbackAlive_(std::make_shared<std::atomic_bool>(true)),
          worker_([this] { workerLoop(); })
    {
        defaultPresetId_ = juce::Uuid().toString().toStdString();
        if (turnSink_ == nullptr && contextManager_ != nullptr)
            turnSink_ = contextManager_;
    }

    ~Impl()
    {
        std::shared_ptr<CancellationSource> cancellation;
        {
            std::lock_guard<std::mutex> lock(cancellationMutex_);
            cancellation = activeCancellation_;
        }
        if (cancellation != nullptr)
            cancellation->requestCancellation();
        callbackAlive_->store(false, std::memory_order_release);

        auto complete = std::make_shared<std::promise<void>>();
        auto future = complete->get_future();
        enqueueInternal([this, complete] {
            if (!isTerminal(working_.state) && working_.state != AgentSessionState::idle)
                cancelOnWorker();
            requestHandle_.reset();
            credential_.clear();
            complete->set_value();
        });
        future.wait();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopping_ = true;
        }
        queueCondition_.notify_one();
        if (worker_.joinable())
            worker_.join();
        listenerBridge_->deactivate();
    }

    void start(UserAgentRequest request)
    {
        auto cancellation = std::make_shared<CancellationSource>();
        {
            std::lock_guard<std::mutex> lock(cancellationMutex_);
            if (activeCancellation_ != nullptr)
                activeCancellation_->requestCancellation();
            activeCancellation_ = cancellation;
        }
        enqueue([this, request = std::move(request), cancellation]() mutable {
            startOnWorker(std::move(request), std::move(cancellation));
        });
    }

    void loadConversation(context::PresetContextView view)
    {
        enqueue([this, view = std::move(view)] {
            if (!isTerminal(working_.state) && working_.state != AgentSessionState::idle)
                return;
            working_.transcript.clear();
            if (!view.empty())
                for (const auto& turn : view->recentTurns)
                    for (const auto& message : turn.messages)
                        if (message.role == context::ConversationRole::user)
                            appendTranscript({ AgentTranscriptKind::user, message.text });
                        else if (message.role == context::ConversationRole::assistant
                                 && !message.text.empty())
                            appendTranscript({ AgentTranscriptKind::assistant, message.text });
            publish();
        });
    }

    void confirmProposal(std::string proposalId)
    {
        enqueue([this, proposalId = std::move(proposalId)] {
            confirmOnWorker(proposalId);
        });
    }

    void cancel() noexcept
    {
        std::shared_ptr<CancellationSource> cancellation;
        {
            std::lock_guard<std::mutex> lock(cancellationMutex_);
            cancellation = activeCancellation_;
        }
        if (cancellation != nullptr)
            cancellation->requestCancellation();
        enqueue([this] { cancelOnWorker(); });
    }

    void addListener(AgentSessionListener* listener) { listenerBridge_->add(listener); }
    void removeListener(AgentSessionListener* listener) { listenerBridge_->remove(listener); }

    AgentSessionSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        return publishedSnapshot_;
    }

private:
    void enqueue(std::function<void()> task)
    {
        if (!callbackAlive_->load(std::memory_order_acquire))
            return;
        enqueueInternal(std::move(task));
    }

    void enqueueInternal(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (stopping_)
                return;
            queue_.push_back(std::move(task));
        }
        queueCondition_.notify_one();
    }

    void workerLoop()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCondition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty())
                {
                    if (stopping_)
                        return;
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

    void publish()
    {
        AgentSessionSnapshot snapshotCopy = working_;
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            publishedSnapshot_ = snapshotCopy;
        }
        listenerBridge_->publish(std::move(snapshotCopy));
    }

    void setState(AgentSessionState state)
    {
        working_.state = state;
        publish();
    }

    void startOnWorker(
        UserAgentRequest request,
        std::shared_ptr<CancellationSource> cancellation)
    {
        if (!isTerminal(working_.state) && working_.state != AgentSessionState::idle)
            cancelOnWorker();
        requestHandle_.reset();
        credential_.clear();
        toolDispatcher_.resetSession();
        toolDispatcher_.setReleasePolicy(request.prompt);
        ++generation_;
        requestSequence_ = 0;
        toolIterations_ = 0;
        consecutiveProtocolErrors_ = 0;
        preferences_ = request.preferences;
        cancellation_ = std::move(cancellation);
        messages_.clear();
        turnMessages_.clear();
        toolNamesByCallId_.clear();
        pendingCalls_.clear();
        pendingCallIds_.clear();
        pendingConfirmation_.reset();
        working_ = {};
        terminalEmitted_ = false;
        presetId_ = request.presetId.empty() ? defaultPresetId_ : request.presetId;
        turnId_ = juce::Uuid().toString().toStdString();
        expectedContextVersion_ = 0;
        credentialId_ = request.credentialId;

        if (request.prompt.empty() || request.prompt.size() > maxPromptBytes)
        {
            fail("invalid_request", "Agent prompt is empty or exceeds the configured limit");
            return;
        }
        if (cancellation_->token().isCancellationRequested())
        {
            cancelOnWorker();
            return;
        }

        auto loaded = credentialStore_.load(request.credentialId);
        if (!loaded.ok() || loaded.secret.empty())
        {
            fail("missing_credential", "No model credential is available for this provider");
            return;
        }
        credential_ = std::move(loaded.secret);

        turnMessages_.push_back({ context::ConversationRole::user, request.prompt });
        auto systemPrompt = createAgentSystemPrompt(preferences_.applyMode, request.prompt);
        if (contextManager_ != nullptr)
        {
            auto built = contextManager_->buildRequestContext(
                presetId_, request.prompt, std::move(systemPrompt));
            if (!built.ok)
            {
                fail("context_load_failed", built.error);
                return;
            }
            messages_ = std::move(built.messages);
            if (!built.context.empty())
                expectedContextVersion_ = built.context->revision;
            for (const auto& entry : built.history)
                appendTranscript({
                    entry.role == context::ConversationRole::user
                        ? AgentTranscriptKind::user : AgentTranscriptKind::assistant,
                    entry.text, {}, {}, true });
        }
        else
        {
            messages_.push_back({ "system", std::move(systemPrompt), {}, {} });
            messages_.push_back({ "user", request.prompt, {}, {} });
        }
        appendTranscript({ AgentTranscriptKind::user, request.prompt, {}, {}, true });
        startModelRequest();
    }

    void startModelRequest()
    {
        if (cancelled())
        {
            cancelOnWorker();
            return;
        }
        trimContext();
        pendingCalls_.clear();
        pendingCallIds_.clear();
        roundText_.clear();
        roundReasoning_.reset();
        working_.streamingText.clear();
        setState(AgentSessionState::requesting);

        model::ModelRequest request;
        request.provider = preferences_.providerConfig();
        request.requestId = "agent-session-" + std::to_string(generation_)
            + "-" + std::to_string(++requestSequence_);
        request.authorization = credential_.view();
        request.messages = messages_;
        if (toolIterations_ < limits::maxToolIterations)
            request.tools = tools::createAgentToolSchemas();
        request.cancellation = cancellation_->token();

        const auto generation = generation_;
        const auto alive = callbackAlive_;
        try
        {
            auto handle = modelClient_.start(
                request,
                [this, generation, alive](const model::ModelEvent& event) {
                    if (!alive->load(std::memory_order_acquire))
                        return;
                    enqueue([this, generation, event] { handleModelEvent(generation, event); });
                });
            if (handle == nullptr)
            {
                fail("model_start_failed", "Model client did not create a request");
                return;
            }
            requestHandle_ = std::move(handle);
        }
        catch (...)
        {
            fail("model_start_failed", "Model request could not be started");
        }
    }

    void handleModelEvent(uint64_t generation, const model::ModelEvent& event)
    {
        if (generation != generation_ || isTerminal(working_.state))
            return;
        if (cancelled())
        {
            cancelOnWorker();
            return;
        }

        if (std::holds_alternative<model::ModelStarted>(event))
        {
            setState(AgentSessionState::streaming);
            return;
        }
        if (const auto* delta = std::get_if<model::ModelReasoningDelta>(&event))
        {
            if (!roundReasoning_) roundReasoning_.emplace();
            if (roundReasoning_->size() + delta->text.size() > maxContextBytes)
            {
                fail("context_limit", "Model continuation exceeds the context limit");
                return;
            }
            *roundReasoning_ += delta->text;
            return;
        }
        if (const auto* delta = std::get_if<model::ModelTextDelta>(&event))
        {
            const auto remaining = maxStreamingTextBytes > roundText_.size()
                ? maxStreamingTextBytes - roundText_.size() : 0;
            roundText_.append(delta->text.data(), std::min(remaining, delta->text.size()));
            working_.streamingText = roundText_;
            setState(AgentSessionState::streaming);
            return;
        }
        if (const auto* call = std::get_if<model::ModelToolCallReady>(&event))
        {
            if (!call->callId.empty() && pendingCallIds_.insert(call->callId).second)
                pendingCalls_.push_back(*call);
            setState(AgentSessionState::streaming);
            return;
        }
        if (const auto* failure = std::get_if<model::ModelFailed>(&event))
        {
            if (failure->error.code == "cancelled" || cancelled())
                cancelOnWorker();
            else
                fail(failure->error.code, failure->error.message);
            return;
        }
        if (std::holds_alternative<model::ModelCompleted>(event))
            completeModelRound();
    }

    void completeModelRound()
    {
        requestHandle_.reset();
        model::ModelMessage assistant;
        assistant.role = "assistant";
        assistant.text = roundText_;
        assistant.reasoningContent = roundReasoning_;
        for (const auto& call : pendingCalls_)
            assistant.toolCalls.push_back({ call.callId, call.name, call.arguments });
        messages_.push_back(std::move(assistant));
        if (!roundText_.empty())
        {
            appendTranscript({ AgentTranscriptKind::assistant, roundText_, {}, {}, true });
            turnMessages_.push_back({ context::ConversationRole::assistant, roundText_ });
        }

        if (pendingCalls_.empty())
        {
            if (roundText_.empty() && toolIterations_ < limits::maxToolIterations)
            {
                fail("empty_response", "Model returned no answer or tool call; the task did not complete");
                return;
            }
            if (roundText_.empty() && toolIterations_ >= limits::maxToolIterations)
            {
                working_.limitReached = true;
                fail("tool_limit", "Agent reached its tool limit without a final answer");
                return;
            }
            working_.finalText = roundText_;
            complete();
            return;
        }

        if (toolIterations_ >= limits::maxToolIterations)
        {
            working_.limitReached = true;
            fail("tool_limit", "Agent reached its tool limit with unfinished tool calls");
            return;
        }

        auto calls = std::move(pendingCalls_);
        pendingCalls_.clear();
        executeTools(std::move(calls));
    }

    void executeTools(std::vector<model::ModelToolCallReady> calls)
    {
        for (std::size_t index = 0; index < calls.size(); ++index)
        {
            if (cancelled())
            {
                cancelOnWorker();
                return;
            }
            auto call = std::move(calls[index]);
            if (call.callId.empty() || call.callId.size() > 128
                || call.arguments.size() > limits::maxRequestBytes)
            {
                handleMalformedCall(call, "Tool call metadata exceeds the configured limit");
                if (isTerminal(working_.state))
                    return;
                continue;
            }

            auto parsed = parseJsonObject(call.arguments);
            if (!parsed.ok())
            {
                handleMalformedCall(call, "Tool arguments are not a JSON object");
                if (isTerminal(working_.state))
                    return;
                continue;
            }

            if (preferences_.applyMode == AgentApplyMode::confirmation
                && call.name == "apply_parameter_patch")
            {
                parsed.value->getDynamicObject()->setProperty("mode", "proposed");
                call.arguments = json(*parsed.value);
            }

            setState(AgentSessionState::executingTool);
            appendToolCall(call);
            tools::ToolResult result;
            try
            {
                result = toolDispatcher_.dispatch(
                    call.name, *parsed.value, call.callId, cancellation_->token());
            }
            catch (...)
            {
                result = { call.callId, false,
                           errorObject("tool_exception", "Local tool execution failed"),
                           "tool_exception" };
            }
            if (cancelled() || result.errorCode == "cancelled")
            {
                cancelOnWorker();
                return;
            }

            consecutiveProtocolErrors_ = 0;
            updateTransaction(call, *parsed.value, result);
            if (preferences_.applyMode == AgentApplyMode::confirmation
                && call.name == "apply_parameter_patch" && result.success
                && propertyString(result.output, "status") == std::optional<std::string>("proposed"))
            {
                const auto transactionId = propertyString(*parsed.value, "transaction_id");
                if (!transactionId)
                {
                    fail("invalid_proposal", "Proposed transaction has no transaction ID");
                    return;
                }
                PendingConfirmation pending;
                pending.transactionId = *transactionId;
                pending.callId = call.callId;
                for (auto remaining = index + 1; remaining < calls.size(); ++remaining)
                    pending.remainingCalls.push_back(std::move(calls[remaining]));
                pendingConfirmation_ = std::move(pending);
                working_.pendingProposalId = *transactionId;
                setState(AgentSessionState::awaitingConfirmation);
                return;
            }
            appendToolResult(result);
        }
        finishToolRound();
    }

    void handleMalformedCall(
        const model::ModelToolCallReady& call, const char* message)
    {
        appendToolCall(call);
        tools::ToolResult result {
            call.callId, false, errorObject("malformed_tool_call", message),
            "malformed_tool_call"
        };
        appendToolResult(result);
        ++consecutiveProtocolErrors_;
        if (consecutiveProtocolErrors_ >= limits::maxConsecutiveProtocolErrors)
            fail("too_many_protocol_errors", "The model emitted repeated malformed tool calls");
    }

    void finishToolRound()
    {
        ++toolIterations_;
        working_.toolIterations = toolIterations_;
        trimContext();
        startModelRequest();
    }

    void confirmOnWorker(const std::string& proposalId)
    {
        if (working_.state != AgentSessionState::awaitingConfirmation
            || !pendingConfirmation_ || pendingConfirmation_->transactionId != proposalId)
            return;
        if (cancelled())
        {
            cancelOnWorker();
            return;
        }

        setState(AgentSessionState::executingTool);
        auto result = toolDispatcher_.confirmProposal(
            pendingConfirmation_->transactionId, pendingConfirmation_->callId);
        appendToolResult(result);
        updateConfirmedTransaction(pendingConfirmation_->transactionId, result);
        working_.pendingProposalId.clear();
        auto remaining = std::move(pendingConfirmation_->remainingCalls);
        pendingConfirmation_.reset();
        if (!remaining.empty())
            executeTools(std::move(remaining));
        else
            finishToolRound();
    }

    void appendToolCall(const model::ModelToolCallReady& call)
    {
        appendTranscript({ AgentTranscriptKind::toolCall, call.arguments,
                           call.callId, call.name, true });
        toolNamesByCallId_[call.callId] = call.name;
        turnMessages_.push_back({ context::ConversationRole::toolCall,
                                  call.arguments, call.callId, call.name, true });
    }

    void appendToolResult(tools::ToolResult result)
    {
        auto output = json(result.output);
        if (output.size() > maxSingleToolOutputBytes)
        {
            result.success = false;
            result.errorCode = "tool_output_too_large";
            output = R"({"error":{"code":"tool_output_too_large","message":"Tool output was too large for model context"}})";
        }
        model::ModelMessage message;
        message.toolResult = model::ModelToolResultMessage { result.callId, output };
        messages_.push_back(std::move(message));
        appendTranscript({ AgentTranscriptKind::toolResult, output,
                           result.callId, {}, result.success });
        const auto found = toolNamesByCallId_.find(result.callId);
        turnMessages_.push_back({ context::ConversationRole::toolResult, output,
                                  result.callId,
                                  found == toolNamesByCallId_.end() ? std::string() : found->second,
                                  result.success });
    }

    void updateTransaction(
        const model::ModelToolCallReady& call,
        const juce::var& arguments,
        const tools::ToolResult& result)
    {
        if (call.name != "apply_parameter_patch")
            return;
        const auto transactionId = propertyString(arguments, "transaction_id");
        if (!transactionId)
            return;
        AgentTransactionSummary summary;
        summary.transactionId = *transactionId;
        summary.reason = propertyString(arguments, "reason").value_or(std::string());
        summary.status = propertyString(result.output, "status").value_or(
            result.success ? "committed" : "rejected");
        summary.baseRevision = propertyRevision(result.output, "base_revision");
        summary.resultingRevision = propertyRevision(result.output, "resulting_revision");
        auto found = std::find_if(
            working_.transactions.begin(), working_.transactions.end(),
            [&summary](const auto& item) { return item.transactionId == summary.transactionId; });
        if (found != working_.transactions.end())
            *found = std::move(summary);
        else
        {
            if (working_.transactions.size() == maxTransactionSummaries)
                working_.transactions.erase(working_.transactions.begin());
            working_.transactions.push_back(std::move(summary));
        }
    }

    void updateConfirmedTransaction(
        const std::string& transactionId, const tools::ToolResult& result)
    {
        const auto found = std::find_if(
            working_.transactions.begin(), working_.transactions.end(),
            [&transactionId](const auto& item) { return item.transactionId == transactionId; });
        if (found == working_.transactions.end())
            return;
        found->status = propertyString(result.output, "status").value_or(
            result.success ? "committed" : "rejected");
        found->baseRevision = propertyRevision(result.output, "base_revision");
        found->resultingRevision = propertyRevision(result.output, "resulting_revision");
    }

    void appendTranscript(AgentTranscriptEntry entry)
    {
        if (entry.text.size() > maxStreamingTextBytes)
            entry.text.resize(maxStreamingTextBytes);
        working_.transcript.push_back(std::move(entry));
        auto bytes = transcriptBytes();
        while ((working_.transcript.size() > maxTranscriptEntries
                || bytes > maxTranscriptBytes)
               && working_.transcript.size() > 1)
        {
            bytes -= working_.transcript.front().text.size()
                + working_.transcript.front().callId.size()
                + working_.transcript.front().toolName.size();
            working_.transcript.erase(working_.transcript.begin());
        }
    }

    std::size_t transcriptBytes() const
    {
        std::size_t result = 0;
        for (const auto& entry : working_.transcript)
            result += entry.text.size() + entry.callId.size() + entry.toolName.size() + 32;
        return result;
    }

    void trimContext()
    {
        auto total = contextBytes();
        while (total > maxContextBytes && messages_.size() > 2)
        {
            std::size_t removeCount = 1;
            if (!messages_[2].toolCalls.empty())
                while (2 + removeCount < messages_.size()
                       && messages_[2 + removeCount].toolResult)
                    ++removeCount;
            if (messages_[2].toolCall && messages_.size() > 3
                && messages_[3].toolResult
                && messages_[2].toolCall->callId == messages_[3].toolResult->callId)
                removeCount = 2;
            messages_.erase(
                messages_.begin() + 2,
                messages_.begin() + 2 + static_cast<std::ptrdiff_t>(removeCount));
            total = contextBytes();
        }
    }

    std::size_t contextBytes() const
    {
        std::size_t result = 0;
        for (const auto& message : messages_)
            result += messageSize(message);
        return result;
    }

    bool cancelled() const
    {
        return cancellation_ != nullptr
            && cancellation_->token().isCancellationRequested();
    }

    void emitTerminalTurn(context::TurnTerminalState terminalState)
    {
        if (terminalEmitted_)
            return;
        terminalEmitted_ = true;
        if (turnSink_ == nullptr || presetId_.empty() || turnMessages_.empty())
            return;
        const auto secret = credential_.view();
        context::ConversationTurn turn;
        turn.turnId = turnId_;
        turn.terminalState = terminalState;
        turn.messages = turnMessages_;
        for (auto& message : turn.messages)
            message.text = persistentText(message.text, secret);
        for (const auto& transaction : working_.transactions)
            turn.transactions.push_back({
                transaction.transactionId,
                persistentText(transaction.reason, secret),
                transaction.resultingRevision
            });
        context::TerminalTurn terminal;
        terminal.presetId = presetId_;
        terminal.expectedVersion = expectedContextVersion_;
        terminal.turn = std::move(turn);
        terminal.provider = preferences_.providerConfig();
        terminal.credentialId = credentialId_;
        try { turnSink_->onTurnFinished(std::move(terminal)); }
        catch (...) {}
    }

    void cancelActiveResources()
    {
        if (pendingConfirmation_)
            toolDispatcher_.cancelProposal(pendingConfirmation_->transactionId);
        pendingConfirmation_.reset();
        requestHandle_.reset();
        credential_.clear();
    }

    void cancelOnWorker()
    {
        if (isTerminal(working_.state))
            return;
        emitTerminalTurn(context::TurnTerminalState::cancelled);
        cancelActiveResources();
        working_.pendingProposalId.clear();
        working_.errorCode = "cancelled";
        working_.errorMessage = "Agent task was cancelled";
        setState(AgentSessionState::cancelled);
    }

    void fail(std::string code, std::string message)
    {
        if (isTerminal(working_.state))
            return;
        if (message.size() > limits::maxProtocolErrorMessageBytes)
            message.resize(limits::maxProtocolErrorMessageBytes);
        working_.pendingProposalId.clear();
        working_.errorCode = std::move(code);
        working_.errorMessage = std::move(message);
        appendTranscript({ AgentTranscriptKind::status,
            working_.errorCode + ": " + working_.errorMessage, {}, {}, false });
        emitTerminalTurn(context::TurnTerminalState::failed);
        cancelActiveResources();
        setState(AgentSessionState::failed);
    }

    void complete()
    {
        requestHandle_.reset();
        emitTerminalTurn(context::TurnTerminalState::completed);
        credential_.clear();
        working_.streamingText.clear();
        setState(AgentSessionState::completed);
    }

    model::IModelClient& modelClient_;
    tools::AgentToolDispatcher& toolDispatcher_;
    security::ICredentialStore& credentialStore_;
    std::shared_ptr<context::ContextManager> contextManager_;
    std::shared_ptr<context::ITurnSink> turnSink_;
    std::shared_ptr<ListenerBridge> listenerBridge_;
    std::shared_ptr<std::atomic_bool> callbackAlive_;

    mutable std::mutex snapshotMutex_;
    AgentSessionSnapshot publishedSnapshot_;
    AgentSessionSnapshot working_;

    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::thread worker_;

    std::mutex cancellationMutex_;
    std::shared_ptr<CancellationSource> activeCancellation_;
    std::shared_ptr<CancellationSource> cancellation_;

    uint64_t generation_ = 0;
    uint64_t requestSequence_ = 0;
    int toolIterations_ = 0;
    int consecutiveProtocolErrors_ = 0;
    AgentPreferences preferences_;
    security::SecureSecret credential_;
    std::unique_ptr<http::IRequestHandle> requestHandle_;
    std::vector<model::ModelMessage> messages_;
    std::vector<context::ConversationMessage> turnMessages_;
    std::unordered_map<std::string, std::string> toolNamesByCallId_;
    std::vector<model::ModelToolCallReady> pendingCalls_;
    std::set<std::string> pendingCallIds_;
    std::optional<PendingConfirmation> pendingConfirmation_;
    std::string roundText_;
    std::optional<std::string> roundReasoning_;
    std::string defaultPresetId_;
    std::string presetId_;
    std::string turnId_;
    std::string credentialId_;
    std::uint64_t expectedContextVersion_ = 0;
    bool terminalEmitted_ = false;
};

AgentSession::AgentSession(
    model::IModelClient& modelClient,
    tools::AgentToolDispatcher& toolDispatcher,
    security::ICredentialStore& credentialStore,
    std::shared_ptr<memory::SynthMemory> memory,
    std::shared_ptr<context::ContextManager> contextManager,
    std::shared_ptr<context::ITurnSink> turnSink)
    : impl_(std::make_unique<Impl>(modelClient, toolDispatcher, credentialStore,
          std::move(memory), std::move(contextManager), std::move(turnSink)))
{
}

AgentSession::~AgentSession() = default;

void AgentSession::start(UserAgentRequest request)
{
    impl_->start(std::move(request));
}

void AgentSession::loadConversation(context::PresetContextView view)
{
    impl_->loadConversation(std::move(view));
}

void AgentSession::confirmProposal(std::string proposalId)
{
    impl_->confirmProposal(std::move(proposalId));
}

void AgentSession::cancel() noexcept
{
    impl_->cancel();
}

void AgentSession::addListener(AgentSessionListener* listener)
{
    impl_->addListener(listener);
}

void AgentSession::removeListener(AgentSessionListener* listener)
{
    impl_->removeListener(listener);
}

AgentSessionSnapshot AgentSession::snapshot() const
{
    return impl_->snapshot();
}
}

#include "TestMessagePump.h"

#include <JuceHeader.h>
#include "PluginProcessor.h"

#include "agent/session/AgentSession.h"
#include "agent/memory/SynthMemory.h"
#include "agent/memory/MemoryMaintenanceService.h"
#include "agent/context/ContextManager.h"
#include "agent/AgentLimits.h"
#include "agent/AgentController.h"
#include "ui/AgentPanel.h"
#include "agent/model/IModelClient.h"
#include "agent/model/ChatCompletionsClient.h"
#include "agent/http/JuceHttpTransport.h"
#include "audition/AuditionAnalyzer.h"
#include "agent/tools/AgentToolDispatcher.h"
#include "audition/IAuditionService.h"
#include "security/CredentialStore.h"
#include "state/SynthStateService.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <iostream>

namespace
{
using namespace agentic_dexed;
using namespace agentic_dexed::agent;
using namespace agentic_dexed::agent::model;
using namespace agentic_dexed::agent::session;
using namespace agentic_dexed::agent::tools;
using namespace agentic_dexed::audition;
using namespace agentic_dexed::security;
using namespace std::chrono_literals;

ParameterValue defaultValue(const ParameterDefinition& definition)
{
    if (definition.kind == ParameterKind::text) return std::string();
    if (definition.kind == ParameterKind::boolean)
        return definition.numeric->defaultValue != 0.0;
    if (definition.kind == ParameterKind::real)
        return definition.numeric->defaultValue;
    return static_cast<int64_t>(std::llround(definition.numeric->defaultValue));
}

class SessionBackend final : public ISynthStateBackend
{
public:
    explicit SessionBackend(const ParameterRegistry& registry)
    {
        for (const auto& definition : registry.all())
            if (definition.kind != ParameterKind::command)
                values.emplace(definition.id, defaultValue(definition));
    }

    ParameterValue read(const ParameterDefinition& definition) const override
    {
        return values.at(definition.id);
    }

    void applyValidated(const std::vector<ParameterChange>& changes) override
    {
        ++writes;
        for (const auto& change : changes)
            values[change.parameterId] = change.after;
    }

    std::map<std::string, ParameterValue> values;
    std::atomic_int writes { 0 };
};

class SessionAudition final : public IAuditionService
{
public:
    AuditionResult audition(
        const SynthSnapshot&, const AuditionRequest& request,
        const CancellationToken& cancellation) override
    {
        ++calls;
        if (block)
        {
            entered.store(true, std::memory_order_release);
            while (!cancellation.isCancellationRequested())
                std::this_thread::sleep_for(1ms);
        }
        return { request.durationSeconds, -18.0, 0.4, 0.02, 0.4,
                 2'000.0, 6'000.0, 0.1, false, false, false, false, ReleaseObservation { 2.0, 30.0, 0.1, -120.0, false } };
    }

    std::atomic_int calls { 0 };
    std::atomic_bool entered { false };
    bool block = false;
};

class SessionSave final : public ISavePatchDelegate
{
public:
    SavePatchResult requestSave(std::string_view) override
    {
        ++calls;
        return { true, "Save queued" };
    }
    std::atomic_int calls { 0 };
};

class ScriptHandle final : public http::IRequestHandle
{
public:
    void cancel() noexcept override { cancelled.store(true, std::memory_order_release); }
    std::atomic_bool cancelled { false };
};

struct ModelScript
{
    std::vector<ModelEvent> events;
};

class ScriptedModel final : public IModelClient
{
public:
    explicit ScriptedModel(std::vector<ModelScript> scriptsToUse)
        : scripts(std::move(scriptsToUse))
    {
    }

    std::unique_ptr<http::IRequestHandle> start(
        const ModelRequest& request, ModelEventCallback callback) override
    {
        const auto index = starts.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(mutex);
            requestMessageCounts.push_back(request.messages.size());
            capturedMessages.push_back(request.messages);
            requestToolCounts.push_back(static_cast<std::size_t>(request.tools.size()));
            std::vector<std::string> names;
            for (const auto& schema : request.tools)
                if (const auto* object = schema.getDynamicObject())
                    names.push_back(object->getProperty("name").toString().toStdString());
            capturedToolNames.push_back(std::move(names));
            if (!request.messages.empty())
                firstSystemPrompt = request.messages.front().text;
        }
        if (index < scripts.size())
            for (const auto& event : scripts[index].events)
                callback(event);
        return std::make_unique<ScriptHandle>();
    }

    std::vector<ModelScript> scripts;
    std::atomic_size_t starts { 0 };
    std::mutex mutex;
    std::vector<std::size_t> requestMessageCounts;
    std::vector<std::vector<ModelMessage>> capturedMessages;
    std::vector<std::size_t> requestToolCounts;
    std::vector<std::vector<std::string>> capturedToolNames;
    std::string firstSystemPrompt;
};

class RecordingListener final : public AgentSessionListener
{
public:
    void agentSessionChanged(const AgentSessionSnapshot& snapshot) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        states.push_back(snapshot.state);
    }

    bool saw(AgentSessionState state) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::find(states.begin(), states.end(), state) != states.end();
    }

    mutable std::mutex mutex;
    std::vector<AgentSessionState> states;
};

struct SessionHarness
{
    explicit SessionHarness(std::vector<ModelScript> scripts,
        std::shared_ptr<memory::SynthMemory> memory = {},
        std::shared_ptr<context::ContextManager> contextManagerToUse = {},
        std::shared_ptr<context::ITurnSink> turnSinkToUse = {})
        : backend(registry), state(registry, backend), model(std::move(scripts)),
          dispatcher(registry, state, audition, save),
          contextManager(std::move(contextManagerToUse)), turnSink(std::move(turnSinkToUse)),
          session(model, dispatcher, credentials, std::move(memory), contextManager, turnSink)
    {
        credentials.store("provider.test", "sk-session-secret");
    }

    UserAgentRequest request(AgentApplyMode mode = AgentApplyMode::live) const
    {
        UserAgentRequest result;
        result.prompt = "Make a focused electric bass patch";
        result.credentialId = "provider.test";
        result.preferences.applyMode = mode;
        return result;
    }

    ParameterRegistry registry { ParameterRegistry::createDexed() };
    SessionBackend backend;
    SynthStateService state;
    SessionAudition audition;
    SessionSave save;
    ScriptedModel model;
    MemoryCredentialStore credentials;
    AgentToolDispatcher dispatcher;
    std::shared_ptr<context::ContextManager> contextManager;
    std::shared_ptr<context::ITurnSink> turnSink;
    AgentSession session;
};

ModelScript completedTool(
    const char* requestId, const char* callId,
    const char* name, const std::string& arguments,
    const char* text = "")
{
    ModelScript script;
    script.events.push_back(ModelStarted { requestId });
    if (*text != '\0') script.events.push_back(ModelTextDelta { text });
    script.events.push_back(ModelToolCallReady { callId, name, arguments });
    script.events.push_back(ModelCompleted { std::string(requestId) + "-response" });
    return script;
}

ModelScript completedText(const char* requestId, const char* text)
{
    return { { ModelStarted { requestId }, ModelTextDelta { text },
               ModelCompleted { std::string(requestId) + "-response" } } };
}

std::string stateArguments()
{
    return R"({"scope":"all","group":null,"ids":null})";
}

std::string operatorOneDiscoveryArguments()
{
    return R"({"group":null,"operator_number":1,"ids":["algorithm","feedback","lfo_rate","lfo_wave","lfo_pitch_mod_depth","osc_mode","transpose","master_tune","cutoff"]})";
}

std::string operatorTwoDiscoveryArguments()
{
    return R"({"group":null,"operator_number":2,"ids":["envelope","output_level","frequency_ratio","detune","key_scale","rate","level_start","level_end"]})";
}

std::string patchArguments(
    const char* transactionId, int revision,
    const char* parameterId, int value)
{
    return "{\"transaction_id\":\"" + std::string(transactionId)
        + "\",\"base_revision\":" + std::to_string(revision)
        + ",\"reason\":\"session test\",\"mode\":\"live\",\"operations\":[{\"parameter_id\":\""
        + parameterId + "\",\"value\":" + std::to_string(value) + "}]}";
}

std::string auditionArguments()
{
    return R"({"phrase":"single_note","midi_note":48,"velocity":100,"duration_seconds":0.2})";
}

bool pumpUntil(
    AgentSession& session,
    const std::function<bool(const AgentSessionSnapshot&)>& predicate,
    std::chrono::milliseconds timeout = 2s)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end)
    {
        agentic_dexed::test::pumpMessagesFor(2);
        if (predicate(session.snapshot()))
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate(session.snapshot());
}

bool terminal(const AgentSessionSnapshot& snapshot)
{
    return snapshot.state == AgentSessionState::completed
        || snapshot.state == AgentSessionState::cancelled
        || snapshot.state == AgentSessionState::failed;
}

class LiveAgentTests final : public juce::UnitTest
{
public:
    LiveAgentTests() : juce::UnitTest("Real DeepSeek sound design", "LiveAgent") {}
    void runTest() override
    {
        beginTest("Real streaming model must commit an audible long-release pad");
        const auto environmentKey = juce::SystemStats::getEnvironmentVariable("DEEPSEEK_API_KEY", "");
        SecureSecret key(environmentKey.toStdString());
        if (key.empty())
        {
            auto platformStore = createPlatformCredentialStore();
            auto stored = platformStore->load("agent.model");
            if (stored.ok()) key = std::move(stored.secret);
        }
        expect(!key.empty(), "Save the DeepSeek key in Agent settings, or set DEEPSEEK_API_KEY");
        if (key.empty()) return;
        DexedAudioProcessor processor(true);
        const auto program = juce::SystemStats::getEnvironmentVariable("DEEPSEEK_TEST_PROGRAM", "");
        if (program.isNotEmpty()) processor.setCurrentProgram(program.getIntValue());
        auto& state = processor.synthStateService();
        const auto& registry = state.registry();
        const auto initialRevision = state.revision();
        const auto initialState = state.snapshot({});
        std::cout << "LIVE initial patch=" << state.snapshot({ SnapshotScopeKind::all, {}, {} }).patchName << std::endl;
        AuditionAnalyzer audition;
        auto credentials = std::make_unique<MemoryCredentialStore>();
        credentials->store("agent.model", key.view());
        AgentController controller(registry, state, std::make_unique<http::JuceHttpTransport>(), std::move(credentials));
        auto& session = controller.session();
        ui::AgentPanel panel(controller, state);
        panel.setSize(1200, 560);
        UserAgentRequest request;
        request.credentialId = "agent.model";
        request.prompt = u8"空灵的pad，余音很长";
        request.preferences.protocol = ProviderProtocol::chatCompletions;
        request.preferences.baseUrl = "https://api.deepseek.com";
        request.preferences.model = "deepseek-flash";
        panel.setPreferences(request.preferences);
        panel.promptEditor().setText(juce::String::fromUTF8(request.prompt.c_str()), false);
        expect(panel.console().handlePromptKeyForTest(juce::KeyPress(juce::KeyPress::returnKey)));

        const auto deadline = std::chrono::steady_clock::now() + 10min;
        std::size_t observed = 0;
        bool finished = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            test::pumpMessagesFor(10);
            const auto snapshot = session.snapshot();
            while (observed < snapshot.transcript.size())
            {
                const auto& entry = snapshot.transcript[observed++];
                if (entry.kind == AgentTranscriptKind::toolCall)
                    std::cout << "LIVE tool: " << entry.toolName << std::endl;
                if (entry.kind == AgentTranscriptKind::toolResult && !entry.success)
                    std::cout << "LIVE tool failure: " << entry.text << std::endl;
            }
            if (terminal(snapshot)) { finished = true; break; }
            std::this_thread::sleep_for(10ms);
        }
        if (!finished) session.cancel();
        expect(finished, "Live agent timed out");
        const auto result = session.snapshot();
        std::cout << "LIVE rounds=" << result.toolIterations << " error=" << result.errorCode
                  << " message=" << result.errorMessage << std::endl;
        std::cout << "LIVE final=" << result.finalText << std::endl;
        expect(result.state == AgentSessionState::completed, result.errorMessage);
        expect(!result.limitReached, "Live agent exhausted its tool budget");
        expect(state.revision() > initialRevision, "Model must actually change synth parameters");
        expect(!result.finalText.empty(), "Model must explain the resulting patch");
        bool committed = false;
        for (const auto& transaction : result.transactions)
            committed = committed || transaction.status == "committed";
        expect(committed, "A committed transaction is required");
        if (committed)
        {
            const auto snapshot = state.snapshot({ SnapshotScopeKind::all, {}, {} });
            CancellationSource cancellation;
            OfflinePatchRenderer renderer;
            const auto rendered = renderer.render(snapshot, { "major_chord", 60, 90, 10.0 }, cancellation.token());
            const auto metrics = AuditionAnalyzer::analyzeBuffer(rendered.audio, rendered.sampleRate, cancellation.token());
            std::cout << "LIVE audio peak=" << metrics.peak << " rms=" << metrics.rmsLufsProxy << std::endl;
            expect(!metrics.silent, "Generated patch must produce sound");
            expect(!metrics.nonFinite && !metrics.clipped, "Generated patch must remain finite and unclipped");
            // Note-off occurs at seven seconds; verify audible sound two seconds later.
            const auto tailStart = static_cast<int>(9.0 * rendered.sampleRate);
            const auto tailRms = rendered.audio.getRMSLevel(0, tailStart, rendered.audio.getNumSamples() - tailStart);
            std::cout << "LIVE release tail rms=" << tailRms << std::endl;
            expect(tailRms > 0.0001f, "Long-release request must leave an audible two-second tail");
        }
        panel.flushPendingForTest();
        expect(!panel.console().displayText().contains("TOOL "));
        expect(!panel.console().displayText().contains("DATA "));
        expect(!panel.console().displayText().contains("parameter_id"));
        if (committed)
        {
            const auto tail = audition.audition(state.snapshot({ SnapshotScopeKind::all, {}, {} }),
                { "release_check", 60, 100, 32.0 }, {});
            expect(tail.release.has_value() && !tail.release->signalAtEnd,
                   "Generated long-release pad must settle after note-off");
        }
        if (result.state != AgentSessionState::completed) return;

        // Continue against the same processor/session, using state assertions rather than prose.
        const auto waitForRequest = [&](bool allowProposal)
        {
            const auto until = std::chrono::steady_clock::now() + 5min;
            while (std::chrono::steady_clock::now() < until)
            {
                test::pumpMessagesFor(10);
                const auto current = session.snapshot();
                const bool currentRequest = !current.transcript.empty()
                    && current.transcript.front().kind == AgentTranscriptKind::user
                    && current.transcript.front().text == request.prompt;
                if (currentRequest && (terminal(current)
                    || (allowProposal && current.state == AgentSessionState::awaitingConfirmation)))
                    return true;
                std::this_thread::sleep_for(10ms);
            }
            session.cancel();
            return false;
        };
        const auto readState = [&] { return state.snapshot({ SnapshotScopeKind::all, {}, {} }); };
        const auto expectOnlyOutput = [&](const SynthSnapshot& before, double expected)
        {
            const auto after = readState();
            expectWithinAbsoluteError(std::get<double>(after.values.at("global.output")), expected, 0.00001);
            for (const auto& entry : before.values)
                if (entry.first != "global.output")
                    expect(after.values.at(entry.first) == entry.second,
                           "Unrequested mutation: " + juce::String(entry.first));
        };
        const auto logResult = [&](const char* stage)
        {
            const auto current = session.snapshot();
            std::cout << "LIVE " << stage << " rounds=" << current.toolIterations
                      << " error=" << current.errorCode << " final=" << current.finalText << std::endl;
        };

        beginTest("Real provider follows a second precise request without changing other parameters");
        const auto beforeSecond = readState();
        request.prompt = u8"只把主输出 global.output 设为 0.35。所有其他参数和音色名称保持原值。";
        controller.start(request);
        expect(waitForRequest(false), "Second request timed out");
        logResult("second request");
        expect(session.snapshot().state == AgentSessionState::completed);
        expectOnlyOutput(beforeSecond, 0.35);
        if (session.snapshot().state != AgentSessionState::completed) return;

        beginTest("Real provider confirmation changes nothing until approved and then resumes");
        const auto beforeConfirmation = readState();
        request.preferences.applyMode = AgentApplyMode::confirmation;
        request.prompt = u8"只把主输出 global.output 设为 0.45。所有其他参数和音色名称保持原值。";
        controller.start(request);
        expect(waitForRequest(true), "Confirmation proposal timed out");
        expect(session.snapshot().state == AgentSessionState::awaitingConfirmation);
        expect(readState().values == beforeConfirmation.values, "A proposal must not mutate the patch");
        expect(state.revision() == beforeConfirmation.revision);
        if (session.snapshot().state != AgentSessionState::awaitingConfirmation) return;
        session.confirmProposal(session.snapshot().pendingProposalId);
        expect(waitForRequest(false), "Confirmed request failed to resume");
        logResult("confirmed request");
        expect(session.snapshot().state == AgentSessionState::completed);
        expectOnlyOutput(beforeConfirmation, 0.45);
        if (session.snapshot().state != AgentSessionState::completed) return;

        beginTest("Cancelling a real provider proposal leaves every parameter and revision unchanged");
        const auto beforeCancellation = readState();
        request.prompt = u8"只把主输出 global.output 设为 0.55。所有其他参数和音色名称保持原值。";
        controller.start(request);
        expect(waitForRequest(true), "Cancellation proposal timed out");
        expect(session.snapshot().state == AgentSessionState::awaitingConfirmation);
        panel.flushPendingForTest();
        panel.undoButton().onClick();
        expect(waitForRequest(false), "Cancellation did not terminate");
        test::pumpMessagesFor(30);
        panel.flushPendingForTest();
        logResult("cancelled request");
        expect(session.snapshot().state == AgentSessionState::cancelled);
        expect(readState().values == beforeCancellation.values);
        expect(state.revision() == beforeCancellation.revision);

        beginTest("Back restores each complete real user turn, including all sound-design commits");
        panel.undoButton().onClick();
        expect(readState().values == beforeConfirmation.values);
        panel.undoButton().onClick();
        expect(readState().values == beforeSecond.values);
        panel.undoButton().onClick();
        expect(readState().values == initialState.values);
        expect(!panel.undoButton().isEnabled());
    }
};

LiveAgentTests liveAgentTests;

class AgentSessionTests final : public juce::UnitTest
{
public:
    AgentSessionTests() : juce::UnitTest("Bounded Agent session", "AgentSession") {}

    void runTest() override
    {
        beginTest("Consecutive requests restore the same preset and expose no memory write tool");
        {
            const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("sbfad-session-context-" + juce::Uuid().toString());
            auto store = std::make_shared<context::ConversationContextStore>(directory);
            auto manager = std::make_shared<context::ContextManager>(store);
            const auto preset = juce::Uuid().toString().toStdString();
            SessionHarness first({ completedText("first", u8"已经做成温暖柔和的 pad") }, {}, manager, manager);
            auto request = first.request();
            request.presetId = preset;
            request.prompt = u8"做一个温暖柔和的 pad";
            first.session.start(request);
            expect(pumpUntil(first.session, terminal));
            expect(first.session.snapshot().state == AgentSessionState::completed);
            expectEquals(first.backend.writes.load(), 0);
            {
                std::lock_guard<std::mutex> lock(first.model.mutex);
                expect(std::none_of(first.model.capturedToolNames.front().begin(),
                                    first.model.capturedToolNames.front().end(),
                    [](const auto& name) { return name == "update_synth_memory"; }));
            }

            SessionHarness next({ completedText("next", u8"已经进一步调亮") }, {}, manager, manager);
            auto overrideRequest = next.request();
            overrideRequest.presetId = preset;
            overrideRequest.prompt = u8"这一次稍微明亮一点";
            next.session.start(overrideRequest);
            expect(pumpUntil(next.session, terminal));
            {
                std::lock_guard<std::mutex> lock(next.model.mutex);
                std::string combined;
                for (const auto& message : next.model.capturedMessages.front())
                    combined += message.text + "\n";
                expect(combined.find(u8"做一个温暖柔和的 pad") != std::string::npos);
                expect(combined.find(u8"已经做成温暖柔和的 pad") != std::string::npos);
                expectEquals(next.model.capturedMessages.front().back().text, overrideRequest.prompt);
            }
            const auto transcript = next.session.snapshot().transcript;
            expect(transcript.size() >= 4);
            expect(std::all_of(transcript.begin(), transcript.end(), [](const auto& entry) {
                return entry.kind != AgentTranscriptKind::toolCall
                    && entry.kind != AgentTranscriptKind::toolResult;
            }));

            SessionHarness isolated({ completedText("other", "Done") }, {}, manager, manager);
            auto other = isolated.request();
            other.presetId = juce::Uuid().toString().toStdString();
            other.prompt = "different preset";
            isolated.session.start(other);
            expect(pumpUntil(isolated.session, terminal));
            {
                std::lock_guard<std::mutex> lock(isolated.model.mutex);
                std::string combined;
                for (const auto& message : isolated.model.capturedMessages.front())
                    combined += message.text;
                expect(combined.find(u8"温暖柔和") == std::string::npos);
            }
            expect(directory.deleteRecursively());
        }

        beginTest("Session preserves one assistant turn with reasoning and parallel calls");
        ModelScript parallel {{ ModelStarted { "parallel" },
            ModelReasoningDelta { "opaque-continuation" }, ModelTextDelta { "Checking" },
            ModelToolCallReady { "a", "get_synth_state", stateArguments() },
            ModelToolCallReady { "b", "describe_parameters", "{}" },
            ModelCompleted { "parallel" } }};
        SessionHarness grouped({ parallel, completedText("end", "Done") });
        grouped.session.start(grouped.request());
        expect(pumpUntil(grouped.session, terminal));
        {
            std::lock_guard<std::mutex> lock(grouped.model.mutex);
            expectEquals(static_cast<int>(grouped.model.capturedMessages.size()), 2);
            if (grouped.model.capturedMessages.size() == 2)
            {
                const auto& messages = grouped.model.capturedMessages[1];
                expectEquals(static_cast<int>(messages.size()), 5);
                expectEquals(messages[2].reasoningContent.value_or(""), std::string("opaque-continuation"));
                expectEquals(static_cast<int>(messages[2].toolCalls.size()), 2);
                expect(messages[3].toolResult.has_value());
                expect(messages[4].toolResult.has_value());
            }
        }
        for (const auto& entry : grouped.session.snapshot().transcript)
            expect(entry.text.find("opaque-continuation") == std::string::npos);

        beginTest("Empty completion is a visible failure, not success");
        SessionHarness empty({ completedText("empty", "") });
        empty.session.start(empty.request());
        expect(pumpUntil(empty.session, terminal));
        const auto emptyResult = empty.session.snapshot();
        expect(emptyResult.state == AgentSessionState::failed);
        expectEquals(emptyResult.errorCode, std::string("empty_response"));
        expect(!emptyResult.transcript.empty() && !emptyResult.transcript.back().success);

        beginTest("live mode completes a four-tool sound-design loop");
        SessionHarness happy({
            completedTool("r1", "state-call", "get_synth_state", stateArguments(), "Inspecting. "),
            completedTool("r2", "patch-1", "apply_parameter_patch",
                          patchArguments("txn-1", 0, "global.algorithm", 7)),
            completedTool("r3", "listen-1", "audition_patch", auditionArguments()),
            completedTool("r4", "patch-2", "apply_parameter_patch",
                          patchArguments("txn-2", 1, "global.feedback", 5)),
            completedText("r5", "Created a focused electric bass with controlled feedback.")
        });
        RecordingListener listener;
        happy.session.addListener(&listener);
        happy.session.start(happy.request());
        expect(pumpUntil(happy.session, terminal));
        agentic_dexed::test::pumpMessagesFor(20);
        const auto happyResult = happy.session.snapshot();
        expect(happyResult.state == AgentSessionState::completed);
        expectEquals(happyResult.toolIterations, 4);
        expectEquals(happy.backend.writes.load(), 2);
        expectEquals(happy.audition.calls.load(), 3); // two preflight release checks plus requested audition
        expectEquals(static_cast<int>(happyResult.transactions.size()), 2);
        expect(std::all_of(
            happyResult.transactions.begin(), happyResult.transactions.end(),
            [](const auto& item) { return item.status == "committed"; }));
        expect(happyResult.finalText.find("electric bass") != std::string::npos);
        expect(happyResult.streamingText.empty(),
               "completed sessions must not retain the final streaming buffer");
        std::size_t transcriptBytes = 0;
        for (const auto& entry : happyResult.transcript)
        {
            transcriptBytes += entry.text.size() + entry.callId.size() + entry.toolName.size();
            expect(entry.text.find("sk-session-secret") == std::string::npos);
        }
        expect(transcriptBytes <= 128u * 1024u);
        expect(listener.saw(AgentSessionState::requesting));
        expect(listener.saw(AgentSessionState::streaming));
        expect(listener.saw(AgentSessionState::executingTool));
        expect(listener.saw(AgentSessionState::completed));
        const auto linkedResults = std::count_if(
            happyResult.transcript.begin(), happyResult.transcript.end(),
            [](const auto& item) {
                return item.kind == AgentTranscriptKind::toolResult && !item.callId.empty();
            });
        expectEquals(static_cast<int>(linkedResults), 4);
        expect(happy.model.firstSystemPrompt.find("Only use registered tools")
               != std::string::npos);
        expect(happy.model.firstSystemPrompt.find("Copy canonical parameter IDs verbatim")
               != std::string::npos);
        expect(happy.model.firstSystemPrompt.find("descriptive search terms")
               != std::string::npos);
        expect(happy.model.firstSystemPrompt.find("LANGUAGE REQUIREMENT")
               != std::string::npos);
        expect(happy.model.firstSystemPrompt.find("latest user request contains Chinese")
               == std::string::npos);
        happy.session.removeListener(&listener);

        beginTest("Chinese requests receive an explicit per-request language requirement");
        SessionHarness chinese({ completedText("zh1", "已生成空灵的音色。") });
        auto chineseRequest = chinese.request();
        chineseRequest.prompt = "空灵的pad";
        chinese.session.start(std::move(chineseRequest));
        expect(pumpUntil(chinese.session, terminal));
        expect(chinese.session.snapshot().state == AgentSessionState::completed);
        expect(chinese.model.firstSystemPrompt.find(
                   "latest user request contains Chinese characters") != std::string::npos);
        expect(chinese.model.firstSystemPrompt.find(
                   "before the first tool call") != std::string::npos);

        beginTest("reported Chinese pad workflow completes all combined discovery rounds");
        SessionHarness padWorkflow({
            completedTool("pad1", "discover-op1", "describe_parameters",
                          operatorOneDiscoveryArguments(), "我先查询参数。"),
            completedTool("pad2", "discover-op2", "describe_parameters",
                          operatorTwoDiscoveryArguments()),
            completedTool("pad3", "read-state", "get_synth_state", stateArguments()),
            completedTool("pad4", "apply-pad", "apply_parameter_patch",
                          patchArguments("pad-txn", 0, "global.algorithm", 12)),
            completedText("pad5", "已经生成空灵的 Pad，请试听延音和动态。")
        });
        auto padRequest = padWorkflow.request();
        padRequest.prompt = "空灵的pad";
        padWorkflow.session.start(std::move(padRequest));
        expect(pumpUntil(padWorkflow.session, terminal));
        const auto padResult = padWorkflow.session.snapshot();
        expect(padResult.state == AgentSessionState::completed);
        expectEquals(padResult.toolIterations, 4);
        expectEquals(padWorkflow.backend.writes.load(), 1);
        expect(padResult.finalText.find("空灵") != std::string::npos);
        expect(std::none_of(
            padResult.transcript.begin(), padResult.transcript.end(), [](const auto& item) {
                return item.kind == AgentTranscriptKind::toolResult
                    && item.text.find("invalid_arguments") != std::string::npos;
            }));

        beginTest("confirmation mode proposes before committing and resumes afterward");
        SessionHarness confirmation({
            completedTool("c1", "confirm-call", "apply_parameter_patch",
                          patchArguments("confirm-txn", 0, "global.algorithm", 9)),
            completedText("c2", "The confirmed change is complete.")
        });
        confirmation.session.start(confirmation.request(AgentApplyMode::confirmation));
        expect(pumpUntil(confirmation.session, [](const auto& snapshot) {
            return snapshot.state == AgentSessionState::awaitingConfirmation;
        }));
        expectEquals(confirmation.backend.writes.load(), 0);
        expectEquals(confirmation.session.snapshot().pendingProposalId,
                     std::string("confirm-txn"));
        confirmation.session.confirmProposal("confirm-txn");
        expect(pumpUntil(confirmation.session, terminal));
        expect(confirmation.session.snapshot().state == AgentSessionState::completed);
        expectEquals(confirmation.backend.writes.load(), 1);
        expectEquals(confirmation.session.snapshot().transactions.front().status,
                     std::string("committed"));
        {
            std::lock_guard<std::mutex> lock(confirmation.model.mutex);
            const auto& messages = confirmation.model.capturedMessages.back();
            const auto result = juce::JSON::parse(messages.back().toolResult->output);
            expect(static_cast<bool>(result["user_confirmed"]),
                   "Continuation must explicitly tell the model that approval has already happened");
            expectEquals(result["status"].toString(), juce::String("committed"));
        }

        beginTest("missing credentials fail before starting the model");
        SessionHarness missing({ completedText("unused", "unused") });
        auto missingRequest = missing.request();
        missingRequest.credentialId = "provider.missing";
        missing.session.start(std::move(missingRequest));
        expect(pumpUntil(missing.session, terminal));
        expect(missing.session.snapshot().state == AgentSessionState::failed);
        expectEquals(missing.session.snapshot().errorCode, std::string("missing_credential"));
        expectEquals(missing.model.starts.load(), std::size_t { 0 });

        beginTest("two malformed tool calls stop the session without side effects");
        SessionHarness malformed({
            completedTool("m1", "bad-json-1", "apply_parameter_patch", "{"),
            completedTool("m2", "bad-json-2", "apply_parameter_patch", "not-json")
        });
        malformed.session.start(malformed.request());
        expect(pumpUntil(malformed.session, terminal));
        expect(malformed.session.snapshot().state == AgentSessionState::failed);
        expectEquals(malformed.session.snapshot().errorCode,
                     std::string("too_many_protocol_errors"));
        expectEquals(malformed.backend.writes.load(), 0);

        beginTest("stale revisions are returned to the model for recovery");
        SessionHarness recovery({
            completedTool("s1", "stale", "apply_parameter_patch",
                          patchArguments("stale-txn", 99, "global.algorithm", 11)),
            completedTool("s2", "fresh", "apply_parameter_patch",
                          patchArguments("fresh-txn", 0, "global.algorithm", 11)),
            completedText("s3", "Recovered from the revision conflict.")
        });
        recovery.session.start(recovery.request());
        expect(pumpUntil(recovery.session, terminal));
        const auto recoveryResult = recovery.session.snapshot();
        expect(recoveryResult.state == AgentSessionState::completed);
        expectEquals(recovery.backend.writes.load(), 1);
        expect(std::any_of(
            recoveryResult.transcript.begin(), recoveryResult.transcript.end(),
            [](const auto& item) {
                return item.kind == AgentTranscriptKind::toolResult
                    && item.text.find("stale_revision") != std::string::npos;
            }));

        beginTest("exhausting the tool budget reports unfinished work as a failure");
        std::vector<ModelScript> limitScripts;
        for (int index = 0; index <= limits::maxToolIterations; ++index)
            limitScripts.push_back(completedTool(
                ("l" + std::to_string(index)).c_str(),
                ("limit-" + std::to_string(index)).c_str(),
                "get_synth_state", stateArguments()));
        SessionHarness limited(std::move(limitScripts));
        limited.session.start(limited.request());
        expect(pumpUntil(limited.session, terminal));
        const auto limitedResult = limited.session.snapshot();
        expect(limitedResult.state == AgentSessionState::failed);
        expect(limitedResult.limitReached);
        expectEquals(limitedResult.toolIterations, limits::maxToolIterations);
        expectEquals(limited.model.starts.load(), static_cast<std::size_t>(limits::maxToolIterations + 1));
        {
            std::lock_guard<std::mutex> lock(limited.model.mutex);
            expectEquals(limited.model.requestToolCounts.back(), std::size_t { 0 });
        }

        beginTest("duplicate tool events do not duplicate mutations");
        auto duplicate = completedTool(
            "d1", "duplicate-call", "apply_parameter_patch",
            patchArguments("duplicate-txn", 0, "global.algorithm", 13));
        duplicate.events.insert(
            duplicate.events.end() - 1,
            ModelToolCallReady { "duplicate-call", "apply_parameter_patch",
                                 patchArguments("duplicate-txn", 0, "global.algorithm", 13) });
        SessionHarness deduplicated({ duplicate, completedText("d2", "Done.") });
        deduplicated.session.start(deduplicated.request());
        expect(pumpUntil(deduplicated.session, terminal));
        expectEquals(deduplicated.backend.writes.load(), 1);

        beginTest("provider disconnects fail before or after a retained transaction");
        SessionHarness disconnectedBefore({ { {
            ModelStarted { "e1" },
            ModelFailed { makeProtocolError("incomplete_stream", "Disconnected") }
        } } });
        disconnectedBefore.session.start(disconnectedBefore.request());
        expect(pumpUntil(disconnectedBefore.session, terminal));
        expect(disconnectedBefore.session.snapshot().state == AgentSessionState::failed);
        expectEquals(disconnectedBefore.backend.writes.load(), 0);

        SessionHarness disconnectedAfter({
            completedTool("e2", "kept-call", "apply_parameter_patch",
                          patchArguments("kept-txn", 0, "global.algorithm", 15)),
            { { ModelStarted { "e3" },
                ModelFailed { makeProtocolError("incomplete_stream", "Disconnected") } } }
        });
        disconnectedAfter.session.start(disconnectedAfter.request());
        expect(pumpUntil(disconnectedAfter.session, terminal));
        expect(disconnectedAfter.session.snapshot().state == AgentSessionState::failed);
        expectEquals(disconnectedAfter.backend.writes.load(), 1);
        expectEquals(static_cast<int>(
            disconnectedAfter.session.snapshot().transactions.size()), 1);

        beginTest("cancellation reaches requesting, streaming, tools, and confirmation");
        SessionHarness requesting({ {} });
        requesting.session.start(requesting.request());
        expect(pumpUntil(requesting.session, [](const auto& snapshot) {
            return snapshot.state == AgentSessionState::requesting;
        }));
        requesting.session.cancel();
        expect(pumpUntil(requesting.session, terminal));
        expect(requesting.session.snapshot().state == AgentSessionState::cancelled);

        SessionHarness streaming({ { { ModelStarted { "hold-stream" } } } });
        streaming.session.start(streaming.request());
        expect(pumpUntil(streaming.session, [](const auto& snapshot) {
            return snapshot.state == AgentSessionState::streaming;
        }));
        streaming.session.cancel();
        expect(pumpUntil(streaming.session, terminal));
        expect(streaming.session.snapshot().state == AgentSessionState::cancelled);

        SessionHarness executing({
            completedTool("x1", "blocking-audition", "audition_patch", auditionArguments())
        });
        executing.audition.block = true;
        executing.session.start(executing.request());
        expect(pumpUntil(executing.session, [&executing](const auto& snapshot) {
            return snapshot.state == AgentSessionState::executingTool
                && executing.audition.entered.load(std::memory_order_acquire);
        }));
        executing.session.cancel();
        expect(pumpUntil(executing.session, terminal));
        expect(executing.session.snapshot().state == AgentSessionState::cancelled);

        SessionHarness awaiting({
            completedTool("a1", "pending-call", "apply_parameter_patch",
                          patchArguments("pending-txn", 0, "global.algorithm", 17))
        });
        awaiting.session.start(awaiting.request(AgentApplyMode::confirmation));
        expect(pumpUntil(awaiting.session, [](const auto& snapshot) {
            return snapshot.state == AgentSessionState::awaitingConfirmation;
        }));
        awaiting.session.cancel();
        expect(pumpUntil(awaiting.session, terminal));
        expect(awaiting.session.snapshot().state == AgentSessionState::cancelled);
        expectEquals(awaiting.backend.writes.load(), 0);
    }
};

AgentSessionTests agentSessionTests;

class LiveMemoryTests final : public juce::UnitTest
{
public:
    LiveMemoryTests() : juce::UnitTest("Real provider automatic memory", "LiveMemory") {}
    void runTest() override
    {
        beginTest("Real DeepSeek selectively curates preferences and compacts per-preset context");
        auto environmentKey = juce::SystemStats::getEnvironmentVariable("DEEPSEEK_API_KEY", "");
        SecureSecret key(environmentKey.toStdString());
        environmentKey = {};
        if (key.empty())
        {
            auto platformStore = createPlatformCredentialStore();
            auto stored = platformStore->load("agent.model");
            if (stored.ok()) key = std::move(stored.secret);
        }
        expect(!key.empty(), "Save the DeepSeek key in Agent settings, or set DEEPSEEK_API_KEY");
        if (key.empty()) return;
        const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile("sbfad-live-memory", "", false);
        struct Cleanup { juce::File directory; ~Cleanup() { directory.deleteRecursively(); } } cleanup { directory };
        directory.createDirectory();
        auto contextStore = std::make_shared<context::ConversationContextStore>(directory);
        auto contextManager = std::make_shared<context::ContextManager>(contextStore);
        ParameterRegistry registry = ParameterRegistry::createDexed();
        SessionBackend backend(registry);
        SynthStateService state(registry, backend);
        SessionAudition audition;
        SessionSave save;
        AgentToolDispatcher dispatcher(registry, state, audition, save);
        MemoryCredentialStore credentials;
        credentials.store("provider.test", key.view());
        key.clear();
        http::JuceHttpTransport transport;
        ChatCompletionsClient model(transport);
        auto maintenance = std::make_shared<memory::MemoryMaintenanceService>(
            model, credentials, contextStore, contextManager);

        AgentPreferences preferences;
        preferences.protocol = ProviderProtocol::chatCompletions;
        preferences.baseUrl = "https://api.deepseek.com";
        preferences.model = "deepseek-flash";

        auto terminalTurn = [&](const std::string& presetId,
                                std::uint64_t version,
                                std::string id,
                                std::string prompt) {
            context::TerminalTurn terminal;
            terminal.presetId = presetId;
            terminal.expectedVersion = version;
            terminal.provider = preferences.providerConfig();
            terminal.credentialId = "provider.test";
            terminal.turn.turnId = std::move(id);
            terminal.turn.messages = {
                { context::ConversationRole::user, std::move(prompt) },
                { context::ConversationRole::assistant, u8"已完成当前请求" }
            };
            maintenance->onTurnFinished(std::move(terminal));
            expect(maintenance->waitUntilIdle(180s), "Background memory request timed out");
        };

        const auto originalPreset = juce::Uuid().toString().toStdString();
        terminalTurn(originalPreset, 0, "live-one-off", u8"这次做一个明亮的 pad");
        const auto afterOneOff = contextStore->loadPreferences();
        expect(afterOneOff.ok);
        expect(!afterOneOff.text.contains(u8"明亮"),
               "A single one-off request must not become a durable preference");

        terminalTurn(originalPreset, 1, "live-lasting",
            u8"我一般偏好温暖柔和、低频饱满的 pad，避免刺耳高频");
        const auto saved = contextStore->loadPreferences();
        expect(saved.ok && saved.text.contains("- ["),
               "An explicit lasting preference must be curated automatically");
        expect(saved.text.contains(u8"温暖") || saved.text.contains(u8"柔和")
               || saved.text.contains(u8"低频"));

        AgentSession session(
            model, dispatcher, credentials, {}, contextManager, maintenance);
        auto run = [&](const std::string& presetId, const char* prompt) {
            UserAgentRequest request;
            request.presetId = presetId;
            request.prompt = prompt;
            request.credentialId = "provider.test";
            request.preferences = preferences;
            session.start(request);
            expect(pumpUntil(session, terminal, 180s), "Live memory request timed out");
            const auto snapshot = session.snapshot();
            expect(snapshot.state == AgentSessionState::completed, juce::String(snapshot.errorCode));
            expect(maintenance->waitUntilIdle(180s), "Background maintenance timed out");
            return snapshot;
        };

        const auto newPreset = juce::Uuid().toString().toStdString();
        const auto builtForNewPreset = contextManager->buildRequestContext(
            newPreset, u8"我的长期 pad 偏好是什么？", "PRIMARY SYSTEM");
        expect(builtForNewPreset.ok);
        std::string newPresetInput;
        for (const auto& message : builtForNewPreset.messages)
            newPresetInput += message.text + "\n";
        expect(newPresetInput.find(u8"温暖") != std::string::npos
               || newPresetInput.find(u8"柔和") != std::string::npos);
        expect(newPresetInput.find(u8"这次做一个明亮的 pad") == std::string::npos,
               "A new preset must not inherit another preset's conversation");
        const auto recalled = run(newPreset,
            u8"我的长期 pad 音色偏好是什么？只用中文自然语言回答，不修改音色。");
        expect(!recalled.finalText.empty());
        expect(recalled.finalText.find(u8"温暖") != std::string::npos
               || recalled.finalText.find(u8"柔和") != std::string::npos
               || recalled.finalText.find(u8"低频") != std::string::npos);
        expectEquals(backend.writes.load(), 0);

        const auto longPreset = juce::Uuid().toString().toStdString();
        context::PresetConversationContext longContext;
        longContext.presetId = longPreset;
        for (int index = 0; index < 12; ++index)
        {
            context::ConversationTurn turn;
            turn.turnId = "live-history-" + std::to_string(index);
            turn.messages = {
                { context::ConversationRole::user,
                  u8"逐步把 pad 做得更柔和，步骤 " + std::to_string(index) },
                { context::ConversationRole::assistant,
                  u8"已保留柔和方向，步骤 " + std::to_string(index) }
            };
            longContext.recentTurns.push_back(std::move(turn));
        }
        expect(contextStore->commit(longContext, 0).status
               == context::ContextCommitStatus::committed);
        terminalTurn(longPreset, 1, "live-history-12",
            u8"继续整理这个 pad 的空间感");
        const auto compacted = contextStore->load(longPreset);
        expect(compacted.ok && compacted.exists);
        expectEquals(static_cast<int>(compacted.context.recentTurns.size()), 8);
        expect(compacted.context.summary.size() > 0);
        expectEquals(compacted.context.recentTurns.back().turnId,
                     std::string("live-history-12"));
        const auto afterCompaction = contextManager->buildRequestContext(
            longPreset, u8"继续刚才的方向", "PRIMARY SYSTEM");
        expect(afterCompaction.ok);
        expect(!afterCompaction.context.empty());
        expectEquals(static_cast<int>(afterCompaction.context->recentTurns.size()), 8);

        maintenance->shutdown();
        expect(directory.exists(), "Temporary data should exist until test cleanup");
    }
} liveMemoryTests;
}

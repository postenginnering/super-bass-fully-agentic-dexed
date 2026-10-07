#include <JuceHeader.h>

#include "agent/memory/MemoryMaintenanceService.h"
#include "agent/context/ConversationContextStore.h"
#include "agent/context/ContextManager.h"
#include "agent/model/IModelClient.h"
#include "security/CredentialStore.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace {
using namespace agentic_dexed::agent;
using namespace agentic_dexed::agent::context;
using namespace agentic_dexed::agent::memory;
using namespace agentic_dexed::agent::model;

class MaintenanceHandle final : public http::IRequestHandle {
public:
    void cancel() noexcept override { cancelled = true; }
    std::atomic_bool cancelled { false };
};

class SharedMaintenanceHandle final : public http::IRequestHandle {
public:
    explicit SharedMaintenanceHandle(std::shared_ptr<std::atomic_bool> cancelled)
        : cancelled_(std::move(cancelled)) {}
    void cancel() noexcept override { cancelled_->store(true); }
private:
    std::shared_ptr<std::atomic_bool> cancelled_;
};

class BlockingMaintenanceModel final : public IModelClient {
public:
    std::unique_ptr<http::IRequestHandle> start(
        const ModelRequest&, ModelEventCallback) override
    {
        starts.fetch_add(1);
        return std::make_unique<SharedMaintenanceHandle>(cancelled);
    }
    std::atomic_int starts { 0 };
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
};

class MaintenanceModel final : public IModelClient {
public:
    std::unique_ptr<http::IRequestHandle> start(
        const ModelRequest& request, ModelEventCallback callback) override
    {
        std::string response;
        {
            std::lock_guard<std::mutex> lock(mutex);
            providers.push_back(request.provider);
            credentials.emplace_back(request.authorization);
            toolCounts.push_back(static_cast<std::size_t>(request.tools.size()));
            messages.push_back(request.messages);
            if (nextResponse < responses.size())
                response = responses[nextResponse++];
        }
        callback(ModelStarted { "maintenance" });
        if (!response.empty())
            callback(ModelTextDelta { response });
        callback(ModelCompleted { "maintenance-complete" });
        return std::make_unique<MaintenanceHandle>();
    }

    std::mutex mutex;
    std::vector<std::string> responses;
    std::size_t nextResponse = 0;
    std::vector<ProviderConfig> providers;
    std::vector<std::string> credentials;
    std::vector<std::size_t> toolCounts;
    std::vector<std::vector<ModelMessage>> messages;
};

struct TemporaryRoot {
    TemporaryRoot()
        : file(juce::File::getSpecialLocation(juce::File::tempDirectory)
              .getChildFile("sbfad-maintenance-" + juce::Uuid().toString()))
    { file.createDirectory(); }
    ~TemporaryRoot() { file.deleteRecursively(); }
    juce::File file;
};

ConversationTurn makeTurn(std::string id, std::string prompt)
{
    ConversationTurn result;
    result.turnId = std::move(id);
    result.messages = {
        { ConversationRole::user, std::move(prompt) },
        { ConversationRole::assistant, u8"已经完成" }
    };
    return result;
}

TerminalTurn terminal(const PresetId& presetId, std::string id, std::string prompt,
                      std::uint64_t version, std::string model = "captured-model")
{
    TerminalTurn result;
    result.presetId = presetId;
    result.expectedVersion = version;
    result.turn = makeTurn(std::move(id), std::move(prompt));
    result.provider.protocol = ProviderProtocol::chatCompletions;
    result.provider.baseUrl = "https://maintenance.invalid/v1";
    result.provider.model = std::move(model);
    result.credentialId = "provider.maintenance";
    return result;
}

class MemoryMaintenanceTests final : public juce::UnitTest {
public:
    MemoryMaintenanceTests()
        : juce::UnitTest("Automatic memory maintenance", "AgentMemory") {}

    void runTest() override
    {
        beginTest("Background curation is selective, tool-free, and uses captured provider settings");
        TemporaryRoot root;
        auto store = std::make_shared<ConversationContextStore>(root.file);
        auto manager = std::make_shared<ContextManager>(store);
        agentic_dexed::security::MemoryCredentialStore credentials;
        expect(credentials.store("provider.maintenance", "maintenance-test-secret").ok());
        MaintenanceModel model;
        model.responses = {
            R"({"operation":"observe","key":"pad_space","preference":"空灵的 pad","evidence":"这次做一个空灵的 pad"})",
            R"({"operation":"observe","key":"pad_space","preference":"空灵的 pad","evidence":"再做一个空灵的 pad"})",
            R"({"operation":"add","key":"pad_tone","preference":"温暖柔和的 pad","evidence":"我一般偏好温暖柔和的 pad"})"
        };
        MemoryMaintenanceService service(model, credentials, store, manager);
        const auto preset = juce::Uuid().toString().toStdString();
        service.onTurnFinished(terminal(preset, "one", u8"这次做一个空灵的 pad", 0, "model-a"));
        expect(service.waitUntilIdle(std::chrono::seconds(2)));
        expect(!store->loadPreferences().text.contains(u8"空灵的 pad"));
        service.onTurnFinished(terminal(preset, "two", u8"再做一个空灵的 pad", 1, "model-b"));
        expect(service.waitUntilIdle(std::chrono::seconds(2)));
        expect(store->loadPreferences().text.contains(u8"空灵的 pad"));
        service.onTurnFinished(terminal(
            preset, "three", u8"我一般偏好温暖柔和的 pad", 2, "model-c"));
        expect(service.waitUntilIdle(std::chrono::seconds(2)));
        expect(store->loadPreferences().text.contains(u8"温暖柔和的 pad"));
        {
            std::lock_guard<std::mutex> lock(model.mutex);
            expectEquals(static_cast<int>(model.providers.size()), 3);
            expectEquals(model.providers[0].model, std::string("model-a"));
            expectEquals(model.providers[1].model, std::string("model-b"));
            expectEquals(model.providers[2].model, std::string("model-c"));
            expect(std::all_of(model.toolCounts.begin(), model.toolCounts.end(),
                               [](auto count) { return count == 0; }));
            expectEquals(model.credentials[0], std::string("maintenance-test-secret"));
        }
        service.shutdown();

        beginTest("Compaction preserves the newest eight turns and invalid output never deletes history");
        TemporaryRoot compactRoot;
        auto compactStore = std::make_shared<ConversationContextStore>(compactRoot.file);
        auto compactManager = std::make_shared<ContextManager>(compactStore);
        agentic_dexed::security::MemoryCredentialStore compactCredentials;
        expect(compactCredentials.store("provider.maintenance", "maintenance-test-secret").ok());
        const auto compactPreset = juce::Uuid().toString().toStdString();
        PresetConversationContext initial;
        initial.presetId = compactPreset;
        for (int i = 0; i < 12; ++i)
            initial.recentTurns.push_back(makeTurn("seed-" + std::to_string(i),
                                                   "seed request " + std::to_string(i)));
        expect(compactStore->commit(initial, 0).status == ContextCommitStatus::committed);
        MaintenanceModel compactModel;
        compactModel.responses = {
            R"({"operation":"none","key":"","preference":"","evidence":""})",
            R"({"summary":"此前围绕 pad 做了多轮调整，最近偏向空灵质感。"})"
        };
        MemoryMaintenanceService compactService(
            compactModel, compactCredentials, compactStore, compactManager);
        compactService.onTurnFinished(terminal(
            compactPreset, "seed-12", u8"继续调整这个 pad", 1));
        expect(compactService.waitUntilIdle(std::chrono::seconds(2)));
        const auto compacted = compactStore->load(compactPreset);
        expect(compacted.ok);
        expectEquals(static_cast<int>(compacted.context.recentTurns.size()), 8);
        expectEquals(compacted.context.recentTurns.front().turnId, std::string("seed-5"));
        expect(compacted.context.summary.find(u8"多轮调整") != std::string::npos);
        compactService.shutdown();

        beginTest("Invalid curator and compactor responses preserve every stored turn");
        TemporaryRoot invalidRoot;
        auto invalidStore = std::make_shared<ConversationContextStore>(invalidRoot.file);
        auto invalidManager = std::make_shared<ContextManager>(invalidStore);
        agentic_dexed::security::MemoryCredentialStore invalidCredentials;
        expect(invalidCredentials.store(
            "provider.maintenance", "maintenance-test-secret").ok());
        const auto invalidPreset = juce::Uuid().toString().toStdString();
        PresetConversationContext beforeInvalid;
        beforeInvalid.presetId = invalidPreset;
        beforeInvalid.summary = "keep this summary";
        for (int i = 0; i < 12; ++i)
            beforeInvalid.recentTurns.push_back(makeTurn(
                "invalid-seed-" + std::to_string(i),
                "request " + std::to_string(i)));
        expect(invalidStore->commit(beforeInvalid, 0).status
               == ContextCommitStatus::committed);
        MaintenanceModel invalidModel;
        invalidModel.responses = { "not json", "still not json" };
        MemoryMaintenanceService invalidService(
            invalidModel, invalidCredentials, invalidStore, invalidManager);
        invalidService.onTurnFinished(terminal(
            invalidPreset, "invalid-seed-12", u8"继续这个 pad", 1));
        expect(invalidService.waitUntilIdle(std::chrono::seconds(2)));
        const auto afterInvalid = invalidStore->load(invalidPreset);
        expect(afterInvalid.ok);
        expectEquals(static_cast<int>(afterInvalid.context.recentTurns.size()), 13);
        expectEquals(afterInvalid.context.summary, std::string("keep this summary"));
        invalidService.shutdown();

        beginTest("Cancelled turns are persisted but maintenance is deferred");
        TemporaryRoot cancelRoot;
        auto cancelStore = std::make_shared<ConversationContextStore>(cancelRoot.file);
        auto cancelManager = std::make_shared<ContextManager>(cancelStore);
        agentic_dexed::security::MemoryCredentialStore cancelCredentials;
        expect(cancelCredentials.store(
            "provider.maintenance", "maintenance-test-secret").ok());
        MaintenanceModel cancelModel;
        cancelModel.responses = {
            R"({"operation":"add","key":"pad_brightness","preference":"明亮的 pad","evidence":"我一般偏好很亮的 pad"})",
            R"({"operation":"none","key":"","preference":"","evidence":""})"
        };
        MemoryMaintenanceService cancelService(
            cancelModel, cancelCredentials, cancelStore, cancelManager);
        const auto cancelPreset = juce::Uuid().toString().toStdString();
        auto cancelled = terminal(cancelPreset, "cancelled", u8"我一般偏好很亮的 pad", 0);
        cancelled.turn.terminalState = TurnTerminalState::cancelled;
        cancelService.onTurnFinished(std::move(cancelled));
        expect(cancelService.waitUntilIdle(std::chrono::seconds(2)));
        expectEquals(static_cast<int>(cancelModel.providers.size()), 0);
        expectEquals(static_cast<int>(cancelStore->load(cancelPreset).context.recentTurns.size()), 1);
        cancelService.onTurnFinished(terminal(
            cancelPreset, "after-cancel", u8"继续这个 pad", 1));
        expect(cancelService.waitUntilIdle(std::chrono::seconds(2)));
        expectEquals(static_cast<int>(cancelModel.providers.size()), 2);
        expect(cancelStore->loadPreferences().text.contains(u8"明亮的 pad"));
        cancelService.shutdown();

        beginTest("Shutdown cancels an active maintenance request and drains queued work");
        TemporaryRoot shutdownRoot;
        auto shutdownStore = std::make_shared<ConversationContextStore>(shutdownRoot.file);
        auto shutdownManager = std::make_shared<ContextManager>(shutdownStore);
        agentic_dexed::security::MemoryCredentialStore shutdownCredentials;
        expect(shutdownCredentials.store(
            "provider.maintenance", "maintenance-test-secret").ok());
        BlockingMaintenanceModel blockingModel;
        MemoryMaintenanceService shutdownService(
            blockingModel, shutdownCredentials, shutdownStore, shutdownManager);
        const auto shutdownPreset = juce::Uuid().toString().toStdString();
        shutdownService.onTurnFinished(terminal(
            shutdownPreset, "shutdown", u8"我一般偏好温暖的 pad", 0));
        const auto deadline = juce::Time::getMillisecondCounter() + 2000u;
        while (blockingModel.starts.load() == 0
               && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep(5);
        expect(blockingModel.starts.load() > 0);
        shutdownService.shutdown();
        expect(blockingModel.cancelled->load());
        expect(shutdownService.waitUntilIdle(std::chrono::milliseconds(50)));
    }
};

MemoryMaintenanceTests memoryMaintenanceTests;
}

#include <JuceHeader.h>
#include "agent/context/ConversationContextStore.h"

#include <thread>

namespace {
using namespace agentic_dexed::agent::context;

ConversationTurn turn(std::string id, std::string userText)
{
    ConversationTurn value;
    value.turnId = std::move(id);
    value.terminalState = TurnTerminalState::completed;
    value.messages.push_back({ ConversationRole::user, std::move(userText), {}, {}, true });
    value.messages.push_back({ ConversationRole::assistant, juce::String::fromUTF8("已完成").toStdString(), {}, {}, true });
    return value;
}

class ConversationContextStoreTests final : public juce::UnitTest {
public:
    ConversationContextStoreTests() : UnitTest("Conversation context store", "Agent") {}

    void runTest() override
    {
        beginTest("Chinese context is compressed, atomic, and reloadable");
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sbfad-context-store-" + juce::Uuid().toString());
        expect(root.createDirectory());
        ConversationContextStore store(root);
        PresetConversationContext context;
        context.presetId = juce::Uuid().toString().toStdString();
        context.summary = juce::String::fromUTF8("空灵、温暖、有限释音").toStdString();
        context.recentTurns.push_back(turn("turn-1", juce::String::fromUTF8("做一个空灵 pad").toStdString()));
        const auto committed = store.commit(context, 0);
        expect(committed.status == ContextCommitStatus::committed, committed.error);
        expectEquals(committed.currentVersion, uint64_t(1));
        const auto loaded = ConversationContextStore(root).load(context.presetId);
        expect(loaded.ok, loaded.error);
        expect(loaded.exists);
        expectEquals(loaded.context.summary, context.summary);
        expectEquals(loaded.context.recentTurns.front().messages.front().text,
                     context.recentTurns.front().messages.front().text);
        const auto file = root.getChildFile("contexts").getChildFile(context.presetId + ".json.z");
        expect(file.existsAsFile());
        expect(file.getSize() < static_cast<juce::int64>(juce::JSON::toString(serializeContext(loaded.context)).getNumBytesAsUTF8()));

        beginTest("Concurrent appenders re-read and merge version conflicts");
        ConversationContextStore writerA(root);
        ConversationContextStore writerB(root);
        const auto baselineVersion = loaded.context.revision;
        ContextCommitResult resultA;
        ContextCommitResult resultB;
        std::thread first([&] { resultA = writerA.appendTurn(context.presetId, turn("turn-a", "first"), baselineVersion); });
        std::thread second([&] { resultB = writerB.appendTurn(context.presetId, turn("turn-b", "second"), baselineVersion); });
        first.join();
        second.join();
        expect(resultA.status == ContextCommitStatus::committed, resultA.error);
        expect(resultB.status == ContextCommitStatus::committed, resultB.error);
        const auto merged = store.load(context.presetId);
        expect(merged.ok);
        expectEquals(static_cast<int>(merged.context.recentTurns.size()), 3);

        beginTest("Direct stale commits report conflict without replacing newer data");
        auto stale = loaded.context;
        stale.summary = "stale overwrite";
        const auto conflict = store.commit(stale, baselineVersion);
        expect(conflict.status == ContextCommitStatus::conflict);
        expect(!store.load(context.presetId).context.summary.empty());
        expect(store.load(context.presetId).context.summary != "stale overwrite");

        beginTest("Credentials and private paths are redacted before compression");
        const std::string credential = "sk-DO-NOT-LEAK-0123456789";
        const std::string privatePath = "C:\\Users\\alice\\private\\preset.dexedpreset";
        const auto secure = store.appendTurn(context.presetId,
            turn("turn-secret", "key=" + credential + " path=" + privatePath),
            merged.context.revision, credential);
        expect(secure.status == ContextCommitStatus::committed);
        const auto sanitized = store.load(context.presetId);
        const auto& persisted = sanitized.context.recentTurns.back().messages.front().text;
        expect(persisted.find(credential) == std::string::npos);
        expect(persisted.find(privatePath) == std::string::npos);
        expect(persisted.find(juce::String::fromUTF8("[已隐藏敏感信息]").toStdString()) != std::string::npos);

        beginTest("Invalid oversized writes preserve the last committed context");
        const auto beforeInvalid = store.load(context.presetId);
        auto invalid = beforeInvalid.context;
        invalid.recentTurns.back().messages.front().text.assign(48 * 1024 + 1, 'x');
        const auto refused = store.commit(invalid, beforeInvalid.context.revision);
        expect(refused.status == ContextCommitStatus::error);
        const auto afterInvalid = store.load(context.presetId);
        expectEquals(afterInvalid.context.revision, beforeInvalid.context.revision);
        expectEquals(afterInvalid.context.recentTurns.back().turnId, std::string("turn-secret"));

        beginTest("Symlink context files are rejected when the platform creates one");
        const auto otherId = juce::Uuid().toString().toStdString();
        const auto external = root.getChildFile("outside.json.z");
        expect(external.replaceWithText("outside"));
        const auto link = root.getChildFile("contexts").getChildFile(otherId + ".json.z");
        if (external.createSymbolicLink(link, true)) {
            const auto linked = store.load(otherId);
            expect(!linked.ok);
            expectEquals(external.loadFileAsString(), juce::String("outside"));
        }

        beginTest("No successful operation leaves temporary files");
        expectEquals(root.findChildFiles(juce::File::findFiles, true, "*temp*;*.tmp").size(), 0);
        expect(root.deleteRecursively());
    }
} conversationContextStoreTests;
}

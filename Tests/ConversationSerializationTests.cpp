#include <JuceHeader.h>
#include "agent/context/ConversationSerialization.h"

namespace {
using namespace agentic_dexed::agent::context;

PresetConversationContext exampleContext()
{
    PresetConversationContext context;
    context.presetId = "66bff4f0-89d5-4f13-b570-4eafebd3015c";
    context.revision = 7;
    context.summary = juce::String::fromUTF8("目标：空灵但不要无限延音").toStdString();
    context.updatedAtUnixMs = 1791324000000LL;
    context.fingerprintAliases = { "sha256:abc", "sha256:def" };

    ConversationTurn turn;
    turn.turnId = "turn-1";
    turn.terminalState = TurnTerminalState::completed;
    turn.messages = {
        { ConversationRole::user, juce::String::fromUTF8("做一个空灵的 pad").toStdString(), {}, {}, true },
        { ConversationRole::assistant, "I will adjust the sound.", {}, {}, true },
        { ConversationRole::toolCall, "{\"operations\":[]}", "call-1", "apply_parameter_patch", true },
        { ConversationRole::toolResult, "{\"revision\":3}", "call-1", "apply_parameter_patch", true },
        { ConversationRole::assistant, juce::String::fromUTF8("已经完成，尾音较长但会自然结束。").toStdString(), {}, {}, true },
    };
    turn.transactions.push_back({ "tx-1", juce::String::fromUTF8("降低滤波并延长释放").toStdString(), 3 });
    context.recentTurns.push_back(std::move(turn));
    return context;
}

class ConversationSerializationTests final : public juce::UnitTest {
public:
    ConversationSerializationTests() : UnitTest("Conversation serialization", "Agent") {}

    void runTest() override
    {
        beginTest("Version 1 round-trips Chinese text, tool pairs, and transactions");
        const auto original = exampleContext();
        const auto parsed = parseContext(serializeContext(original));
        expect(parsed.ok(), parsed.error);
        expectEquals(parsed.context->presetId, original.presetId);
        expectEquals(parsed.context->summary, original.summary);
        expectEquals(static_cast<int>(parsed.context->recentTurns.size()), 1);
        expectEquals(static_cast<int>(parsed.context->recentTurns.front().messages.size()), 5);
        expectEquals(parsed.context->recentTurns.front().messages[2].callId, std::string("call-1"));
        expectEquals(parsed.context->recentTurns.front().transactions.front().resultingRevision, uint64_t(3));

        beginTest("Tool calls and results must remain paired");
        auto unpaired = serializeContext(original);
        auto* turns = unpaired.getDynamicObject()->getProperty("recent_turns").getArray();
        turns->getReference(0).getDynamicObject()->getProperty("messages").getArray()->remove(3);
        expect(!parseContext(unpaired).ok());

        beginTest("A single persisted message is limited to 48 KiB");
        auto oversizedMessage = original;
        oversizedMessage.recentTurns.front().messages.front().text.assign(48 * 1024 + 1, 'x');
        expect(!parseContext(serializeContext(oversizedMessage)).ok());

        beginTest("A decompressed context is limited to 2 MiB");
        auto oversizedContext = original;
        oversizedContext.recentTurns.clear();
        for (int i = 0; i < 24; ++i) {
            ConversationTurn turn;
            turn.turnId = "large-" + std::to_string(i);
            turn.terminalState = TurnTerminalState::completed;
            turn.messages.push_back({ ConversationRole::user, std::string(48 * 1024, 'x'), {}, {}, true });
            turn.messages.push_back({ ConversationRole::assistant, std::string(48 * 1024, 'y'), {}, {}, true });
            oversizedContext.recentTurns.push_back(std::move(turn));
        }
        expect(!parseContext(serializeContext(oversizedContext)).ok());

        beginTest("Illegal roles, versions, controls, and security fields are rejected");
        auto illegalRole = serializeContext(original);
        illegalRole.getDynamicObject()->getProperty("recent_turns").getArray()->getReference(0)
            .getDynamicObject()->getProperty("messages").getArray()->getReference(0)
            .getDynamicObject()->setProperty("role", "system");
        expect(!parseContext(illegalRole).ok());

        auto futureVersion = serializeContext(original);
        futureVersion.getDynamicObject()->setProperty("version", 2);
        expect(!parseContext(futureVersion).ok());

        auto controlText = serializeContext(original);
        controlText.getDynamicObject()->setProperty("summary", juce::String("bad") + juce::String::charToString(1));
        expect(!parseContext(controlText).ok());

        auto reasoning = serializeContext(original);
        reasoning.getDynamicObject()->setProperty("reasoning", "hidden chain");
        expect(!parseContext(reasoning).ok());

        const char invalidUtf8[] = { '{', '"', 'x', '"', ':', '"', static_cast<char>(0xff), '"', '}' };
        expect(!parseContextJson(std::string_view(invalidUtf8, sizeof(invalidUtf8))).ok());

        beginTest("Namespaced future metadata is ignored under version 1");
        auto extension = serializeContext(original);
        extension.getDynamicObject()->setProperty("x_future_note", "ignored");
        expect(parseContext(extension).ok());
    }
} conversationSerializationTests;
}

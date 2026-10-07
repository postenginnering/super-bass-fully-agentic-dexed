#include <JuceHeader.h>

#include "agent/context/ContextCompactor.h"

namespace {
using namespace agentic_dexed::agent::context;

ConversationTurn numberedTurn(int number)
{
    ConversationTurn result;
    result.turnId = "turn-" + std::to_string(number);
    result.messages = {
        { ConversationRole::user, "request-" + std::to_string(number) },
        { ConversationRole::toolCall, R"({"scope":"all"})", "call-" + std::to_string(number), "get_synth_state" },
        { ConversationRole::toolResult, R"({"revision":2})", "call-" + std::to_string(number), "get_synth_state" },
        { ConversationRole::assistant, "answer-" + std::to_string(number) }
    };
    result.transactions.push_back({ "tx-" + std::to_string(number), "changed tone", 2 });
    return result;
}

class ContextCompactorTests final : public juce::UnitTest {
public:
    ContextCompactorTests()
        : juce::UnitTest("Automatic context compactor", "AgentMemory") {}

    void runTest() override
    {
        PresetConversationContext original;
        original.presetId = juce::Uuid().toString().toStdString();
        original.revision = 7;
        original.summary = "old summary";
        original.fingerprintAliases = { "fingerprint-a" };
        for (int i = 0; i < 15; ++i)
            original.recentTurns.push_back(numberedTurn(i));

        beginTest("Compaction keeps structure and the eight newest complete turns");
        expect(ContextCompactor::shouldCompact(original));
        const auto request = ContextCompactor::buildRequest(original);
        expectEquals(static_cast<int>(request.size()), 2);
        expect(request.front().text.find("JSON") != std::string::npos);
        const auto compacted = ContextCompactor::parseResponse(
            R"({"summary":"用户正在制作 pad；偏好缓慢起音，已尝试降低高频。"})", original);
        expect(compacted.ok && compacted.context.has_value());
        expectEquals(static_cast<int>(compacted.context->recentTurns.size()), 8);
        expectEquals(compacted.context->recentTurns.front().turnId, std::string("turn-7"));
        expectEquals(compacted.context->recentTurns.back().turnId, std::string("turn-14"));
        expectEquals(compacted.context->presetId, original.presetId);
        expectEquals(static_cast<int64>(compacted.context->revision), static_cast<int64>(7));
        expectEquals(compacted.context->fingerprintAliases.front(), std::string("fingerprint-a"));
        expectEquals(compacted.context->recentTurns.front().transactions.front().transactionId,
                     std::string("tx-7"));

        beginTest("Invalid, unsafe, or oversized model output leaves the source untouched");
        expect(!ContextCompactor::parseResponse("not json", original).ok);
        expect(!ContextCompactor::parseResponse(
            R"({"summary":"ok","delete_recent":true})", original).ok);
        expectEquals(static_cast<int>(original.recentTurns.size()), 15);
        expectEquals(original.summary, std::string("old summary"));

        beginTest("Small contexts do not schedule compaction");
        auto small = original;
        small.recentTurns.resize(8);
        small.summary.clear();
        expect(!ContextCompactor::shouldCompact(small));
    }
};

ContextCompactorTests contextCompactorTests;
}

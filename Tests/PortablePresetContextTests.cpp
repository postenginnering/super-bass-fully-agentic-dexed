#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "agent/AgentController.h"
#include "agent/context/ContextManager.h"
#include "agent/context/PortablePresetContext.h"
#include "state/SynthStateService.h"

#include <atomic>
#include <thread>

namespace {
using namespace agentic_dexed::agent::context;

PresetConversationContext sampleContext()
{
    PresetConversationContext context;
    context.presetId = juce::Uuid().toString().toStdString();
    context.revision = 7;
    context.summary = u8"目标：空灵但有有限余音；拒绝：刺耳高频；待确认：低频厚度。";
    ConversationTurn turn;
    turn.turnId = "portable-turn";
    turn.messages = {
        { ConversationRole::user, u8"做一个空灵的 pad" },
        { ConversationRole::assistant,
          u8"已经完成；凭据 sk-portable-secret 和路径 C:\\Users\\alice\\private.dexedpreset 不会保存" },
        { ConversationRole::toolCall, R"({"scope":"all"})", "portable-call", "get_synth_state" },
        { ConversationRole::toolResult, R"({"revision":7})", "portable-call", "get_synth_state", true }
    };
    context.recentTurns.push_back(std::move(turn));
    context.fingerprintAliases.push_back(std::string(64, 'a'));
    return context;
}

juce::MemoryBlock oversizedCompressedEnvelope()
{
    std::string expanded(kMaximumContextBytes + 1, 'x');
    juce::MemoryOutputStream compressed;
    {
        juce::GZIPCompressorOutputStream gzip(compressed, 6);
        gzip.write(expanded.data(), expanded.size());
        gzip.flush();
    }
    const auto payload = compressed.getMemoryBlock();
    const auto hash = juce::SHA256(payload).toHexString();
    juce::MemoryOutputStream envelope;
    envelope.write(kPortableContextMagic.data(), kPortableContextMagic.size());
    envelope.writeInt(kPortableContextEnvelopeVersion);
    envelope.writeInt(static_cast<int>(payload.getSize()));
    envelope.write(hash.toRawUTF8(), 64);
    envelope.write(payload.getData(), payload.getSize());
    return envelope.getMemoryBlock();
}

class PortablePresetContextTests final : public juce::UnitTest {
public:
    PortablePresetContextTests()
        : juce::UnitTest("Portable preset context", "AgentContext") {}

    void runTest() override
    {
        beginTest("Versioned context round-trips and credentials are redacted");
        const auto source = sampleContext();
        const auto encoded = encodePortableContext(source);
        expect(!encoded.isEmpty());
        const auto decoded = decodePortableContext(encoded);
        expect(decoded.ok && decoded.context.has_value(), decoded.error);
        if (decoded.context) {
            expectEquals(decoded.context->presetId, source.presetId);
            expectEquals(decoded.context->revision, source.revision);
            expect(decoded.context->summary.find(u8"无限") == std::string::npos);
            expect(decoded.context->recentTurns.front().messages[1].text.find("sk-portable-secret") == std::string::npos);
            expect(decoded.context->recentTurns.front().messages[1].text.find("C:\\Users\\alice") == std::string::npos);
            expect(decoded.context->recentTurns.front().messages[1].text.find(u8"[已隐藏敏感信息]") != std::string::npos);
        }

        beginTest("Hash mismatch and malformed envelopes are rejected");
        auto corrupted = encoded;
        auto* bytes = static_cast<std::uint8_t*>(corrupted.getData());
        bytes[corrupted.getSize() - 1] ^= 0x55;
        expect(!decodePortableContext(corrupted).ok);
        juce::MemoryBlock random("not-a-context", 13);
        expect(!decodePortableContext(random).ok);

        beginTest("Decompression limit rejects a compressed bomb");
        const auto bomb = oversizedCompressedEnvelope();
        expect(bomb.getSize() < 64 * 1024);
        const auto rejectedBomb = decodePortableContext(bomb);
        expect(!rejectedBomb.ok);
        expect(rejectedBomb.error.find("limit") != std::string::npos);

        beginTest("Cross-instance processor state restores UUID and context");
        DexedAudioProcessor first;
        expect(first.agentController().importPortableContext(encoded));
        juce::MemoryBlock state;
        first.getStateInformation(state);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(
            state.getData(), static_cast<int>(state.getSize()));
        expect(xml != nullptr && xml->getChildByName("agentContext") != nullptr);
        expectEquals(xml != nullptr ? xml->getIntAttribute("agenticStateVersion") : 0, 2);
        expect(xml != nullptr && !xml->hasAttribute("activeFileCartridge"));

        DexedAudioProcessor restored;
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const auto restoredContext = decodePortableContext(
            restored.agentController().portableContextSnapshot());
        expect(restoredContext.ok && restoredContext.context.has_value(), restoredContext.error);
        if (restoredContext.context)
            expectEquals(restoredContext.context->presetId, source.presetId);

        beginTest("Corrupt context does not prevent the synth patch from loading");
        auto corruptXml = juce::AudioProcessor::getXmlFromBinary(
            state.getData(), static_cast<int>(state.getSize()));
        expect(corruptXml != nullptr);
        if (corruptXml != nullptr) {
            corruptXml->setAttribute("gain", 0.375);
            if (auto* node = corruptXml->getChildByName("agentContext"))
                node->setAttribute("sha256", std::string(64, '0'));
            juce::MemoryBlock corruptState;
            juce::AudioProcessor::copyXmlToBinary(*corruptXml, corruptState);
            DexedAudioProcessor target;
            target.setStateInformation(corruptState.getData(), static_cast<int>(corruptState.getSize()));
            const auto snapshot = target.synthStateService().snapshot(
                { agentic_dexed::SnapshotScopeKind::ids, {}, { "global.output" } });
            expectWithinAbsoluteError(std::get<double>(snapshot.values.at("global.output")), 0.375, 1.0e-6);
        }

        beginTest("Concurrent imports never expose a partial portable snapshot");
        DexedAudioProcessor concurrent;
        std::atomic_bool failed { false };
        std::thread writer([&] {
            for (int i = 0; i < 20; ++i) {
                auto next = sampleContext();
                next.summary = "revision-" + std::to_string(i);
                if (!concurrent.agentController().importPortableContext(encodePortableContext(next)))
                    failed.store(true);
            }
        });
        for (int i = 0; i < 100; ++i) {
            juce::MemoryBlock hostState;
            concurrent.getStateInformation(hostState);
            const auto hostXml = juce::AudioProcessor::getXmlFromBinary(
                hostState.getData(), static_cast<int>(hostState.getSize()));
            const auto* node = hostXml != nullptr ? hostXml->getChildByName("agentContext") : nullptr;
            juce::MemoryBlock portable;
            if (node == nullptr
                || !portable.fromBase64Encoding(node->getAllSubText())
                || node->getStringAttribute("sha256").toStdString()
                    != portableContextSha256(portable)
                || !decodePortableContext(portable).ok)
                failed.store(true);
        }
        writer.join();
        expect(!failed.load());
    }
};

PortablePresetContextTests portablePresetContextTests;
}

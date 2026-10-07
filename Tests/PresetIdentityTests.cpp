#include <JuceHeader.h>
#include "agent/context/PresetIdentityService.h"

namespace {
using namespace agentic_dexed::agent::context;

CanonicalVoice voiceWithSeed(std::uint8_t seed)
{
    CanonicalVoice voice {};
    for (std::size_t index = 0; index < voice.size(); ++index)
        voice[index] = static_cast<std::uint8_t>((index + seed) & 0x7f);
    return voice;
}

class PresetIdentityTests final : public juce::UnitTest {
public:
    PresetIdentityTests() : UnitTest("Preset identity", "Agent") {}

    void runTest() override
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sbfad-preset-identity-" + juce::Uuid().toString());
        const auto indexFile = root.getChildFile("preset-index.json");

        beginTest("New voices get UUIDs and identical content restores identity");
        expect(root.createDirectory());
        PresetIdentityService service(indexFile);
        const auto voiceA = voiceWithSeed(1);
        const auto first = service.activateVoice(voiceA);
        expect(first.created);
        expect(juce::Uuid(first.presetId).toString().isNotEmpty());
        const auto repeated = service.activateVoice(voiceA);
        expect(!repeated.created);
        expectEquals(repeated.presetId, first.presetId);
        expectEquals(repeated.fingerprint, first.fingerprint);

        PresetIdentityService restored(indexFile);
        expectEquals(restored.activateVoice(voiceA).presetId, first.presetId);

        beginTest("Edited fingerprints remain aliases of the same preset");
        auto voiceB = voiceA;
        voiceB[134] = static_cast<std::uint8_t>((voiceB[134] + 1) & 0x1f);
        expect(service.updateFingerprint(first.presetId, voiceB));
        expectEquals(service.activateVoice(voiceA).presetId, first.presetId);
        expectEquals(service.activateVoice(voiceB).presetId, first.presetId);

        beginTest("Copies get a new UUID and cloned-from metadata");
        const auto clone = service.clonePreset(first.presetId);
        expect(clone.has_value());
        expect(*clone != first.presetId);
        expectEquals(service.clonedFrom(*clone).value_or(""), first.presetId);

        beginTest("Moves and renames retain identity while shifting occupied slots");
        expect(service.bindSlot(1, first.presetId));
        expect(service.bindSlot(2, *clone));
        expect(service.moveSlot(1, 3));
        expectEquals(service.presetForSlot(3).value_or(""), first.presetId);
        expectEquals(service.presetForSlot(1).value_or(""), *clone);
        expect(service.renamePreset(first.presetId));
        expectEquals(service.presetForSlot(3).value_or(""), first.presetId);

        beginTest("A corrupt imported index cannot replace valid in-memory state");
        auto corrupt = service.exportIndex();
        corrupt.getDynamicObject()->setProperty("version", 99);
        expect(!service.importIndex(corrupt));
        expectEquals(service.activateVoice(voiceA).presetId, first.presetId);
        expectEquals(service.presetForSlot(3).value_or(""), first.presetId);

        beginTest("Identity lookup never changes traditional SysEx bytes");
        std::array<std::uint8_t, 4104> syx {};
        for (std::size_t index = 0; index < syx.size(); ++index)
            syx[index] = static_cast<std::uint8_t>(index & 0x7f);
        const auto syxFile = root.getChildFile("same-content.syx");
        expect(syxFile.replaceWithData(syx.data(), syx.size()));
        const auto hashBefore = juce::SHA256(syxFile).toHexString();
        const auto fromFirstFile = service.activateVoice(voiceA);
        const auto duplicateFile = root.getChildFile("duplicate-name.syx");
        expect(duplicateFile.replaceWithData(syx.data(), syx.size()));
        const auto fromSecondFile = service.activateVoice(voiceA);
        expectEquals(fromSecondFile.presetId, fromFirstFile.presetId);
        expectEquals(juce::SHA256(syxFile).toHexString(), hashBefore);
        expectEquals(juce::SHA256(duplicateFile).toHexString(), hashBefore);

        beginTest("Index writes are atomic and leave no temporary files");
        expect(service.save());
        expect(indexFile.existsAsFile());
        expectEquals(root.findChildFiles(juce::File::findFiles, false, "*.tmp").size(), 0);
        expect(root.deleteRecursively());
    }
} presetIdentityTests;
}

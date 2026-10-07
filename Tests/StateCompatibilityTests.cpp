#include "TestDataPaths.h"
#include "../Source/PluginProcessor.h"
#include "../Source/state/SynthStateService.h"

#include <JuceHeader.h>

#include <array>
#include <cmath>
#include <memory>

namespace
{
using namespace agentic_dexed;

bool equivalentValue(const ParameterValue& left, const ParameterValue& right)
{
    if (left.index() != right.index())
        return false;
    if (const auto* value = std::get_if<double>(&left))
        return std::abs(*value - std::get<double>(right)) <= 1.0e-6;
    return left == right;
}

class StateCompatibilityTests final : public juce::UnitTest
{
public:
    StateCompatibilityTests()
        : juce::UnitTest("Legacy state round-trip compatibility", "Compatibility")
    {
    }

    void runTest() override
    {
        beginTest("Upstream state migrates and an Agent edit round-trips every value");
        const auto fixture = agentic_dexed::test::dataRoot().getChildFile("Tests/fixtures/upstream-init-state.bin");
        juce::MemoryBlock legacyState;
        expect(fixture.loadFileAsData(legacyState));
        if (legacyState.isEmpty())
            return;

        auto source = std::make_unique<DexedAudioProcessor>();
        const auto revisionBeforeLoad = source->atomicParameterStore().revision();
        source->setStateInformation(
            legacyState.getData(), static_cast<int>(legacyState.getSize()));
        expectEquals(
            source->atomicParameterStore().revision(), revisionBeforeLoad + 1);

        PatchRequest change;
        change.transactionId = "state-round-trip-edit";
        change.baseRevision = source->synthStateService().revision();
        change.reason = "state compatibility test";
        change.operations = { { "patch.name", std::string { "AGENT TEST" } } };
        expect(source->synthStateService().submit(change).status == PatchStatus::committed);

        const auto expected = source->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });
        juce::MemoryBlock savedState;
        source->getStateInformation(savedState);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(
            savedState.getData(), static_cast<int>(savedState.getSize()));
        expect(xml != nullptr);
        if (xml == nullptr)
            return;
        expectEquals(xml->getIntAttribute("agenticStateVersion", 0), 2);
        expect(xml->getChildByName("agentContext") != nullptr);
        expect(!xml->hasAttribute("transactionHistory"));
        expect(!xml->hasAttribute("credentials"));
        expect(!xml->hasAttribute("conversation"));

        auto restored = std::make_unique<DexedAudioProcessor>();
        restored->setStateInformation(
            savedState.getData(), static_cast<int>(savedState.getSize()));
        const auto actual = restored->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });
        expectEquals(actual.values.size(), expected.values.size());
        for (const auto& [id, expectedValue] : expected.values)
        {
            const auto found = actual.values.find(id);
            expect(found != actual.values.end(), id);
            if (found != actual.values.end())
                expect(equivalentValue(found->second, expectedValue), id);
        }
        expectEquals(actual.patchName, expected.patchName);
        expect(restored->synthStateService().history().empty());

        beginTest("State restore returns while the pixel editor is open");
        auto editorOpen = std::make_unique<DexedAudioProcessor>();
        juce::MemoryBlock editorState;
        editorOpen->getStateInformation(editorState);
        std::unique_ptr<juce::AudioProcessorEditor> editor(
            editorOpen->createEditorIfNeeded());
        expect(editor != nullptr);
        editorOpen->setStateInformation(
            editorState.getData(), static_cast<int>(editorState.getSize()));
        expect(editorOpen->getActiveEditor() == editor.get());

        beginTest("Historical states retain initialized defaults for absent attributes");
        auto historicalXml = juce::AudioProcessor::getXmlFromBinary(
            legacyState.getData(), static_cast<int>(legacyState.getSize()));
        expect(historicalXml != nullptr);
        if (historicalXml == nullptr)
            return;

        static constexpr std::array attributesToRemove {
            "cutoff", "reso", "gain", "engineType", "masterTune", "opSwitch",
            "transpose12AsScale", "mpeEnabled", "mpePitchBendRange", "monoMode",
            "portamento", "glissando", "wheelMod", "footMod", "breathMod",
            "aftertouchMod"
        };
        for (const auto* attribute : attributesToRemove)
            historicalXml->removeAttribute(attribute);

        juce::MemoryBlock historicalState;
        juce::AudioProcessor::copyXmlToBinary(*historicalXml, historicalState);
        auto historical = std::make_unique<DexedAudioProcessor>();
        const auto defaults = historical->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });
        historical->setStateInformation(
            historicalState.getData(), static_cast<int>(historicalState.getSize()));
        const auto migrated = historical->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });

        static constexpr std::array defaultedParameterIds {
            "effects.filter.cutoff", "effects.filter.resonance", "global.output",
            "performance.mono", "global.master_tune", "engine.model",
            "performance.transpose_as_scale", "performance.mpe.enabled",
            "performance.mpe.pitch_bend_range", "performance.portamento.time",
            "performance.portamento.glissando", "operator.1.enabled",
            "operator.2.enabled", "operator.3.enabled", "operator.4.enabled",
            "operator.5.enabled", "operator.6.enabled", "modulation.wheel.range",
            "modulation.wheel.pitch", "modulation.wheel.amplitude",
            "modulation.wheel.envelope", "modulation.foot.range",
            "modulation.foot.pitch", "modulation.foot.amplitude",
            "modulation.foot.envelope", "modulation.breath.range",
            "modulation.breath.pitch", "modulation.breath.amplitude",
            "modulation.breath.envelope", "modulation.aftertouch.range",
            "modulation.aftertouch.pitch", "modulation.aftertouch.amplitude",
            "modulation.aftertouch.envelope"
        };
        for (const auto* id : defaultedParameterIds)
        {
            const auto before = defaults.values.find(id);
            const auto after = migrated.values.find(id);
            expect(before != defaults.values.end(), id);
            expect(after != migrated.values.end(), id);
            if (before != defaults.values.end() && after != migrated.values.end())
                expect(equivalentValue(before->second, after->second), id);
        }

        beginTest("An incomplete legacy state is rejected before any registered value changes");
        auto malformedXml = juce::AudioProcessor::getXmlFromBinary(
            legacyState.getData(), static_cast<int>(legacyState.getSize()));
        expect(malformedXml != nullptr);
        if (malformedXml == nullptr)
            return;
        if (auto* blob = malformedXml->getChildByName("dexedBlob"))
            malformedXml->removeChildElement(blob, true);
        auto* tuning = malformedXml->createNewChildElement("dexedTuning");
        auto* scl = tuning->createNewChildElement("scl");
        scl->addTextElement("! compatibility.scl\nCompatibility\n1\n2/1\n");
        juce::MemoryBlock malformedState;
        juce::AudioProcessor::copyXmlToBinary(*malformedXml, malformedState);

        auto guarded = std::make_unique<DexedAudioProcessor>();
        const auto guardedBefore = guarded->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });
        const auto guardedRevision = guarded->synthStateService().revision();
        guarded->setStateInformation(
            malformedState.getData(), static_cast<int>(malformedState.getSize()));
        const auto guardedAfter = guarded->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });
        expect(guardedAfter.values == guardedBefore.values);
        expectEquals(guardedAfter.patchName, guardedBefore.patchName);
        expectEquals(guarded->synthStateService().revision(), guardedRevision);

        beginTest("Out-of-range legacy attributes are rejected before publication");
        auto invalidAttributeXml = juce::AudioProcessor::getXmlFromBinary(
            legacyState.getData(), static_cast<int>(legacyState.getSize()));
        expect(invalidAttributeXml != nullptr);
        if (invalidAttributeXml == nullptr)
            return;
        invalidAttributeXml->setAttribute("mpePitchBendRange", 999);
        juce::MemoryBlock invalidAttributeState;
        juce::AudioProcessor::copyXmlToBinary(
            *invalidAttributeXml, invalidAttributeState);

        auto attributeGuard = std::make_unique<DexedAudioProcessor>();
        const auto attributeBefore = attributeGuard->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });
        attributeGuard->setStateInformation(
            invalidAttributeState.getData(),
            static_cast<int>(invalidAttributeState.getSize()));
        const auto attributeAfter = attributeGuard->synthStateService().snapshot(
            { SnapshotScopeKind::all, {}, {} });
        expect(attributeAfter.values == attributeBefore.values);
        expectEquals(attributeAfter.revision, attributeBefore.revision);
    }
};

StateCompatibilityTests stateCompatibilityTests;
} // namespace

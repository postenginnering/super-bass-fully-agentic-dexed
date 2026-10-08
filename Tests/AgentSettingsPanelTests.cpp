#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "security/CredentialStore.h"
#include "ui/AgentSettingsPanel.h"
#include "ui/WorkbenchTheme.h"

namespace
{
using namespace agentic_dexed;
using namespace agentic_dexed::agent;
using namespace agentic_dexed::security;
using namespace agentic_dexed::ui;

class AgentSettingsPanelTests final : public juce::UnitTest
{
public:
    AgentSettingsPanelTests()
        : juce::UnitTest("Secure Agent settings panel", "AgentSettings") {}

    void runTest() override
    {
        DexedAudioProcessor processor;
        AgentPreferences preferences;
        MemoryCredentialStore credentials;
        AgentSettingsPanel panel(
            processor.agentController(), preferences, credentials);

        beginTest("provider fields use dark readable text on the paper background");
        for (auto* editor : { &panel.baseUrlEditor(), &panel.modelEditor() })
        {
            expect(editor->findColour(juce::TextEditor::textColourId)
                   == WorkbenchTheme::ink);
            expect(editor->findColour(juce::TextEditor::backgroundColourId)
                   == WorkbenchTheme::paper);
            expect(WorkbenchTheme::contrastRatio(
                       editor->findColour(juce::TextEditor::textColourId),
                       editor->findColour(juce::TextEditor::backgroundColourId)) >= 7.0);
        }

        beginTest("stored secrets become a fixed mask and cannot be copied back");
        const juce::String literal = "sk-panel-secret-DO-NOT-LEAK";
        panel.secretField().setNewSecret(literal);
        expect(panel.secretField().copyAllowed());
        expect(panel.saveCredentialForTest());
        expect(!panel.secretField().copyAllowed());
        expect(!panel.secretField().displayText().contains(literal));
        expect(panel.secretField().newSecret().isEmpty());
        const auto loaded = credentials.load("agent.model");
        expect(loaded.ok());
        expectEquals(juce::String::fromUTF8(
                         loaded.secret.view().data(),
                         static_cast<int>(loaded.secret.view().size())), literal);

        beginTest("applying settings stores a newly entered replacement credential");
        const juce::String replacement = "sk-panel-replacement-DO-NOT-LEAK";
        panel.beginCredentialReplacement();
        expect(!panel.secretField().editor().isReadOnly());
        panel.secretField().editor().setText(replacement, false);
        expect(panel.applyPreferences());
        const auto replaced = credentials.load("agent.model");
        expect(replaced.ok());
        expectEquals(juce::String::fromUTF8(
                         replaced.secret.view().data(),
                         static_cast<int>(replaced.secret.view().size())), replacement);
        expect(!panel.secretField().copyAllowed());
        expect(panel.secretField().newSecret().isEmpty());

        beginTest("invalid provider URLs are blocked with an actionable reason");
        panel.baseUrlEditor().setText("http://example.com/v1");
        expect(!panel.applyPreferences());
        expect(panel.statusText().containsIgnoreCase("https"));

        beginTest("valid non-secret settings update preferences");
        panel.baseUrlEditor().setText("http://localhost:11434/v1");
        panel.modelEditor().setText("local-model");
        expect(panel.applyPreferences());
        expectEquals(preferences.baseUrl, std::string("http://localhost:11434/v1"));
        expectEquals(preferences.model, std::string("local-model"));

        beginTest("connection testing is cancellable");
        panel.startConnectionTest();
        expect(panel.connectionTestInProgress());
        panel.cancelConnectionTest();
        expect(!panel.connectionTestInProgress());

        beginTest("forget removes the credential and clears secret status");
        expect(panel.forgetCredentialForTest());
        expect(credentials.load("agent.model").status == CredentialStatus::notFound);
        expect(panel.secretField().displayText().isEmpty());
    }
};

AgentSettingsPanelTests agentSettingsPanelTests;
}

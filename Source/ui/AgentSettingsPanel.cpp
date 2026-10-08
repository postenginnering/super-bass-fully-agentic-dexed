#include "AgentSettingsPanel.h"

#include "../agent/AgentController.h"
#include "../agent/http/BaseUrlPolicy.h"
#include "../agent/http/IHttpTransport.h"
#include "../security/CredentialStore.h"

#include <algorithm>

namespace agentic_dexed::ui
{
namespace
{
constexpr const char* credentialId = "agent.model";
}

AgentSettingsPanel::AgentSettingsPanel(
    agent::AgentController& controller,
    agent::AgentPreferences& preferences,
    security::ICredentialStore& credentialStore)
    : WorkbenchPanel(juce::String::fromUTF8("智能体设置 / AGENT SETTINGS")),
      controller_(controller),
      preferences_(preferences), credentialStore_(credentialStore)
{
    protocol_.addItem("OpenAI Responses", 1);
    protocol_.addItem("Compatible Chat Completions", 2);
    protocol_.setSelectedId(
        preferences_.protocol == agent::model::ProviderProtocol::responses ? 1 : 2,
        juce::dontSendNotification);
    protocol_.setName("Provider protocol");
    protocol_.setTitle("Provider protocol");

    baseUrl_.setName("Base URL");
    baseUrl_.setTitle("HTTPS provider URL or loopback HTTP URL");
    baseUrl_.setText(juce::String::fromUTF8(preferences_.baseUrl.c_str()), false);
    model_.setName("Model");
    model_.setTitle("Provider model identifier");
    model_.setText(juce::String::fromUTF8(preferences_.model.c_str()), false);
    for (auto* editor : { &baseUrl_, &model_ })
    {
        editor->setColour(juce::TextEditor::textColourId, WorkbenchTheme::ink);
        editor->setColour(juce::TextEditor::backgroundColourId, WorkbenchTheme::paper);
        editor->setColour(juce::TextEditor::outlineColourId, WorkbenchTheme::inkSoft);
        editor->setColour(juce::TextEditor::focusedOutlineColourId,
                          WorkbenchTheme::accentBlue);
        editor->setColour(juce::TextEditor::highlightColourId,
                          WorkbenchTheme::focusBlue);
        editor->setColour(juce::TextEditor::highlightedTextColourId,
                          WorkbenchTheme::ink);
        editor->setColour(juce::CaretComponent::caretColourId,
                          WorkbenchTheme::ink);
    }

    applyMode_.addItem("Apply live", 1);
    applyMode_.addItem("Ask before applying", 2);
    applyMode_.setSelectedId(
        preferences_.applyMode == agent::AgentApplyMode::live ? 1 : 2,
        juce::dontSendNotification);
    applyMode_.setName("Agent apply mode");
    applyMode_.setTitle("Apply changes live or request confirmation");

    for (auto* component : std::initializer_list<juce::Component*> {
             &protocol_, &baseUrl_, &model_, &applyMode_, &secret_, &apply_, &saveKey_,
             &forgetKey_, &test_, &cancelTest_, &status_ })
        addAndMakeVisible(*component);

    apply_.onClick = [this] { applyPreferences(); };
    saveKey_.onClick = [this]
    {
        if (secret_.state() == SecretField::State::stored)
            beginCredentialReplacement();
        else
            saveCredentialAsync();
    };
    forgetKey_.onClick = [this] { forgetCredentialAsync(); };
    test_.onClick = [this] { startConnectionTest(); };
    cancelTest_.onClick = [this] { cancelConnectionTest(); };
    cancelTest_.setEnabled(false);

    probeCredentialAsync();
}

AgentSettingsPanel::~AgentSettingsPanel()
{
    cancelConnectionTest();
    for (auto& task : backgroundTasks_)
        if (task.valid())
            task.wait();
}

bool AgentSettingsPanel::applyPreferences()
{
    const auto validation = agent::http::validateBaseUrl(baseUrl_.getText().trim().toStdString());
    if (!validation.ok())
    {
        status_.setText(juce::String::fromUTF8(validation.error->message.c_str()),
                        juce::dontSendNotification);
        setWorkbenchState(WorkbenchState::error);
        return false;
    }
    if (model_.getText().trim().isEmpty())
    {
        status_.setText("A model name is required", juce::dontSendNotification);
        setWorkbenchState(WorkbenchState::error);
        return false;
    }

    bool credentialIsTemporary = false;
    auto rawSecret = secret_.newSecret().toStdString();
    if (!rawSecret.empty())
    {
        auto secret = security::SecureSecret(rawSecret);
        std::fill(rawSecret.begin(), rawSecret.end(), '\0');
        const auto result = credentialStore_.store(credentialId, secret.view());
        secret.clear();
        setCredentialResult(true, result.ok(), result.temporaryOnly,
                            juce::String::fromUTF8(result.sanitizedMessage.c_str()));
        if (!result.ok())
            return false;
        credentialIsTemporary = result.temporaryOnly;
    }

    preferences_.protocol = protocol_.getSelectedId() == 2
        ? agent::model::ProviderProtocol::chatCompletions
        : agent::model::ProviderProtocol::responses;
    preferences_.baseUrl = validation.value->value;
    preferences_.model = model_.getText().trim().toStdString();
    preferences_.applyMode = applyMode_.getSelectedId() == 2
        ? agent::AgentApplyMode::confirmation : agent::AgentApplyMode::live;
    status_.setText(
        credentialIsTemporary
            ? juce::String::fromUTF8(
                u8"Settings ready · credential available for this process only")
            : juce::String("Settings ready"),
        juce::dontSendNotification);
    setWorkbenchState(WorkbenchState::success);
    if (onPreferencesApplied)
        onPreferencesApplied();
    return true;
}

void AgentSettingsPanel::beginCredentialReplacement()
{
    secret_.clear();
    secret_.editor().grabKeyboardFocus();
    status_.setText(
        "Paste the replacement credential, then Apply or Test Connection",
        juce::dontSendNotification);
    setWorkbenchState(WorkbenchState::active);
}

bool AgentSettingsPanel::saveCredentialForTest()
{
    auto secret = secret_.newSecret().toStdString();
    if (secret.empty())
        return false;
    const auto result = credentialStore_.store(credentialId, secret);
    std::fill(secret.begin(), secret.end(), '\0');
    setCredentialResult(true, result.ok(), result.temporaryOnly,
                        juce::String::fromUTF8(result.sanitizedMessage.c_str()));
    return result.ok();
}

bool AgentSettingsPanel::forgetCredentialForTest()
{
    const auto result = credentialStore_.erase(credentialId);
    const auto ok = result.ok() || result.status == security::CredentialStatus::notFound;
    setCredentialResult(false, ok, false,
                        juce::String::fromUTF8(result.sanitizedMessage.c_str()));
    return ok;
}

void AgentSettingsPanel::saveCredentialAsync()
{
    auto rawSecret = secret_.newSecret().toStdString();
    auto secret = security::SecureSecret(rawSecret);
    std::fill(rawSecret.begin(), rawSecret.end(), '\0');
    if (secret.empty())
    {
        status_.setText("Paste a replacement key first", juce::dontSendNotification);
        return;
    }
    secret_.clear();
    status_.setText(juce::String::fromUTF8(u8"Saving credential…"),
                    juce::dontSendNotification);
    const auto safe = juce::Component::SafePointer<AgentSettingsPanel>(this);
    backgroundTasks_.push_back(std::async(std::launch::async,
        [this, safe, secret = std::move(secret)]() mutable
        {
            const auto result = credentialStore_.store(credentialId, secret.view());
            secret.clear();
            juce::MessageManager::callAsync([safe, result]
            {
                if (auto* panel = safe.getComponent())
                    panel->setCredentialResult(
                        true, result.ok(), result.temporaryOnly,
                        juce::String::fromUTF8(result.sanitizedMessage.c_str()));
            });
        }));
}

void AgentSettingsPanel::probeCredentialAsync()
{
    const auto safe = juce::Component::SafePointer<AgentSettingsPanel>(this);
    backgroundTasks_.push_back(std::async(std::launch::async, [this, safe]
    {
        auto result = credentialStore_.load(credentialId);
        const auto present = result.ok() && !result.secret.empty();
        result.secret.clear();
        juce::MessageManager::callAsync([safe, present]
        {
            if (present)
                if (auto* panel = safe.getComponent())
                    if (panel->secret_.state() == SecretField::State::empty)
                        panel->secret_.setStoredPlaceholder();
        });
    }));
}

void AgentSettingsPanel::forgetCredentialAsync()
{
    const auto safe = juce::Component::SafePointer<AgentSettingsPanel>(this);
    backgroundTasks_.push_back(std::async(std::launch::async, [this, safe]
    {
        const auto result = credentialStore_.erase(credentialId);
        juce::MessageManager::callAsync([safe, result]
        {
            if (auto* panel = safe.getComponent())
                panel->setCredentialResult(
                    false,
                    result.ok() || result.status == security::CredentialStatus::notFound,
                    false, juce::String::fromUTF8(result.sanitizedMessage.c_str()));
        });
    }));
}

void AgentSettingsPanel::setCredentialResult(
    bool saved, bool ok, bool temporary, juce::String message)
{
    if (ok && saved)
        secret_.setStoredPlaceholder();
    else if (ok)
        secret_.clear();
    if (message.isEmpty())
        message = saved ? "Credential saved" : "Credential forgotten";
    if (temporary)
        message += juce::String::fromUTF8(u8" · current process only");
    status_.setText(message, juce::dontSendNotification);
    setWorkbenchState(ok ? WorkbenchState::success : WorkbenchState::error);
}

void AgentSettingsPanel::startConnectionTest()
{
    if (!applyPreferences())
        return;
    cancelConnectionTest();
    status_.setText(juce::String::fromUTF8(u8"Testing connection…"),
                    juce::dontSendNotification);
    const auto safe = juce::Component::SafePointer<AgentSettingsPanel>(this);
    connectionHandle_ = controller_.testConnection(
        preferences_.providerConfig(),
        [safe](agent::ConnectionTestResult result)
        {
            if (auto* panel = safe.getComponent())
            {
                panel->connectionHandle_.reset();
                panel->test_.setEnabled(true);
                panel->cancelTest_.setEnabled(false);
                panel->status_.setText(
                    juce::String::fromUTF8(result.sanitizedMessage.c_str()),
                    juce::dontSendNotification);
                panel->setWorkbenchState(
                    result.success ? WorkbenchState::success : WorkbenchState::error);
            }
        });
    test_.setEnabled(false);
    cancelTest_.setEnabled(connectionHandle_ != nullptr);
}

void AgentSettingsPanel::cancelConnectionTest()
{
    if (connectionHandle_ != nullptr)
        connectionHandle_->cancel();
    connectionHandle_.reset();
    test_.setEnabled(true);
    cancelTest_.setEnabled(false);
}

void AgentSettingsPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    area.removeFromTop(24);
    const auto rowHeight = 28;
    protocol_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(6);
    baseUrl_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(6);
    model_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(6);
    applyMode_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(10);
    secret_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(8);
    auto keyActions = area.removeFromTop(rowHeight);
    saveKey_.setBounds(keyActions.removeFromLeft(keyActions.getWidth() / 2).reduced(0, 1));
    forgetKey_.setBounds(keyActions.reduced(4, 1));
    area.removeFromTop(6);
    auto actions = area.removeFromTop(rowHeight);
    apply_.setBounds(actions.removeFromLeft(actions.getWidth() / 3).reduced(0, 1));
    test_.setBounds(actions.removeFromLeft(actions.getWidth() / 2).reduced(4, 1));
    cancelTest_.setBounds(actions.reduced(0, 1));
    area.removeFromTop(8);
    status_.setBounds(area.removeFromTop(44));
}
}

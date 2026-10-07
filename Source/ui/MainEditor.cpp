#include "MainEditor.h"

#include "WorkbenchTheme.h"
#include "../agent/AgentController.h"

namespace agentic_dexed::ui
{
namespace
{
juce::PropertiesFile openPreferences()
{
    juce::PropertiesFile::Options options;
    return juce::PropertiesFile(
        DexedAudioProcessor::dexedAppDir.getChildFile("Dexed.xml"), options);
}

bool creativePage(WorkspacePage page) noexcept
{
    return page >= WorkspacePage::sound && page <= WorkspacePage::generate;
}
}

agent::AgentPreferences MainEditor::loadAgentPreferences(bool persist)
{
    if (!persist)
        return {};
    auto properties = openPreferences();
    return properties.isValidFile()
        ? agent::AgentPreferences::loadFrom(properties)
        : agent::AgentPreferences {};
}

MainEditor::MainEditor(DexedAudioProcessor& processor, bool persistPreferences)
    : processor_(processor), persistPreferences_(persistPreferences),
      preferences_(processor.agenticEditorPreferences),
      agentPreferences_(loadAgentPreferences(persistPreferences)),
      pageHost_(processor, preferences_, agentPreferences_, overlays_),
      keyboard_(processor.keyboardState,
                juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setName(juce::String::fromUTF8("Super Bass Fully Agentic Dexed 工作台 / WORKBENCH"));
    setTitle(getName());
    setAccessible(true);
    setWantsKeyboardFocus(true);
    setLookAndFeel(&lookAndFeel_);

    keyboard_.setName(juce::String::fromUTF8("演奏键盘 / PERFORMANCE KEYBOARD"));
    keyboard_.setTitle(keyboard_.getName());
    keyboard_.setLowestVisibleKey(24);
    keyboard_.setWantsKeyboardFocus(true);

    for (auto* child : std::initializer_list<juce::Component*> {
             &header_, &patchHeader_, &tabs_, &pageHost_, &statusBar_,
             &keyboard_, &overlays_ })
        addAndMakeVisible(*child);
    overlays_.setVisible(false);

    header_.onShowSynth = [this] { showSynth(); };
    header_.onShowSystem = [this] { showSystem(); };
    tabs_.onPageSelected = [this](WorkspacePage page) { setPage(page); };
    patchHeader_.onPrevious = [this]
    {
        const auto count = processor_.getNumPrograms();
        if (count > 0)
            processor_.setCurrentProgram(
                (processor_.getCurrentProgram() + count - 1) % count);
        processor_.updateHostDisplay();
        refreshState();
    };
    patchHeader_.onNext = [this]
    {
        const auto count = processor_.getNumPrograms();
        if (count > 0)
            processor_.setCurrentProgram((processor_.getCurrentProgram() + 1) % count);
        processor_.updateHostDisplay();
        refreshState();
    };
    patchHeader_.onSelectProgram = [this](int index)
    {
        processor_.setCurrentProgram(index);
        processor_.updateHostDisplay();
        refreshState();
    };
    patchHeader_.onImport = [this] { requestOpenPreset(); };
    patchHeader_.onSave = [this] { requestSavePreset(); };
    pageHost_.presetPage().onRequestOpen = [this] { requestOpenPreset(); };
    pageHost_.presetPage().onRequestSave = [this] { requestSavePreset(); };
    pageHost_.generatePage().agentPanel().onSavePreset = [this] { requestSaveCurrentPreset(); };
    pageHost_.systemPage().onRequestScl = [this] { requestOpenScl(); };
    pageHost_.systemPage().onRequestKbm = [this] { requestOpenKbm(); };
    pageHost_.onFeedback = [this](juce::String message, WorkbenchState state)
    {
        statusBar_.setMessage(std::move(message), state);
    };

    lastSynthPage_ = juce::isPositiveAndBelow(preferences_.selectedPage, 5)
        ? static_cast<WorkspacePage>(preferences_.selectedPage)
        : WorkspacePage::sound;
    setReducedMotion(preferences_.reducedMotion);
    setKeyboardExpanded(preferences_.keyboardExpanded);
    setPage(lastSynthPage_);
    refreshState();
    startTimerHz(20);
}

MainEditor::~MainEditor()
{
    stopTimer();
    overlays_.close();
    processor_.agenticEditorPreferences = preferences_;
    processor_.showKeyboard = preferences_.keyboardExpanded;
    if (persistPreferences_)
    {
        saveAgentPreferences();
        processor_.savePreference();
    }
    setLookAndFeel(nullptr);
}

void MainEditor::setPage(WorkspacePage page)
{
    if (creativePage(page))
    {
        lastSynthPage_ = page;
        preferences_.selectedPage = static_cast<int>(page);
        tabs_.setSelectedPage(page, juce::dontSendNotification);
    }
    header_.setSystemMode(page == WorkspacePage::system);
    pageHost_.show(page);
    statusBar_.setMessage(
        page == WorkspacePage::system
            ? juce::String::fromUTF8("系统设置 / SYSTEM SETTINGS")
            : juce::String::fromUTF8("工作区已切换 / WORKSPACE READY"));
}

void MainEditor::showSystem() { setPage(WorkspacePage::system); }
void MainEditor::showSynth() { setPage(lastSynthPage_); }

void MainEditor::refreshPrograms() { patchHeader_.refresh(processor_); }

void MainEditor::refreshState()
{
    pageHost_.refresh();
    patchHeader_.refresh(processor_);
    statusBar_.refresh(processor_);
}

void MainEditor::setKeyboardExpanded(bool expanded)
{
    preferences_.keyboardExpanded = expanded;
    processor_.showKeyboard = expanded;
    keyboard_.setVisible(expanded);
    pageHost_.systemPage().refresh();
    resized();
}

void MainEditor::setReducedMotion(bool reduced)
{
    preferences_.reducedMotion = reduced;
    pageHost_.setReducedMotion(reduced);
    pageHost_.systemPage().refresh();
}

void MainEditor::setScalePercent(int percent)
{
    if (!WorkbenchTheme::isScalePreset(percent))
        return;
    preferences_.scalePercent = percent;
    pageHost_.systemPage().refresh();
    if (scaleRequest_)
        scaleRequest_(percent);
}

bool MainEditor::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    if ((modifiers.isCtrlDown() || modifiers.isCommandDown())
        && key.getKeyCode() >= '1' && key.getKeyCode() <= '5')
    {
        setPage(static_cast<WorkspacePage>(key.getKeyCode() - '1'));
        return true;
    }
    if (key == juce::KeyPress::escapeKey && overlays_.hasOverlay())
    {
        overlays_.close();
        overlays_.setVisible(false);
        return true;
    }
    return false;
}

bool MainEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    return !files.isEmpty() && std::all_of(files.begin(), files.end(),
        [](const juce::String& path)
        {
            return path.endsWithIgnoreCase(".syx")
                || path.endsWithIgnoreCase(".dexedpreset")
                || path.endsWithIgnoreCase(".scl")
                || path.endsWithIgnoreCase(".kbm");
        });
}

void MainEditor::handleFilesDropped(const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const juce::File file(path);
        if (path.endsWithIgnoreCase(".dexedpreset"))
        {
            juce::MemoryBlock data;
            if (file.getSize() <= 0 || file.getSize() > 16 * 1024 * 1024 || !file.loadFileAsData(data))
            {
                statusBar_.setMessage(juce::String::fromUTF8(u8"无法读取这个预设文件。"), WorkbenchState::error);
                continue;
            }
            const auto xml = juce::AudioProcessor::getXmlFromBinary(data.getData(), static_cast<int>(data.getSize()));
            if (!xml || !xml->hasTagName("dexedState") || xml->getChildByName("dexedBlob") == nullptr)
            {
                statusBar_.setMessage(juce::String::fromUTF8(u8"预设文件无效，音色未改变。"), WorkbenchState::error);
                continue;
            }
            processor_.agentController().cancel();
            processor_.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
            refreshState();
            if (processor_.lastAgentContextImportSucceeded())
                statusBar_.setMessage(juce::String::fromUTF8(u8"预设已载入。"), WorkbenchState::success);
            else
                statusBar_.setMessage(
                    juce::String::fromUTF8(u8"音色已载入，但对话上下文损坏，已使用新的空白上下文。"),
                    WorkbenchState::warning);
        }
        else if (path.endsWithIgnoreCase(".syx"))
        {
            setPage(WorkspacePage::presets);
            statusBar_.setMessage(
                juce::String::fromUTF8("正在打开音色库 / Opening cartridge: ")
                    + file.getFileName(), WorkbenchState::active);
            pageHost_.presetPage().openBrowserFile(file);
        }
        else if (path.endsWithIgnoreCase(".scl"))
        {
            showSystem();
            pageHost_.systemPage().applySclFile(file);
            statusBar_.setMessage(
                juce::String::fromUTF8("已处理 SCL 调律 / SCL tuning processed"),
                WorkbenchState::success);
        }
        else if (path.endsWithIgnoreCase(".kbm"))
        {
            showSystem();
            pageHost_.systemPage().applyKbmFile(file);
            statusBar_.setMessage(
                juce::String::fromUTF8("已处理 KBM 映射 / KBM mapping processed"),
                WorkbenchState::success);
        }
        else
        {
            statusBar_.setMessage(
                juce::String::fromUTF8("不支持的文件 / Unsupported file: ")
                    + file.getFileName(), WorkbenchState::warning);
        }
    }
}

void MainEditor::showMidiLearn(std::string parameterId)
{
    showSystem();
    pageHost_.systemPage().showMidiLearn(std::move(parameterId));
}

void MainEditor::setParameterMessage(juce::String message)
{
    if (message.isEmpty())
        statusBar_.setMessage(
            juce::String::fromUTF8("音频引擎已就绪 / AUDIO ENGINE READY"));
    else
        statusBar_.setMessage(std::move(message), WorkbenchState::active);
}

void MainEditor::requestOpenPreset()
{
    const auto safe = juce::Component::SafePointer<MainEditor>(this);
    FileCompletion completion = [safe](juce::File file)
    {
        if (auto* editor = safe.getComponent(); editor != nullptr && file != juce::File {})
            editor->handleFilesDropped({ file.getFullPathName() });
    };
    if (chooserRequests_.openPreset)
        chooserRequests_.openPreset(std::move(completion));
    else
        launchChooser(juce::String::fromUTF8("打开 DX7 音色库 / Open DX7 cartridge"),
                      "*.syx;*.SYX;*.dexedpreset",
                      juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
                      std::move(completion));
}

void MainEditor::requestSavePreset()
{
    const auto safe = juce::Component::SafePointer<MainEditor>(this);
    FileCompletion completion = [safe](juce::File file)
    {
        if (auto* editor = safe.getComponent(); editor != nullptr && file != juce::File {})
            editor->pageHost_.presetPage().saveActiveFile(file, true);
    };
    if (chooserRequests_.savePreset)
        chooserRequests_.savePreset(std::move(completion));
    else
        launchChooser(juce::String::fromUTF8("保存 DX7 音色库 / Save DX7 cartridge"),
                      "*.syx;*.SYX",
                      juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
                      std::move(completion));
}

void MainEditor::requestSaveCurrentPreset()
{
    const auto safe = juce::Component::SafePointer<MainEditor>(this);
    FileCompletion completion = [safe](juce::File file)
    {
        auto* editor = safe.getComponent();
        if (!editor || file == juce::File {}) return;
        if (!file.hasFileExtension("dexedpreset"))
        {
            file = file.withFileExtension("dexedpreset");
            if (file.existsAsFile())
            {
                editor->statusBar_.setMessage(juce::String::fromUTF8(u8"同名预设已存在，请重新选择文件名。"), WorkbenchState::warning);
                return;
            }
        }
        juce::MemoryBlock state;
        editor->processor_.getStateInformation(state);
        const auto saved = file.replaceWithData(state.getData(), state.getSize());
        editor->statusBar_.setMessage(juce::String::fromUTF8(saved
            ? u8"当前音色已保存为预设，可通过导入或拖入文件重新载入。"
            : u8"预设保存失败，请检查保存位置。"), saved ? WorkbenchState::success : WorkbenchState::error);
    };
    if (chooserRequests_.saveCurrentPreset)
        chooserRequests_.saveCurrentPreset(std::move(completion));
    else
        launchChooser(juce::String::fromUTF8(u8"保存当前音色为预设"), "*.dexedpreset",
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            std::move(completion));
}

void MainEditor::requestOpenScl()
{
    const auto safe = juce::Component::SafePointer<MainEditor>(this);
    FileCompletion completion = [safe](juce::File file)
    {
        if (auto* editor = safe.getComponent(); editor != nullptr && file != juce::File {})
            editor->handleFilesDropped({ file.getFullPathName() });
    };
    if (chooserRequests_.openScl)
        chooserRequests_.openScl(std::move(completion));
    else
        launchChooser("Open Scala tuning", "*.scl",
                      juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
                      std::move(completion));
}

void MainEditor::requestOpenKbm()
{
    const auto safe = juce::Component::SafePointer<MainEditor>(this);
    FileCompletion completion = [safe](juce::File file)
    {
        if (auto* editor = safe.getComponent(); editor != nullptr && file != juce::File {})
            editor->handleFilesDropped({ file.getFullPathName() });
    };
    if (chooserRequests_.openKbm)
        chooserRequests_.openKbm(std::move(completion));
    else
        launchChooser("Open keyboard mapping", "*.kbm",
                      juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
                      std::move(completion));
}

void MainEditor::launchChooser(
    juce::String title, juce::String wildcard, int flags, FileCompletion completion)
{
    chooser_ = std::make_unique<juce::FileChooser>(
        std::move(title), DexedAudioProcessor::dexedCartDir, std::move(wildcard));
    const auto safe = juce::Component::SafePointer<MainEditor>(this);
    chooser_->launchAsync(flags, [safe, completion = std::move(completion)](
                                      const juce::FileChooser& chooser)
    {
        if (auto* editor = safe.getComponent())
        {
            auto file = chooser.getResult();
            editor->chooser_.reset();
            if (completion && file != juce::File {})
                completion(std::move(file));
        }
    });
}

void MainEditor::timerCallback()
{
    patchHeader_.refresh(processor_);
    statusBar_.refresh(processor_);
}

void MainEditor::saveAgentPreferences()
{
    auto properties = openPreferences();
    agentPreferences_.saveTo(properties);
    properties.save();
}

void MainEditor::captureSize()
{
    if (externalWindowSize_)
        return;
    if (getWidth() >= WorkbenchTheme::minimumWidth)
        preferences_.width = juce::jlimit(
            WorkbenchTheme::minimumWidth, 2560, getWidth());
    if (getHeight() >= WorkbenchTheme::minimumHeight)
        preferences_.height = juce::jlimit(
            WorkbenchTheme::minimumHeight, 1520, getHeight());
}

void MainEditor::paint(juce::Graphics& graphics)
{
    WorkbenchTheme::paintCanvas(graphics, getLocalBounds());
}

void MainEditor::resized()
{
    captureSize();
    auto area = getLocalBounds();
    header_.setBounds(area.removeFromTop(44));
    patchHeader_.setBounds(area.removeFromTop(58));
    tabs_.setBounds(area.removeFromTop(46));
    statusBar_.setBounds(area.removeFromBottom(32));
    if (preferences_.keyboardExpanded)
    {
        keyboard_.setVisible(true);
        keyboard_.setBounds(area.removeFromBottom(64));
    }
    else
    {
        keyboard_.setVisible(false);
        keyboard_.setBounds({});
    }
    pageHost_.setBounds(area);
    overlays_.setBounds(getLocalBounds());
    if (overlays_.hasOverlay())
        overlays_.toFront(true);
}
}

#include "PresetLibraryService.h"

#include "../../PluginProcessor.h"
#include "../../agent/AgentController.h"
#include "../../agent/context/ConversationContextStore.h"

#include <algorithm>
#include <cstring>

namespace agentic_dexed::ui
{
namespace
{
struct CartridgeLoadResult
{
    UiOperationResult result;
    Cartridge cartridge;
};

juce::String exactName(const std::array<uint8_t, 10>& bytes)
{
    return juce::String::fromUTF8(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()));
}

CartridgeLoadResult loadCartridgeFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return { { false, juce::String::fromUTF8("文件不存在 / Cartridge file not found"), false }, {} };
    if (!file.hasFileExtension("syx;SYX"))
        return { { false, juce::String::fromUTF8("请选择 DX7 .syx 文件 / Choose a DX7 .syx file"), false }, {} };

    Cartridge candidate;
    const auto loadResult = candidate.load(file);
    if (loadResult == -1)
        return { { false, juce::String::fromUTF8("无法读取文件 / Unable to read cartridge"), false }, {} };
    if (loadResult == 1)
        return { { false, juce::String::fromUTF8("DX7 校验和错误 / DX7 checksum mismatch"), false }, {} };
    if (loadResult != 0)
        return { { false, juce::String::fromUTF8("文件不是 DX7 32 音色库 / Not a DX7 32-program cartridge"), false }, {} };
    return { { true, juce::String::fromUTF8("音色库已打开 / Cartridge opened"), false },
             candidate };
}

agent::context::CanonicalVoice canonicalVoice(Cartridge& cartridge, int slot)
{
    std::array<std::uint8_t, 161> unpacked {};
    cartridge.unpackProgram(unpacked.data(), slot);
    agent::context::CanonicalVoice result {};
    std::copy_n(unpacked.begin(), result.size(), result.begin());
    return result;
}

agent::context::CanonicalVoice currentVoice(const DexedAudioProcessor& processor)
{
    agent::context::CanonicalVoice result {};
    std::copy_n(processor.data, result.size(), result.begin());
    return result;
}
}

PresetLibraryService::PresetLibraryService(
    DexedAudioProcessor& processor,
    std::shared_ptr<agent::context::PresetIdentityService> identities)
    : processor_(processor), identities_(std::move(identities)),
      userDirectory_(DexedAudioProcessor::dexedCartDir)
{
    if (identities_ == nullptr && processor_.hasAgentController())
        identities_ = std::make_shared<agent::context::PresetIdentityService>(
            agent::context::ConversationContextStore::defaultRoot()
                .getChildFile("preset-index.json"));
    if (processor_.hasAgentController())
        if (const auto activation = activationForActiveSlot(processor_.getCurrentProgram()))
            activateAgentContext(*activation);
}

PresetLibraryService::~PresetLibraryService()
{
    alive_->store(false);
    ioPool_.removeAllJobs(true, 5000);
}

UiOperationResult PresetLibraryService::success(juce::String message)
{
    return { true, std::move(message), false };
}

UiOperationResult PresetLibraryService::failure(
    juce::String message, bool overwrite)
{
    return { false, std::move(message), overwrite };
}

std::vector<PresetSlot> PresetLibraryService::activeSlots() const
{
    std::vector<PresetSlot> result;
    result.reserve(32);
    for (int index = 0; index < 32; ++index)
        result.push_back({ index, processor_.currentCart.getProgramName(index),
                           index == processor_.getCurrentProgram(),
                           index == processor_.getCurrentProgram() });
    return result;
}

std::vector<PresetSlot> PresetLibraryService::browserSlots() const
{
    std::vector<PresetSlot> result;
    if (!hasBrowserCart_)
        return result;
    result.reserve(32);
    for (int index = 0; index < 32; ++index)
        result.push_back({ index, browserCart_.getProgramName(index),
                           index == browserSelection_, false });
    return result;
}

const juce::Array<juce::File>&
PresetLibraryService::recentCartridges() const noexcept
{
    return recentCartridges_;
}

UiOperationResult PresetLibraryService::setUserDirectory(const juce::File& directory)
{
    if (!directory.isDirectory())
        return failure(juce::String::fromUTF8("预设目录不存在 / Preset directory unavailable"));
    userDirectory_ = directory;
    return refreshBrowser();
}

UiOperationResult PresetLibraryService::refreshBrowser()
{
    if (!userDirectory_.isDirectory())
        return failure(juce::String::fromUTF8("无法读取预设目录 / Unable to read preset directory"));
    return success(juce::String::fromUTF8("预设目录已刷新 / Preset directory refreshed"));
}

void PresetLibraryService::remember(const juce::File& file)
{
    recentCartridges_.removeAllInstancesOf(file);
    recentCartridges_.insert(0, file);
    while (recentCartridges_.size() > 8)
        recentCartridges_.removeLast();
}

UiOperationResult PresetLibraryService::openBrowserCartridge(const juce::File& file)
{
    const auto loaded = loadCartridgeFile(file);
    if (!loaded.result.ok)
        return loaded.result;
    return commitBrowserCartridge(file, loaded.cartridge);
}

UiOperationResult PresetLibraryService::commitBrowserCartridge(
    const juce::File& file, const Cartridge& cartridge)
{
    browserCart_ = cartridge;
    hasBrowserCart_ = true;
    browserSelection_ = -1;
    browserFile_ = file;
    remember(file);
    return success(juce::String::fromUTF8("音色库已打开 / Cartridge opened"));
}

void PresetLibraryService::openBrowserCartridgeAsync(
    juce::File file, Completion completion)
{
    const auto alive = alive_;
    ioPool_.addJob([this, alive, file = std::move(file),
                    completion = std::move(completion)]() mutable
    {
        auto loaded = loadCartridgeFile(file);
        juce::MessageManager::callAsync(
            [this, alive, file = std::move(file), loaded = std::move(loaded),
             completion = std::move(completion)]() mutable
            {
                if (!alive->load())
                    return;
                auto result = loaded.result.ok
                    ? commitBrowserCartridge(file, loaded.cartridge)
                    : loaded.result;
                if (completion)
                    completion(std::move(result));
            });
    });
}

UiOperationResult PresetLibraryService::activateBrowserSlot(int index)
{
    if (!hasBrowserCart_)
        return failure(juce::String::fromUTF8("请先打开音色库 / Open a cartridge first"));
    if (!validSlot(index))
        return failure(juce::String::fromUTF8("音色编号无效 / Invalid preset slot"));

    cancelAgentRequest();
    std::array<uint8_t, 161> unpacked {};
    browserCart_.unpackProgram(unpacked.data(), index);
    unpacked[155] = sysexChecksum(unpacked.data(), 155);
    if (processor_.updateProgramFromSysex(unpacked.data()) != 0)
        return failure(juce::String::fromUTF8("音色数据无效 / Invalid program data"));
    browserSelection_ = index;
    if (identities_ != nullptr && processor_.hasAgentController())
    {
        const auto activation = identities_->activateVoice(canonicalVoice(browserCart_, index));
        if (!activateAgentContext(activation))
            return failure(juce::String::fromUTF8(u8"音色已载入，但无法切换对话上下文。"));
    }
    processor_.updateHostDisplay();
    return success(juce::String::fromUTF8("试听音色已载入 / Browser preset activated"));
}

UiOperationResult PresetLibraryService::activateActiveSlot(int index)
{
    if (!validSlot(index))
        return failure(juce::String::fromUTF8("音色编号无效 / Invalid preset slot"));
    cancelAgentRequest();
    captureCurrentFingerprint();
    processor_.setCurrentProgram(index);
    if (const auto activation = activationForActiveSlot(index);
        activation.has_value() && !activateAgentContext(*activation))
        return failure(juce::String::fromUTF8(u8"音色已载入，但无法切换对话上下文。"));
    processor_.updateHostDisplay();
    return success(juce::String::fromUTF8("当前音色已载入 / Active preset loaded"));
}

UiOperationResult PresetLibraryService::copyBrowserToActive(
    int source, int destination)
{
    if (!hasBrowserCart_)
        return failure(juce::String::fromUTF8("请先打开音色库 / Open a cartridge first"));
    if (!validSlot(source) || !validSlot(destination))
        return failure(juce::String::fromUTF8("音色编号无效 / Invalid preset slot"));

    cancelAgentRequest();
    std::optional<agent::context::PresetActivation> copiedActivation;
    if (identities_ != nullptr && processor_.hasAgentController())
    {
        const auto sourceActivation = identities_->activateVoice(
            canonicalVoice(browserCart_, source));
        const auto copiedId = identities_->clonePreset(sourceActivation.presetId);
        if (!copiedId.has_value()
            || !processor_.agentController().clonePresetContext(
                sourceActivation.presetId, *copiedId))
            return failure(juce::String::fromUTF8(u8"无法复制音色对话上下文。"));
        copiedActivation = agent::context::PresetActivation {
            *copiedId, sourceActivation.fingerprint, true };
    }

    auto candidate = processor_.currentCart;
    const auto bytes = browserCart_.programBytes(source);
    candidate.replaceProgram(destination, bytes.data());
    processor_.loadCartridge(candidate);
    if (copiedActivation.has_value())
        identities_->bindSlot(destination, copiedActivation->presetId);
    if (destination == processor_.getCurrentProgram())
    {
        processor_.setCurrentProgram(destination);
        if (copiedActivation.has_value())
            activateAgentContext(*copiedActivation);
    }
    return success(juce::String::fromUTF8("音色已复制 / Preset copied"));
}

UiOperationResult PresetLibraryService::moveActiveSlot(int source, int destination)
{
    if (!validSlot(source) || !validSlot(destination))
        return failure(juce::String::fromUTF8("音色编号无效 / Invalid preset slot"));
    if (source == destination)
        return success(juce::String::fromUTF8("位置未改变 / Preset position unchanged"));

    cancelAgentRequest();
    captureCurrentFingerprint();
    if (identities_ != nullptr)
        for (int slot = std::min(source, destination);
             slot <= std::max(source, destination); ++slot)
            activationForActiveSlot(slot);
    auto candidate = processor_.currentCart;
    const auto moved = candidate.programBytes(source);
    if (source < destination)
        for (int index = source; index < destination; ++index)
        {
            const auto next = candidate.programBytes(index + 1);
            candidate.replaceProgram(index, next.data());
        }
    else
        for (int index = source; index > destination; --index)
        {
            const auto previous = candidate.programBytes(index - 1);
            candidate.replaceProgram(index, previous.data());
        }
    candidate.replaceProgram(destination, moved.data());

    auto current = processor_.getCurrentProgram();
    if (current == source)
        current = destination;
    else if (source < destination && current > source && current <= destination)
        --current;
    else if (destination < source && current >= destination && current < source)
        ++current;
    processor_.loadCartridge(candidate);
    if (identities_ != nullptr)
        identities_->moveSlot(source, destination);
    processor_.setCurrentProgram(current);
    if (const auto activation = activationForActiveSlot(current))
        activateAgentContext(*activation);
    return success(juce::String::fromUTF8("音色已移动 / Preset moved"));
}

Dx7NamePreview PresetLibraryService::previewDx7Name(juce::String requested) const
{
    Dx7NamePreview result;
    result.requested = std::move(requested);
    result.bytes.fill(static_cast<uint8_t>(' '));

    auto input = result.requested.getCharPointer();
    std::size_t output = 0;
    while (!input.isEmpty() && output < result.bytes.size())
    {
        auto character = input.getAndAdvance();
        if (character == 0x3000)
            character = ' ';
        else if (character >= 0xFF01 && character <= 0xFF5E)
            character = character - 0xFF01 + 0x21;

        uint8_t encoded = '?';
        if (character >= 32 && character <= 126)
            encoded = static_cast<uint8_t>(character);
        if (encoded == '\\')
            encoded = 'Y';
        else if (encoded == '~')
            encoded = '>';
        result.bytes[output++] = encoded;
    }

    result.normalized = exactName(result.bytes);
    result.changed = result.requested != result.normalized;
    return result;
}

UiOperationResult PresetLibraryService::renameActiveSlot(
    int index, const Dx7NamePreview& preview)
{
    if (!validSlot(index))
        return failure(juce::String::fromUTF8("音色编号无效 / Invalid preset slot"));
    if (index == processor_.getCurrentProgram())
        cancelAgentRequest();
    auto candidate = processor_.currentCart;
    candidate.setProgramNameBytes(index, preview.bytes);
    processor_.loadCartridge(candidate);
    if (index == processor_.getCurrentProgram())
        processor_.setCurrentProgram(index);
    if (identities_ != nullptr)
        if (const auto id = identities_->presetForSlot(index))
            identities_->updateFingerprint(
                *id, canonicalVoice(processor_.currentCart, index));
    return success(juce::String::fromUTF8("音色已重命名 / Preset renamed"));
}

UiOperationResult PresetLibraryService::storeCurrentProgram(
    int destination, const Dx7NamePreview& preview)
{
    if (!validSlot(destination))
        return failure(juce::String::fromUTF8("音色编号无效 / Invalid preset slot"));
    cancelAgentRequest();
    const auto sourceSlot = processor_.getCurrentProgram();
    const auto sourceActivation = activationForActiveSlot(sourceSlot);
    std::optional<agent::context::PresetActivation> destinationActivation;
    if (identities_ != nullptr && processor_.hasAgentController()
        && sourceActivation.has_value() && destination != sourceSlot)
    {
        const auto clone = identities_->clonePreset(sourceActivation->presetId);
        if (!clone.has_value()
            || !processor_.agentController().clonePresetContext(
                sourceActivation->presetId, *clone))
            return failure(juce::String::fromUTF8(u8"无法复制音色对话上下文。"));
        destinationActivation = agent::context::PresetActivation { *clone, {}, true };
    }
    else
        destinationActivation = sourceActivation;

    auto candidate = processor_.currentCart;
    candidate.packProgram(processor_.data, destination,
                          preview.normalized, processor_.controllers.opSwitch);
    candidate.setProgramNameBytes(destination, preview.bytes);
    processor_.loadCartridge(candidate);
    if (destinationActivation.has_value() && identities_ != nullptr)
    {
        identities_->bindSlot(destination, destinationActivation->presetId);
        identities_->updateFingerprint(destinationActivation->presetId,
            canonicalVoice(processor_.currentCart, destination));
    }
    processor_.setCurrentProgram(destination);
    if (destinationActivation.has_value())
        activateAgentContext(*destinationActivation);
    processor_.updateHostDisplay();
    return success(juce::String::fromUTF8("当前音色已存储 / Current program stored"));
}

UiOperationResult PresetLibraryService::initializeCurrentProgram()
{
    cancelAgentRequest();
    processor_.resetToInitVoice();
    if (const auto activation = freshActivation())
    {
        identities_->bindSlot(processor_.getCurrentProgram(), activation->presetId);
        activateAgentContext(*activation);
    }
    processor_.updateHostDisplay();
    return success(juce::String::fromUTF8("当前音色已初始化 / Current program initialized"));
}

void PresetLibraryService::adoptCurrentAgentContext()
{
    if (identities_ == nullptr || !processor_.hasAgentController())
        return;
    const auto conversation = processor_.agentController().currentConversation();
    if (conversation.empty())
        return;
    const auto slot = processor_.getCurrentProgram();
    identities_->adoptPreset(conversation->presetId, currentVoice(processor_));
    identities_->bindSlot(slot, conversation->presetId);
}

UiOperationResult PresetLibraryService::createActiveCartridge()
{
    cancelAgentRequest();
    Cartridge candidate;
    for (int index = 0; index < 32; ++index)
    {
        const auto name = "INIT " + juce::String(index + 1).paddedLeft('0', 2);
        candidate.packProgram(processor_.data, index, name,
                              processor_.controllers.opSwitch);
    }
    processor_.loadCartridge(candidate);
    processor_.activeFileCartridge = juce::File {};
    processor_.setCurrentProgram(0);
    std::optional<agent::context::PresetActivation> first;
    if (identities_ != nullptr && processor_.hasAgentController())
        for (int index = 0; index < 32; ++index)
            if (const auto activation = freshActivation())
            {
                identities_->bindSlot(index, activation->presetId);
                if (index == 0)
                    first = activation;
            }
    if (first.has_value())
        activateAgentContext(*first);
    return success(juce::String::fromUTF8("新音色库已创建 / New cartridge created"));
}

std::optional<agent::context::PresetActivation>
PresetLibraryService::activationForActiveSlot(int index)
{
    if (identities_ == nullptr || !validSlot(index))
        return std::nullopt;
    const auto voice = canonicalVoice(processor_.currentCart, index);
    const auto fingerprint = agent::context::PresetIdentityService::fingerprint(voice);
    if (const auto existing = identities_->presetForSlot(index))
    {
        identities_->updateFingerprint(*existing, voice);
        return agent::context::PresetActivation { *existing, fingerprint, false };
    }

    auto activation = identities_->activateVoice(voice);
    bool boundElsewhere = false;
    for (int slot = 0; slot < 32; ++slot)
        if (slot != index && identities_->presetForSlot(slot) == activation.presetId)
        {
            boundElsewhere = true;
            break;
        }
    if (boundElsewhere)
    {
        const auto fresh = identities_->createPreset();
        if (!fresh.has_value())
            return std::nullopt;
        activation = { *fresh, fingerprint, true };
    }
    if (!identities_->bindSlot(index, activation.presetId))
        return std::nullopt;
    return activation;
}

std::optional<agent::context::PresetActivation>
PresetLibraryService::freshActivation()
{
    if (identities_ == nullptr || !processor_.hasAgentController())
        return std::nullopt;
    const auto id = identities_->createPreset();
    if (!id.has_value())
        return std::nullopt;
    return agent::context::PresetActivation { *id, {}, true };
}

bool PresetLibraryService::activateAgentContext(
    const agent::context::PresetActivation& activation)
{
    return !processor_.hasAgentController()
        || processor_.agentController().activatePreset(activation);
}

void PresetLibraryService::captureCurrentFingerprint()
{
    if (identities_ == nullptr)
        return;
    if (const auto id = identities_->presetForSlot(processor_.getCurrentProgram()))
        identities_->updateFingerprint(*id, currentVoice(processor_));
}

void PresetLibraryService::cancelAgentRequest()
{
    if (processor_.hasAgentController())
        processor_.agentController().cancel();
}

UiOperationResult PresetLibraryService::saveActiveCartridge(
    const juce::File& file, bool overwrite)
{
    if (file == juce::File())
        return failure(juce::String::fromUTF8("未选择文件 / No destination selected"));
    if (!file.hasFileExtension("syx;SYX"))
        return failure(juce::String::fromUTF8("请使用 .syx 扩展名 / Use the .syx extension"));
    if (file.existsAsFile() && !overwrite)
        return failure(juce::String::fromUTF8("文件已存在，确认覆盖 / File exists; confirm overwrite"), true);

    auto candidate = processor_.currentCart;
    if (!candidate.saveVoice(file))
        return failure(juce::String::fromUTF8("无法写入文件 / Unable to write cartridge"));
    processor_.activeFileCartridge = file;
    remember(file);
    return success(juce::String::fromUTF8("音色库已保存 / Cartridge saved"));
}

void PresetLibraryService::saveActiveCartridgeAsync(
    juce::File file, bool overwrite, Completion completion)
{
    const auto alive = alive_;
    auto candidate = processor_.currentCart;
    // Capture the edited voice, not the stale cartridge slot it was loaded from.
    agentic_dexed::RealtimeSynthState realtime;
    if (!processor_.readAgenticRealtimeState(realtime))
    {
        if (completion) completion(failure(juce::String::fromUTF8(u8"当前音色暂时无法读取，请重试。")));
        return;
    }
    char enabled[7] {};
    for (int op = 0; op < 6; ++op)
        enabled[op] = (realtime.voiceBytes[155] & (1u << op)) != 0 ? '1' : '0';
    candidate.packProgram(realtime.voiceBytes.data(), processor_.getCurrentProgram(),
                          juce::String::fromUTF8(processor_.agenticPatchName().c_str()),
                          enabled);
    ioPool_.addJob([this, alive, file = std::move(file), overwrite,
                    candidate = std::move(candidate),
                    completion = std::move(completion)]() mutable
    {
        UiOperationResult result;
        if (file == juce::File())
            result = failure(juce::String::fromUTF8("未选择文件 / No destination selected"));
        else if (!file.hasFileExtension("syx;SYX"))
            result = failure(juce::String::fromUTF8("请使用 .syx 扩展名 / Use the .syx extension"));
        else if (file.existsAsFile() && !overwrite)
            result = failure(juce::String::fromUTF8("文件已存在，确认覆盖 / File exists; confirm overwrite"), true);
        else if (!candidate.saveVoice(file))
            result = failure(juce::String::fromUTF8("无法写入文件 / Unable to write cartridge"));
        else
            result = success(juce::String::fromUTF8("音色库已保存 / Cartridge saved"));

        juce::MessageManager::callAsync(
            [this, alive, file = std::move(file), result = std::move(result),
             completion = std::move(completion)]() mutable
            {
                if (!alive->load())
                    return;
                if (result.ok)
                {
                    processor_.activeFileCartridge = file;
                    remember(file);
                }
                if (completion)
                    completion(std::move(result));
            });
    });
}
}

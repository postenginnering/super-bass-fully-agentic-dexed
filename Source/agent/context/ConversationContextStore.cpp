#include "ConversationContextStore.h"
#include "../memory/SensitiveDataFilter.h"

#include <algorithm>
#include <array>
#include <mutex>

namespace agentic_dexed::agent::context {
namespace {

std::mutex processMutex;

juce::String string(std::string_view value)
{
    return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
}

juce::String lockName(const juce::File& file)
{
    auto path = file.getFullPathName();
   #if JUCE_WINDOWS
    path = path.toLowerCase();
   #endif
    return "SBFAD-context-" + juce::String::toHexString(path.hashCode64());
}

ContextLoadResult loadFailure(const PresetId& id, const char* error)
{
    PresetConversationContext context;
    context.presetId = id;
    return { false, false, std::move(context), error };
}

bool compressJson(const juce::String& json, juce::MemoryBlock& destination)
{
    juce::MemoryOutputStream output;
    {
        juce::GZIPCompressorOutputStream compressor(output, 6);
        if (!compressor.write(json.toRawUTF8(), static_cast<std::size_t>(json.getNumBytesAsUTF8())))
            return false;
        compressor.flush();
    }
    destination = output.getMemoryBlock();
    return !destination.isEmpty();
}

std::optional<std::string> decompressJson(const juce::MemoryBlock& source)
{
    juce::MemoryInputStream input(source, false);
    juce::GZIPDecompressorInputStream decompressor(input);
    juce::MemoryOutputStream output;
    std::array<char, 8192> buffer {};
    for (;;) {
        const auto count = decompressor.read(buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0)
            break;
        if (output.getDataSize() + static_cast<std::size_t>(count) > kMaximumContextBytes)
            return std::nullopt;
        output.write(buffer.data(), static_cast<std::size_t>(count));
    }
    if (output.getDataSize() == 0)
        return std::nullopt;
    return std::string(static_cast<const char*>(output.getData()), output.getDataSize());
}

bool sanitizeText(std::string& text, std::string_view credential)
{
    const auto sanitized = memory::SensitiveDataFilter::sanitizeForContext(text, credential);
    text = sanitized.text;
    return text.size() <= kMaximumMessageBytes;
}

bool safeIdentifier(const std::string& value, std::string_view credential)
{
    const auto sanitized = memory::SensitiveDataFilter::sanitizeForContext(value, credential);
    return !value.empty() && value.size() <= 256 && !sanitized.redacted && sanitized.text == value;
}

} // namespace

ConversationContextStore::ConversationContextStore(juce::File rootDirectory)
    : rootDirectory_(std::move(rootDirectory)),
      preferences_(rootDirectory_.getChildFile("synth.md"))
{
}

juce::File ConversationContextStore::defaultRoot()
{
    return memory::SynthMemory::defaultFile().getParentDirectory();
}

bool ConversationContextStore::validPresetId(const PresetId& id)
{
    return id.size() <= 64 && !juce::Uuid(string(id)).isNull();
}

juce::File ConversationContextStore::contextFile(const PresetId& id) const
{
    return rootDirectory_.getChildFile("contexts").getChildFile(string(id) + ".json.z");
}

ContextLoadResult ConversationContextStore::load(const PresetId& id) const
{
    if (!validPresetId(id))
        return loadFailure(id, "invalid preset id");
    const auto file = contextFile(id);
    std::lock_guard<std::mutex> local(processMutex);
    juce::InterProcessLock lock(lockName(file));
    if (!lock.enter(1500))
        return loadFailure(id, "context is busy");
    struct Exit { juce::InterProcessLock& lock; ~Exit() { lock.exit(); } } exit { lock };
    return loadUnlocked(id, file);
}

ContextLoadResult ConversationContextStore::loadUnlocked(const PresetId& id, const juce::File& file) const
{
    PresetConversationContext empty;
    empty.presetId = id;
    if (!file.exists())
        return { true, false, std::move(empty), {} };
    if (!file.existsAsFile() || file.isSymbolicLink() || file.getSize() <= 0
        || file.getSize() > static_cast<juce::int64>(kMaximumContextBytes))
        return loadFailure(id, "context file is invalid");

    juce::MemoryBlock compressed;
    if (!file.loadFileAsData(compressed))
        return loadFailure(id, "context file cannot be read");
    const auto json = decompressJson(compressed);
    if (!json.has_value())
        return loadFailure(id, "context decompression failed or exceeded the limit");
    auto parsed = parseContextJson(*json);
    if (!parsed.ok() || parsed.context->presetId != id)
        return loadFailure(id, parsed.error.empty() ? "context identity mismatch" : parsed.error.c_str());
    return { true, true, std::move(*parsed.context), {} };
}

bool ConversationContextStore::sanitize(PresetConversationContext& context,
                                         std::string_view exactCredential,
                                         std::string& error)
{
    if (!validPresetId(context.presetId)) {
        error = "invalid preset id";
        return false;
    }
    auto summary = memory::SensitiveDataFilter::sanitizeForContext(context.summary, exactCredential);
    context.summary = std::move(summary.text);
    if (context.summary.size() > kMaximumSummaryBytes) {
        error = "context summary exceeds the limit";
        return false;
    }
    for (auto& turn : context.recentTurns) {
        if (!safeIdentifier(turn.turnId, exactCredential)) {
            error = "unsafe turn id";
            return false;
        }
        for (auto& message : turn.messages) {
            if (!sanitizeText(message.text, exactCredential)
                || (!message.callId.empty() && !safeIdentifier(message.callId, exactCredential))
                || (!message.toolName.empty() && !safeIdentifier(message.toolName, exactCredential))) {
                error = "unsafe or oversized message";
                return false;
            }
        }
        for (auto& transaction : turn.transactions) {
            if (!safeIdentifier(transaction.transactionId, exactCredential)
                || !sanitizeText(transaction.summary, exactCredential)) {
                error = "unsafe transaction summary";
                return false;
            }
        }
    }
    const auto validated = parseContext(serializeContext(context));
    if (!validated.ok()) {
        error = validated.error;
        return false;
    }
    return true;
}

ContextCommitResult ConversationContextStore::commit(PresetConversationContext context,
                                                       std::uint64_t expectedVersion,
                                                       std::string_view exactCredential)
{
    if (!validPresetId(context.presetId))
        return { ContextCommitStatus::error, 0, "invalid preset id" };
    const auto file = contextFile(context.presetId);
    std::lock_guard<std::mutex> local(processMutex);
    juce::InterProcessLock lock(lockName(file));
    if (!lock.enter(1500))
        return { ContextCommitStatus::error, 0, "context is busy" };
    struct Exit { juce::InterProcessLock& lock; ~Exit() { lock.exit(); } } exit { lock };

    const auto current = loadUnlocked(context.presetId, file);
    if (!current.ok)
        return { ContextCommitStatus::error, 0, current.error };
    const auto currentVersion = current.exists ? current.context.revision : 0;
    if (currentVersion != expectedVersion)
        return { ContextCommitStatus::conflict, currentVersion, "context version conflict" };
    context.revision = currentVersion + 1;
    std::string error;
    if (!sanitize(context, exactCredential, error))
        return { ContextCommitStatus::error, currentVersion, std::move(error) };
    return writeUnlocked(std::move(context), file);
}

ContextCommitResult ConversationContextStore::appendTurn(const PresetId& id,
                                                           ConversationTurn turn,
                                                           std::uint64_t expectedVersion,
                                                           std::string_view exactCredential)
{
    if (!validPresetId(id))
        return { ContextCommitStatus::error, 0, "invalid preset id" };
    const auto file = contextFile(id);
    std::lock_guard<std::mutex> local(processMutex);
    juce::InterProcessLock lock(lockName(file));
    if (!lock.enter(1500))
        return { ContextCommitStatus::error, 0, "context is busy" };
    struct Exit { juce::InterProcessLock& lock; ~Exit() { lock.exit(); } } exit { lock };

    auto current = loadUnlocked(id, file);
    if (!current.ok)
        return { ContextCommitStatus::error, 0, current.error };
    if (!current.exists && expectedVersion != 0)
        return { ContextCommitStatus::conflict, 0, "context version conflict" };
    auto& context = current.context;
    const auto duplicate = std::find_if(context.recentTurns.begin(), context.recentTurns.end(),
        [&](const auto& existing) { return existing.turnId == turn.turnId; });
    if (duplicate != context.recentTurns.end())
        return { ContextCommitStatus::committed, context.revision, {} };
    if (context.recentTurns.size() >= kMaximumRecentTurns)
        return { ContextCommitStatus::error, context.revision, "context requires compaction before append" };
    context.recentTurns.push_back(std::move(turn));
    context.revision += 1;
    std::string error;
    if (!sanitize(context, exactCredential, error))
        return { ContextCommitStatus::error, current.context.revision, std::move(error) };
    return writeUnlocked(std::move(context), file);
}

ContextCommitResult ConversationContextStore::writeUnlocked(PresetConversationContext context,
                                                              const juce::File& file) const
{
    context.updatedAtUnixMs = juce::Time::currentTimeMillis();
    const auto json = juce::JSON::toString(serializeContext(context), false);
    if (json.getNumBytesAsUTF8() > static_cast<int>(kMaximumContextBytes))
        return { ContextCommitStatus::error, context.revision - 1, "context exceeds decompressed limit" };
    juce::MemoryBlock compressed;
    if (!compressJson(json, compressed) || compressed.getSize() > kMaximumContextBytes)
        return { ContextCommitStatus::error, context.revision - 1, "context compression failed" };
    if (!file.getParentDirectory().createDirectory() || file.isSymbolicLink())
        return { ContextCommitStatus::error, context.revision - 1, "context target is unsafe" };
    juce::TemporaryFile temporary(file);
    if (!temporary.getFile().replaceWithData(compressed.getData(), compressed.getSize())
        || !temporary.overwriteTargetFileWithTemporary())
        return { ContextCommitStatus::error, context.revision - 1, "context could not be saved" };
    return { ContextCommitStatus::committed, context.revision, {} };
}

memory::MemoryResult ConversationContextStore::loadPreferences(std::string_view exactCredential) const
{
    return preferences_.read(string(exactCredential));
}

memory::MemoryResult ConversationContextStore::applyPreferenceDiff(
    const memory::ValidatedPreferenceDiff& diff, std::string_view exactCredential)
{
    return preferences_.applyPreferenceDiff(diff, exactCredential);
}

} // namespace agentic_dexed::agent::context

#include "PortablePresetContext.h"

#include "ConversationSerialization.h"
#include "../memory/SensitiveDataFilter.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace agentic_dexed::agent::context {
namespace {

constexpr std::size_t hashCharacters = 64;
constexpr std::size_t envelopeHeaderBytes =
    kPortableContextMagic.size() + sizeof(std::int32_t) * 2 + hashCharacters;

PortableContextResult failure(std::string error)
{
    return { false, std::nullopt, std::move(error) };
}

bool safeIdentifier(const std::string& value)
{
    const auto filtered = memory::SensitiveDataFilter::sanitizeForContext(value);
    return !value.empty() && value.size() <= 256
        && !filtered.redacted && filtered.text == value;
}

bool sanitize(PresetConversationContext& context)
{
    if (juce::Uuid(juce::String(context.presetId)).isNull())
        return false;
    context.summary = memory::SensitiveDataFilter::sanitizeForContext(context.summary).text;
    if (context.summary.size() > kMaximumSummaryBytes)
        return false;
    for (auto& alias : context.fingerprintAliases)
        if (!safeIdentifier(alias))
            return false;
    for (auto& turn : context.recentTurns) {
        if (!safeIdentifier(turn.turnId))
            return false;
        for (auto& message : turn.messages) {
            message.text = memory::SensitiveDataFilter::sanitizeForContext(message.text).text;
            if (message.text.size() > kMaximumMessageBytes
                || (!message.callId.empty() && !safeIdentifier(message.callId))
                || (!message.toolName.empty() && !safeIdentifier(message.toolName)))
                return false;
        }
        for (auto& transaction : turn.transactions) {
            transaction.summary = memory::SensitiveDataFilter::sanitizeForContext(
                transaction.summary).text;
            if (!safeIdentifier(transaction.transactionId)
                || transaction.summary.size() > 4096)
                return false;
        }
    }
    return parseContext(serializeContext(context)).ok();
}

bool compress(std::string_view source, juce::MemoryBlock& destination)
{
    juce::MemoryOutputStream output;
    {
        juce::GZIPCompressorOutputStream gzip(output, 6);
        if (!gzip.write(source.data(), source.size()))
            return false;
        gzip.flush();
    }
    destination = output.getMemoryBlock();
    return !destination.isEmpty() && destination.getSize() <= kMaximumContextBytes;
}

std::optional<std::string> decompress(const juce::MemoryBlock& source)
{
    juce::MemoryInputStream input(source, false);
    juce::GZIPDecompressorInputStream gzip(input);
    juce::MemoryOutputStream output;
    std::array<char, 8192> buffer {};
    for (;;) {
        const auto count = gzip.read(buffer.data(), static_cast<int>(buffer.size()));
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

} // namespace

std::string portableContextSha256(const juce::MemoryBlock& encoded)
{
    return juce::SHA256(encoded).toHexString().toStdString();
}

juce::MemoryBlock encodePortableContext(const PresetConversationContext& source)
{
    auto context = source;
    if (!sanitize(context))
        return {};
    const auto json = juce::JSON::toString(serializeContext(context), false).toStdString();
    if (json.empty() || json.size() > kMaximumContextBytes)
        return {};
    juce::MemoryBlock compressed;
    if (!compress(json, compressed))
        return {};
    const auto hash = juce::SHA256(compressed).toHexString();
    if (hash.length() != static_cast<int>(hashCharacters))
        return {};
    juce::MemoryOutputStream envelope;
    envelope.write(kPortableContextMagic.data(), kPortableContextMagic.size());
    envelope.writeInt(kPortableContextEnvelopeVersion);
    envelope.writeInt(static_cast<int>(compressed.getSize()));
    envelope.write(hash.toRawUTF8(), hashCharacters);
    envelope.write(compressed.getData(), compressed.getSize());
    return envelope.getMemoryBlock();
}

PortableContextResult decodePortableContext(const juce::MemoryBlock& encoded)
{
    if (encoded.getSize() < envelopeHeaderBytes
        || encoded.getSize() > envelopeHeaderBytes + kMaximumContextBytes)
        return failure("portable context envelope exceeds the limit");
    juce::MemoryInputStream input(encoded, false);
    std::array<char, 8> magic {};
    if (input.read(magic.data(), magic.size()) != static_cast<int>(magic.size())
        || magic != kPortableContextMagic)
        return failure("portable context magic is invalid");
    if (input.readInt() != kPortableContextEnvelopeVersion)
        return failure("portable context version is unsupported");
    const auto payloadSize = input.readInt();
    if (payloadSize <= 0 || static_cast<std::size_t>(payloadSize) > kMaximumContextBytes
        || input.getNumBytesRemaining() != hashCharacters + static_cast<std::size_t>(payloadSize))
        return failure("portable context payload size exceeds the limit");
    std::array<char, hashCharacters> expectedHash {};
    if (input.read(expectedHash.data(), expectedHash.size())
        != static_cast<int>(expectedHash.size()))
        return failure("portable context hash is missing");
    juce::MemoryBlock payload(static_cast<std::size_t>(payloadSize));
    if (input.read(payload.getData(), payload.getSize()) != payloadSize)
        return failure("portable context payload is incomplete");
    const auto actualHash = juce::SHA256(payload).toHexString().toStdString();
    if (actualHash.size() != expectedHash.size()
        || !std::equal(actualHash.begin(), actualHash.end(), expectedHash.begin()))
        return failure("portable context SHA-256 mismatch");
    const auto json = decompress(payload);
    if (!json.has_value())
        return failure("portable context decompression limit exceeded");
    auto parsed = parseContextJson(*json);
    if (!parsed.ok())
        return failure(parsed.error);
    auto context = std::move(*parsed.context);
    if (!sanitize(context))
        return failure("portable context contains unsafe data");
    return { true, std::move(context), {} };
}

} // namespace agentic_dexed::agent::context

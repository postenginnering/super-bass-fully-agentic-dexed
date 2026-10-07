#include "SensitiveDataFilter.h"

#include <JuceHeader.h>
#include <algorithm>
#include <regex>
#include <vector>

namespace agentic_dexed::agent::memory {
namespace {

constexpr std::string_view redactionMarker = u8"[已隐藏敏感信息]";

struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;
};

bool validUtf8Text(std::string_view input)
{
    if (input.find('\0') != std::string_view::npos || input.size() > 2u * 1024u * 1024u)
        return false;
    if (!juce::CharPointer_UTF8::isValidString(input.data(), static_cast<int>(input.size())))
        return false;

    const auto decoded = juce::String::fromUTF8(input.data(), static_cast<int>(input.size()));
    for (auto cursor = decoded.getCharPointer(); !cursor.isEmpty(); ++cursor) {
        const auto character = static_cast<std::uint32_t>(*cursor);
        if (character == 0xfffd || (character < 0x20 && character != '\n' && character != '\r' && character != '\t'))
            return false;
    }
    return true;
}

void addLiteralRanges(std::string_view input, std::string_view needle, std::vector<Range>& ranges)
{
    if (needle.empty())
        return;
    std::size_t position = 0;
    while ((position = input.find(needle, position)) != std::string_view::npos) {
        ranges.push_back({ position, position + needle.size() });
        position += needle.size();
    }
}

void addRegexRanges(const std::string& input, const std::regex& pattern, std::vector<Range>& ranges)
{
    for (auto match = std::sregex_iterator(input.begin(), input.end(), pattern);
         match != std::sregex_iterator(); ++match) {
        const auto begin = static_cast<std::size_t>(match->position());
        ranges.push_back({ begin, begin + static_cast<std::size_t>(match->length()) });
    }
}

bool containsCredentialAssignment(const std::string& input)
{
    static const std::regex assignment(
        R"((authorization|api[ _-]?key|apikey|password)[ \t]*[:=][ \t]*[^\r\n]+)",
        std::regex::icase | std::regex::optimize);
    return std::regex_search(input, assignment);
}

bool containsPrivateKey(const std::string& input)
{
    static const std::regex beginPrivate(
        R"(-----BEGIN[ A-Z0-9_-]*PRIVATE KEY-----)",
        std::regex::icase | std::regex::optimize);
    return std::regex_search(input, beginPrivate);
}

std::vector<Range> sensitiveRanges(const std::string& input, std::string_view exactCredential)
{
    std::vector<Range> ranges;
    addLiteralRanges(input, exactCredential, ranges);

    static const std::vector<std::regex> patterns {
        std::regex(R"(sk-[A-Za-z0-9_-]{12,})", std::regex::optimize),
        std::regex(R"((gh[pousr]_[A-Za-z0-9_-]{20,}|github_pat_[A-Za-z0-9_-]{20,}))", std::regex::optimize),
        std::regex(R"(AIza[A-Za-z0-9_-]{20,})", std::regex::optimize),
        std::regex(R"(AKIA[0-9A-Z]{16})", std::regex::optimize),
        std::regex(R"(xox[baprs]-[A-Za-z0-9-]{20,})", std::regex::icase | std::regex::optimize),
        std::regex(R"(bearer[ \t]+[A-Za-z0-9._~+/=-]{12,})", std::regex::icase | std::regex::optimize),
        std::regex(R"([A-Za-z][A-Za-z0-9+.-]*://[^:/\s]+:[^@\s]+@[^\s]+)", std::regex::optimize),
        std::regex(R"([A-Za-z]:\\[^\r\n\t\"]+)", std::regex::optimize),
        std::regex(R"(/(Users|home)/[^\r\n\t\"]+)", std::regex::optimize),
        std::regex(R"(100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.[0-9]{1,3}\.[0-9]{1,3})", std::regex::optimize),
        std::regex(R"([A-Za-z0-9.-]+\.ts\.net)", std::regex::icase | std::regex::optimize),
    };
    for (const auto& pattern : patterns)
        addRegexRanges(input, pattern, ranges);
    return ranges;
}

std::string replaceRanges(const std::string& input, std::vector<Range> ranges)
{
    if (ranges.empty())
        return input;
    std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.begin < right.begin || (left.begin == right.begin && left.end < right.end);
    });

    std::vector<Range> merged;
    for (const auto& range : ranges) {
        if (merged.empty() || range.begin > merged.back().end)
            merged.push_back(range);
        else
            merged.back().end = std::max(merged.back().end, range.end);
    }

    std::string result;
    std::size_t position = 0;
    for (const auto& range : merged) {
        result.append(input, position, range.begin - position);
        result.append(redactionMarker);
        position = range.end;
    }
    result.append(input, position, input.size() - position);
    return result;
}

} // namespace

SanitizedText SensitiveDataFilter::sanitizeForContext(
    std::string_view input, std::string_view exactCredential)
{
    if (!validUtf8Text(input))
        return { std::string(redactionMarker), true, "invalid or unsafe text was redacted" };

    const std::string owned(input);
    if (containsCredentialAssignment(owned) || containsPrivateKey(owned))
        return { std::string(redactionMarker), true, "credential-like text was redacted" };

    auto ranges = sensitiveRanges(owned, exactCredential);
    if (ranges.empty())
        return { owned, false, {} };
    return { replaceRanges(owned, std::move(ranges)), true, "sensitive text was redacted" };
}

bool SensitiveDataFilter::acceptPreference(
    std::string_view input, std::string_view exactCredential)
{
    if (input.empty() || input.size() > 512 || input.find('\n') != std::string_view::npos
        || input.find('\r') != std::string_view::npos)
        return false;
    const auto result = sanitizeForContext(input, exactCredential);
    return !result.redacted && result.text == input;
}

} // namespace agentic_dexed::agent::memory

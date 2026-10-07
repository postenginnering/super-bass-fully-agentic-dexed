#include <JuceHeader.h>
#include "agent/memory/SensitiveDataFilter.h"

#include <random>

namespace {
using agentic_dexed::agent::memory::SensitiveDataFilter;

std::string utf16Le(std::string_view source)
{
    std::string result;
    for (const auto character : source) {
        result.push_back(character);
        result.push_back('\0');
    }
    return result;
}

std::string utf16Be(std::string_view source)
{
    std::string result;
    for (const auto character : source) {
        result.push_back('\0');
        result.push_back(character);
    }
    return result;
}

class SensitiveDataFilterTests final : public juce::UnitTest {
public:
    SensitiveDataFilterTests() : UnitTest("Sensitive context data", "Agent") {}

    void runTest() override
    {
        constexpr std::string_view credential = "sk-DO-NOT-LEAK-0123456789";
        const std::string marker = juce::String::fromUTF8("[已隐藏敏感信息]").toStdString();

        beginTest("Exact credentials and their UTF-16 byte forms are redacted");
        for (const auto& sample : { std::string("before ") + std::string(credential) + " after",
                 utf16Le(credential), utf16Be(credential) }) {
            const auto result = SensitiveDataFilter::sanitizeForContext(sample, credential);
            expect(result.redacted);
            expect(result.text.find(credential) == std::string::npos);
            expect(result.text.find(marker) != std::string::npos);
            expect(result.diagnostic.find(credential) == std::string::npos);
        }

        beginTest("Common provider tokens, headers, and private keys are redacted case-insensitively");
        const std::vector<std::string> secrets {
            "sk-abcdefghijklmnopqrstuvwxyz012345",
            "ghp_abcdefghijklmnopqrstuvwxyz0123456789",
            "github_pat_11AA_DO-NOT-LEAK_abcdefghijklmnopqrstuvwxyz",
            "AIzaSyDO-NOT-LEAK-012345678901234567890",
            "AKIAIOSFODNN7EXAMPLE",
            std::string("xox") + "b-123456789012-123456789012-abcdefghijklmnop",
            "bEaReR abcdefghijklmnopqrstuvwxyz.0123456789",
            "-----BEGIN PRIVATE KEY-----\nDO-NOT-LEAK\n-----END PRIVATE KEY-----",
        };
        for (const auto& secret : secrets) {
            const auto result = SensitiveDataFilter::sanitizeForContext("prefix " + secret + " suffix", {});
            expect(result.redacted, secret);
            expect(result.text.find(secret) == std::string::npos, secret);
            expect(!SensitiveDataFilter::acceptPreference(secret, {}), secret);
        }

        beginTest("Password URLs, private paths, and Tailscale addresses are redacted");
        const std::vector<std::string> privateData {
            "https://alice:password@example.com/v1",
            "C:\\Users\\alice\\AppData\\Roaming\\synth.md",
            "F:\\workspace\\agentic-dexed\\preset.dexedpreset",
            "/Users/alice/Library/Application Support/synth.md",
            "/home/alice/.config/synth.md",
            "100.100.22.3",
            "macbook.tail123.ts.net",
        };
        for (const auto& value : privateData) {
            const auto result = SensitiveDataFilter::sanitizeForContext("value=" + value, {});
            expect(result.redacted, value);
            expect(result.text.find(value) == std::string::npos, value);
            expect(!SensitiveDataFilter::acceptPreference(value, {}), value);
        }

        beginTest("NUL, controls, invalid UTF-8, and credential field names never persist");
        const std::vector<std::string> malformed {
            std::string("abc\0def", 7),
            std::string("abc\x01" "def", 7),
            std::string("abc\xff" "def", 7),
            "API_KEY=DO-NOT-LEAK",
            "Password: DO-NOT-LEAK",
            "Authorization: basic DO-NOT-LEAK",
        };
        for (const auto& value : malformed) {
            const auto result = SensitiveDataFilter::sanitizeForContext(value, {});
            expect(result.redacted);
            expect(result.text == marker);
            expect(!SensitiveDataFilter::acceptPreference(value, {}));
        }

        beginTest("Normal Chinese sound language and parameter IDs remain unchanged");
        const auto safe = juce::String::fromUTF8("我喜欢温暖的 pad，global.output 设为 0.45，operator.1.eg.rate.4 不要太慢").toStdString();
        const auto safeResult = SensitiveDataFilter::sanitizeForContext(safe, credential);
        expect(!safeResult.redacted);
        expectEquals(safeResult.text, safe);
        expect(SensitiveDataFilter::acceptPreference(safe, credential));

        beginTest("Random byte boundaries never crash or expose an injected credential");
        std::mt19937 random(0x5bfad);
        std::uniform_int_distribution<int> byte(1, 255);
        for (int iteration = 0; iteration < 500; ++iteration) {
            std::string input(static_cast<std::size_t>(iteration % 97), 'x');
            for (auto& character : input)
                character = static_cast<char>(byte(random));
            if ((iteration % 7) == 0)
                input += credential;
            const auto result = SensitiveDataFilter::sanitizeForContext(input, credential);
            expect(result.text.find(credential) == std::string::npos);
        }
    }
} sensitiveDataFilterTests;
}

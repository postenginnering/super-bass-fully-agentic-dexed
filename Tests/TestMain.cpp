#include <JuceHeader.h>
#include "TestDataPaths.h"

#include <iostream>
#include <chrono>

#if JUCE_MAC
namespace juce
{
void initialiseNSApplication();
}
#endif

namespace
{
class ConsoleTestRunner final : public juce::UnitTestRunner
{
protected:
    void logMessage(const juce::String& message) override
    {
        std::cout << message << '\n';
    }
};

bool matchesFilter(const juce::UnitTest& test, const juce::String& filter)
{
    // Paid network tests run only when explicitly selected, never in ordinary CTest.
    if (test.getCategory().startsWith("Live") && filter != test.getCategory())
        return false;
    if (agentic_dexed::test::portableData && test.getCategory() == "SourceAudit")
        return false;
    return filter.isEmpty()
        || test.getName().containsIgnoreCase(filter)
        || test.getCategory().containsIgnoreCase(filter);
}
} // namespace

int main(int argc, char** argv)
{
    const auto testStarted = std::chrono::steady_clock::now();
   #if JUCE_MAC
    juce::initialiseNSApplication();
   #endif
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::String filter;
    for (int index = 1; index < argc; ++index)
    {
        const juce::String argument(argv[index]);
        if (argument == "--portable")
        {
            agentic_dexed::test::portableData = true;
            continue;
        }
        if (argument != "--filter" || index + 1 >= argc)
        {
            std::cerr << "usage: AgenticDexedTests [--portable] [--filter substring]\n";
            return 2;
        }

        filter = juce::String::fromUTF8(argv[++index]).trim();
        if (filter.isEmpty())
        {
            std::cerr << "--filter requires a non-empty substring\n";
            return 2;
        }
    }

    if (agentic_dexed::test::portableData)
    {
        const auto root = agentic_dexed::test::dataRoot();
        for (const auto* required : {
                 "Tests/fixtures/upstream-init-state.bin",
                 "Tests/fixtures/responses/success.sse",
                 "Tests/fixtures/chat-completions/success.sse",
                 "Tests/golden/macos/workbench-manifest.json",
                 "libs/tuning-library/tests/data/12-ET-P5.scl" })
            if (!root.getChildFile(required).existsAsFile())
            {
                std::cerr << "Missing packaged test data: " << required << '\n';
                return 2;
            }
        std::cout << "Portable runtime suite; source-only audit runs separately on the build checkout.\n";
    }

    juce::Array<juce::UnitTest*> selected;
    for (auto* test : juce::UnitTest::getAllTests())
        if (test != nullptr && matchesFilter(*test, filter))
            selected.add(test);

    if (selected.isEmpty())
    {
        std::cerr << "No tests matched filter '" << filter << "'\n";
        return 2;
    }

    ConsoleTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runTests(selected);

    int passes = 0;
    int failures = 0;
    for (int index = 0; index < runner.getNumResults(); ++index)
    {
        if (const auto* result = runner.getResult(index))
        {
            passes += result->passes;
            failures += result->failures;
        }
    }

    std::cout << "SUMMARY: " << passes << " passed, " << failures << " failed\n";
    std::cout << "ELAPSED_MS: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - testStarted).count()
              << '\n';
    return failures == 0 ? 0 : 1;
}

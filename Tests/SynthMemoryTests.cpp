#include <JuceHeader.h>
#include "agent/memory/SynthMemory.h"
#include <thread>

namespace {
using agentic_dexed::agent::memory::SynthMemory;
using agentic_dexed::agent::memory::PreferenceOperation;
using agentic_dexed::agent::memory::ValidatedPreferenceDiff;
class SynthMemoryTests final : public juce::UnitTest {
public:
    SynthMemoryTests() : UnitTest("Local synth memory", "Agent") {}
    void runTest() override {
        const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile("sbfad-memory-test", "", false);
        const auto file = folder.getChildFile("synth.md");
        SynthMemory memory(file);
        beginTest("Missing memory is empty and does not write on read");
        expect(memory.read().text.isEmpty());
        expect(!file.exists());
        beginTest("Chinese preferences persist across instances and replace by key");
        expect(memory.update("remember", "pad", juce::String::fromUTF8("喜欢温暖的 pad"), juce::String::fromUTF8("喜欢温暖的 pad"), juce::String::fromUTF8("我喜欢温暖的 pad"), "").ok);
        expect(SynthMemory(file).read().text.contains(juce::String::fromUTF8("喜欢温暖的 pad")));
        expect(memory.update("remember", "pad", juce::String::fromUTF8("喜欢明亮的 pad"), juce::String::fromUTF8("喜欢明亮的 pad"), juce::String::fromUTF8("现在喜欢明亮的 pad"), "").ok);
        expect(!memory.read().text.contains(juce::String::fromUTF8("温暖")));
        beginTest("Repeated same preference does not grow the file");
        expect(memory.update("remember", "pad", "warm pad", "warm pad", "warm pad", "").ok);
        const auto stableSize = file.getSize();
        for (int i = 0; i < 10; ++i)
            expect(memory.update("remember", "pad", "warm pad", "warm pad", "warm pad", "").ok);
        expectEquals(file.getSize(), stableSize);
        beginTest("Unproven evidence and credentials never reach disk");
        const auto before = file.loadFileAsString();
        expect(!memory.update("remember", "bass", "warm", "I prefer warm", "Make a bass", "").ok);
        expect(!memory.update("remember", "secret", "sk-do-not-store", "sk-do-not-store", "sk-do-not-store", "").ok);
        expect(!memory.update("remember", "secret", "private-value", "private-value", "private-value", "private-value").ok);
        expectEquals(file.loadFileAsString(), before);
        beginTest("Manual prose survives edits; separate writers merge");
        expect(file.appendText("\nMy manual notes.\n"));
        SynthMemory other(file);
        std::thread a([&] { memory.update("remember", "bass", "soft bass", "soft bass", "soft bass", ""); });
        std::thread b([&] { other.update("remember", "lead", "bright lead", "bright lead", "bright lead", ""); });
        a.join(); b.join();
        expect(memory.read().text.contains("soft bass"));
        expect(memory.read().text.contains("bright lead"));
        expect(memory.read().text.contains("My manual notes."));
        beginTest("Validated automatic observations promote only across distinct turns");
        const ValidatedPreferenceDiff firstObservation {
            PreferenceOperation::observe, "space", juce::String::fromUTF8("喜欢宽阔的空间感").toStdString(),
            juce::String::fromUTF8("宽阔的空间感").toStdString(), "turn-one", false
        };
        expect(memory.applyPreferenceDiff(firstObservation).ok);
        expect(!memory.read().text.contains(juce::String::fromUTF8("宽阔的空间感")));
        expect(memory.applyPreferenceDiff(firstObservation).ok);
        expect(!memory.read().text.contains(juce::String::fromUTF8("宽阔的空间感")));
        auto secondObservation = firstObservation;
        secondObservation.turnId = "turn-two";
        expect(memory.applyPreferenceDiff(secondObservation).ok);
        expect(memory.read().text.contains(juce::String::fromUTF8("宽阔的空间感")));
        expect(file.getSiblingFile("memory-state.json").existsAsFile());
        beginTest("Automatic preference diffs reject credentials and private paths");
        const auto preferencesBeforeSensitive = file.loadFileAsString();
        expect(!memory.applyPreferenceDiff({ PreferenceOperation::add, "secret", "sk-DO-NOT-LEAK-12345678", "exact", "turn-3", true }).ok);
        expect(!memory.applyPreferenceDiff({ PreferenceOperation::add, "path", "C:\\Users\\alice\\private", "exact", "turn-4", true }).ok);
        expectEquals(file.loadFileAsString(), preferencesBeforeSensitive);
        beginTest("Forget and clear persist; no patch state is involved");
        expect(memory.update("forget", "pad", "", "forget pad", "forget pad", "").ok);
        expect(!memory.read().text.contains("[pad]"));
        expect(memory.update("clear", "", "", "forget all", "forget all", "").ok);
        expect(!memory.read().text.contains("soft bass"));
        beginTest("Oversized and credential-containing manual files are not model context");
        expect(file.replaceWithText(juce::String::repeatedString("x", 9000)));
        expect(!memory.read().ok);
        beginTest("Invalid UTF-8 and embedded NUL are rejected without rewriting");
        const char invalid[] = { static_cast<char>(0xff), 'x' };
        expect(file.replaceWithData(invalid, sizeof(invalid)));
        expect(!memory.read().ok);
        const char nul[] = { 'a', '\0', 'b' };
        expect(file.replaceWithData(nul, sizeof(nul)));
        expect(!memory.read().ok);
        expect(!memory.update("clear", "", "", "forget all", "forget all", "").ok);
        expect(file.replaceWithText("sk-secret-user-value"));
        expect(!memory.read().ok);
        beginTest("Unwritable target returns an error without destroying existing content");
        expect(!SynthMemory(file.getChildFile("synth.md")).update("remember", "pad", "warm", "warm", "warm", "").ok);
        expect(folder.deleteRecursively());
    }
} synthMemoryTests;
}

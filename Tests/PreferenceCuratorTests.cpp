#include <JuceHeader.h>

#include "agent/memory/PreferenceCurator.h"
#include "agent/memory/SynthMemory.h"

namespace {
using namespace agentic_dexed::agent::context;
using namespace agentic_dexed::agent::memory;

ConversationTurn userTurn(std::string id, std::string user)
{
    ConversationTurn result;
    result.turnId = std::move(id);
    result.messages = {
        { ConversationRole::user, std::move(user) },
        { ConversationRole::assistant, u8"已完成" }
    };
    return result;
}

class PreferenceCuratorTests final : public juce::UnitTest {
public:
    PreferenceCuratorTests()
        : juce::UnitTest("Automatic preference curator", "AgentMemory") {}

    void runTest() override
    {
        beginTest("Explicit lasting preferences are accepted without a remember command");
        const auto lasting = userTurn("lasting-1", u8"我一般偏好温暖、柔和的 pad");
        const auto added = PreferenceCurator::parseResponse(
            R"({"operation":"add","key":"pad_tone","preference":"温暖、柔和的 pad","evidence":"我一般偏好温暖、柔和的 pad"})",
            lasting);
        expect(added.ok);
        expect(added.diff.has_value());
        expect(added.diff->operation == PreferenceOperation::add);
        expect(added.diff->explicitDurable);

        beginTest("A one-off sound request is only observed and needs two distinct turns");
        const auto oneOff = userTurn("observe-1", u8"这次做一个空灵的 pad");
        const auto observed = PreferenceCurator::parseResponse(
            R"({"operation":"add","key":"pad_space","preference":"空灵的 pad","evidence":"这次做一个空灵的 pad"})",
            oneOff);
        expect(observed.ok && observed.diff.has_value());
        expect(observed.diff->operation == PreferenceOperation::observe);
        expect(!observed.diff->explicitDurable);

        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sbfad-curator-" + juce::Uuid().toString());
        root.createDirectory();
        SynthMemory memory(root.getChildFile("synth.md"));
        expect(memory.applyPreferenceDiff(*observed.diff).ok);
        expect(!memory.read().text.contains(u8"空灵的 pad"));
        auto second = *observed.diff;
        second.turnId = "observe-2";
        expect(memory.applyPreferenceDiff(second).ok);
        expect(memory.read().text.contains(u8"空灵的 pad"));
        root.deleteRecursively();

        beginTest("Contradictions can replace an existing sound preference");
        const auto correction = userTurn("replace-1", u8"我以后不喜欢明亮的 pad，偏好暗一点");
        const auto replaced = PreferenceCurator::parseResponse(
            R"({"operation":"replace","key":"pad_brightness","preference":"pad 偏暗，不要明亮","evidence":"我以后不喜欢明亮的 pad，偏好暗一点"})",
            correction);
        expect(replaced.ok && replaced.diff.has_value());
        expect(replaced.diff->operation == PreferenceOperation::replace);

        const auto removal = userTurn("remove-1", u8"以后不要保留我的混响偏好");
        const auto removed = PreferenceCurator::parseResponse(
            R"({"operation":"remove","key":"reverb","preference":"","evidence":"以后不要保留我的混响偏好"})",
            removal);
        expect(removed.ok && removed.diff.has_value());
        expect(removed.diff->operation == PreferenceOperation::remove);

        beginTest("Only the strict operation vocabulary and exact current evidence are accepted");
        expect(!PreferenceCurator::parseResponse(
            R"({"operation":"remember","key":"pad_tone","preference":"温暖","evidence":"我一般偏好温暖、柔和的 pad"})",
            lasting).ok);
        expect(!PreferenceCurator::parseResponse(
            R"({"operation":"add","key":"pad_tone","preference":"温暖","evidence":"并不存在的原话"})",
            lasting).ok);
        expect(!PreferenceCurator::parseResponse(
            R"({"operation":"add","key":"birthday","preference":"用户生日是五月","evidence":"我一般偏好温暖、柔和的 pad"})",
            lasting).ok);

        beginTest("Curator requests contain data-only messages and no tool contract");
        const auto request = PreferenceCurator::buildRequest(lasting, u8"- [pad_tone] 柔和");
        expectEquals(static_cast<int>(request.size()), 2);
        expectEquals(request.front().role, std::string("system"));
        expect(request.front().text.find("JSON") != std::string::npos);
        expect(request.back().text.find(u8"我一般偏好温暖") != std::string::npos);
    }
};

PreferenceCuratorTests preferenceCuratorTests;
}

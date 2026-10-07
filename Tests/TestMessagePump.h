#pragma once

#include <JuceHeader.h>

#include <chrono>
#include <thread>

namespace agentic_dexed::test
{
inline void pumpMessagesFor(int milliseconds)
{
    if (auto* manager = juce::MessageManager::getInstance())
        manager->runDispatchLoopUntil(juce::jmax(1, milliseconds));
}

template <typename Predicate>
bool pumpMessagesUntil(Predicate&& predicate,
                       std::chrono::milliseconds timeout,
                       int sliceMilliseconds = 10)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
    {
        pumpMessagesFor(sliceMilliseconds);
        std::this_thread::yield();
    }
    return predicate();
}
}

// Standalone unit test for the Vulkan validation gate
// (UHE/src/Platform/Vulkan/ValidationGate.*).
//
// The gate decides three things: whether a severity is an error, whether an
// error stops the process, and which of a flood of identical messages is worth a
// line. That policy is what stands between "34 invalid frames a second" and a
// red build, and it is pure logic, so it is exercised here with no GPU, no
// instance and no display. The end-to-end half of the gate -- a real layer
// reporting a real VUID into a real engine run -- is script/check_validation.sh.
//
// What this test cannot check is that the severity bits still match the layer's:
// that is a static_assert in VulkanInstance.cpp, where the Vulkan header is in
// scope.
#include "Platform/Vulkan/ValidationGate.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace UHE::RHI::VULKAN::Validation;

static int g_failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (cond)                                                                                  \
            std::cout << "  ok  - " << msg << '\n';                                                \
        else                                                                                       \
        {                                                                                          \
            std::cout << "  FAIL - " << msg << '\n';                                               \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static void CheckClassify()
{
    std::cout << "\nClassify\n";
    CHECK(Classify(Error) == Severity::Error, "an error is an error");
    CHECK(Classify(Error | Warning) == Severity::Error, "error outranks warning in a combined mask");
    CHECK(Classify(Warning) == Severity::Warning, "a warning is a warning");
    CHECK(Classify(Warning | Info) == Severity::Warning, "warning outranks info");
    CHECK(Classify(Info) == Severity::Info, "info is info");
    CHECK(Classify(Verbose) == Severity::Verbose, "verbose is verbose");
    CHECK(Classify(0) == Severity::Verbose, "an empty mask is not an error");
    // The layer may add a bit this build does not know. Stopping on it would
    // turn a layer quirk into a failed build.
    CHECK(Classify(0x8000u) == Severity::Verbose, "an unknown severity bit is not an error");
    CHECK(Classify(0x8000u | Warning) == Severity::Warning, "an unknown bit does not hide a warning");
}

static void CheckParseEnabledFlag()
{
    std::cout << "\nParseEnabledFlag\n";
    CHECK(!ParseEnabledFlag(nullptr), "unset means off");
    CHECK(!ParseEnabledFlag(""), "empty means off");
    for (const char* off : {"0", "false", "FALSE", "False", "off", "OFF", "no", "No", " 0 ", "\tfalse\n"})
    {
        CHECK(!ParseEnabledFlag(off), std::string("off: '") + off + "'");
    }
    // Anything that is not a recognised off is on, so a typo fails loudly by
    // stopping the run rather than quietly by not stopping it.
    for (const char* on : {"1", "true", "TRUE", "on", "yes", "2", "fatal", " offx "})
    {
        CHECK(ParseEnabledFlag(on), std::string("on: '") + on + "'");
    }
}

static void CheckMessageKey()
{
    std::cout << "\nMessageKey\n";
    CHECK(MessageKey(nullptr, 0).empty(), "no id name and no number has no identity");
    CHECK(MessageKey("", 0).empty(), "an empty id name and no number has no identity");
    CHECK(!MessageKey("VUID-VkRenderingInfo-pNext-06080", 0).empty(), "an id name alone identifies");
    CHECK(!MessageKey(nullptr, 7).empty(), "a number alone identifies");
    CHECK(MessageKey("VUID-x", 7) == MessageKey("VUID-x", 7), "the same id and number match");
    CHECK(MessageKey("VUID-x", 7) != MessageKey("VUID-x", 8), "a different number does not match");
    CHECK(MessageKey("VUID-x", 7) != MessageKey("VUID-y", 7), "a different name does not match");
}

static void CheckRepeatFilter()
{
    std::cout << "\nRepeatFilter\n";
    {
        RepeatFilter filter;
        // The whole point: the layer repeats a defect every frame, and only the
        // first occurrence carries information. 2, 4, 8 ... are the checkpoints.
        const std::array<RepeatVerdict, 16> expected = {
            RepeatVerdict::First,      RepeatVerdict::Report,     RepeatVerdict::Suppressed,
            RepeatVerdict::Report,     RepeatVerdict::Suppressed, RepeatVerdict::Suppressed,
            RepeatVerdict::Suppressed, RepeatVerdict::Report,     RepeatVerdict::Suppressed,
            RepeatVerdict::Suppressed, RepeatVerdict::Suppressed, RepeatVerdict::Suppressed,
            RepeatVerdict::Suppressed, RepeatVerdict::Suppressed, RepeatVerdict::Suppressed,
            RepeatVerdict::Report};
        bool sequenceOk = true;
        unsigned lines = 0;
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            const RepeatVerdict verdict = filter.Observe("VUID-x#7");
            sequenceOk = sequenceOk && verdict == expected[i];
            if (verdict != RepeatVerdict::Suppressed)
            {
                ++lines;
            }
        }
        CHECK(sequenceOk, "sixteen reports of one message give first, 2, 4, 8, 16");
        CHECK(lines == 5, "sixteen reports collapse to five lines");
        CHECK(filter.Count("VUID-x#7") == 16, "the repeat count is the number of reports");
        CHECK(filter.Size() == 1, "one distinct message is tracked");
    }
    {
        RepeatFilter filter;
        CHECK(filter.Observe("") == RepeatVerdict::First, "a message with no identity is logged");
        CHECK(filter.Observe("") == RepeatVerdict::First, "an unidentifiable message is never collapsed");
        CHECK(filter.Size() == 0, "an unidentifiable message is not tracked");
        CHECK(filter.Count("") == 0, "an unidentifiable message has no count");
    }
    {
        RepeatFilter filter;
        filter.Observe("a#1");
        filter.Observe("a#1");
        filter.Observe("b#2");
        CHECK(filter.Count("a#1") == 2, "each message keeps its own count");
        CHECK(filter.Count("b#2") == 1, "a second message starts at one");
        CHECK(filter.Size() == 2, "two distinct messages are tracked");
        CHECK(filter.Count("never#0") == 0, "an unseen message has no count");
    }
    {
        // The bound is the reason the class exists: a per-frame error for ten
        // minutes must not produce ten thousand lines.
        RepeatFilter filter;
        unsigned lines = 0;
        for (int i = 0; i < 4096; ++i)
        {
            if (filter.Observe("VUID-flood#1") != RepeatVerdict::Suppressed)
            {
                ++lines;
            }
        }
        CHECK(lines == 13, "4096 reports collapse to 13 lines");
        CHECK(filter.Count("VUID-flood#1") == 4096, "all 4096 reports are counted");
    }
    {
        // The layer reports from more than one thread; the counts have to be
        // exact anyway, or the collapse bound is a lie.
        RepeatFilter filter;
        std::vector<std::thread> threads;
        constexpr int kThreads = 4;
        constexpr int kPerThread = 512;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back(
                [&filter, t]
                {
                    const std::string key = "thread#" + std::to_string(t);
                    for (int i = 0; i < kPerThread; ++i)
                    {
                        filter.Observe(key);
                    }
                });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        unsigned total = 0;
        for (int t = 0; t < kThreads; ++t)
        {
            total += filter.Count("thread#" + std::to_string(t));
        }
        CHECK(total == static_cast<unsigned>(kThreads * kPerThread),
              "concurrent reports are all counted");
        CHECK(filter.Size() == kThreads, "one entry per thread's message");
    }
}

int main(int argc, char** argv)
{
    bool environmentOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg(argv[i]);
        // The environment read is cached on first use, so a process can only
        // ever assert one value; CTest gives it a process per value.
        if (arg == "--expect-fatal=on")
        {
            environmentOnly = true;
            CHECK(FatalValidationEnabled(), "UHE_VALIDATION_FATAL=1 asks for a fatal gate");
        }
        else if (arg == "--expect-fatal=off")
        {
            environmentOnly = true;
            CHECK(!FatalValidationEnabled(), "UHE_VALIDATION_FATAL=0 turns the gate off");
        }
        else
        {
            std::cout << "usage: validation_test [--expect-fatal=on|off]\n";
            return 2;
        }
    }

    if (!environmentOnly)
    {
        CheckClassify();
        CheckParseEnabledFlag();
        CheckMessageKey();
        CheckRepeatFilter();
    }

    std::cout << (g_failures == 0 ? "\nALL CHECKS PASSED\n" : "\nFAILURES PRESENT\n");
    return g_failures == 0 ? 0 : 1;
}

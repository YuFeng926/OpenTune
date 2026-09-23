// Tests/PitchParameterContractTests.cpp
//
// 真实 include 生产 Note/PitchControlConfig/PitchUtils/AudioEditingScheme 的
// 契约测试（纯标准库）：字段/常量漂移即编译失败，行为契约用 check 断言。
// 不使用 assert：Release 构建带 /DNDEBUG 时 assert 会被编译掉。

#include "Utils/Note.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/PitchUtils.h"
#include "Utils/AudioEditingScheme.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

// Note 默认值与 PitchControlConfig 常量一致；常量关系锁定。
static void testNoteDefaultsMatchConfigConstants()
{
    const OpenTune::Note note;
    check(note.retuneSpeed == OpenTune::PitchControlConfig::kDefaultRetuneSpeedNormalized,
          "Note default retuneSpeed equals kDefaultRetuneSpeedNormalized");
    check(note.vibratoDepth == OpenTune::PitchControlConfig::kDefaultVibratoDepth,
          "Note default vibratoDepth equals kDefaultVibratoDepth");
    check(note.vibratoRate == OpenTune::PitchControlConfig::kDefaultVibratoRateHz,
          "Note default vibratoRate equals kDefaultVibratoRateHz");

    check(OpenTune::PitchControlConfig::kDefaultRetuneSpeedNormalized
              == OpenTune::PitchControlConfig::kDefaultRetuneSpeedPercent / 100.0f,
          "kDefaultRetuneSpeedNormalized equals percent / 100");

    check(OpenTune::PitchControlConfig::kMinNoteSplitCents
              < OpenTune::PitchControlConfig::kDefaultNoteSplitCents
          && OpenTune::PitchControlConfig::kDefaultNoteSplitCents
              < OpenTune::PitchControlConfig::kMaxNoteSplitCents,
          "NoteSplit cents constants stay ordered min < default < max");
}

// Note 拷贝保留参数字段。
static void testNoteCopyPreservesParams()
{
    OpenTune::Note original;
    original.retuneSpeed = 0.8f;
    original.vibratoDepth = 5.0f;
    original.vibratoRate = 12.0f;

    const OpenTune::Note copy = original;
    check(copy.retuneSpeed == 0.8f, "Note copy preserves retuneSpeed");
    check(copy.vibratoDepth == 5.0f, "Note copy preserves vibratoDepth");
    check(copy.vibratoRate == 12.0f, "Note copy preserves vibratoRate");
}

// OpenTune::PitchUtils::mixRetune 行为契约。
static void testMixRetune()
{
    const float source = 450.0f;
    const float target = 440.0f;

    const float out0 = OpenTune::PitchUtils::mixRetune(source, target, 0.0f);
    check(std::abs(out0 - source) < 0.1f, "mixRetune retune=0 returns source");

    const float out1 = OpenTune::PitchUtils::mixRetune(source, target, 1.0f);
    check(std::abs(out1 - target) < 0.1f, "mixRetune retune=1 returns target");

    const float out05 = OpenTune::PitchUtils::mixRetune(source, target, 0.5f);
    check(out05 > std::min(source, target) && out05 < std::max(source, target),
          "mixRetune retune=0.5 lies strictly between source and target");

    const float nonPositiveShifted = OpenTune::PitchUtils::mixRetune(0.0f, target, 0.5f);
    check(nonPositiveShifted == target, "mixRetune shiftedF0<=0 returns target");

    const float nonPositiveTarget = OpenTune::PitchUtils::mixRetune(source, 0.0f, 0.5f);
    check(nonPositiveTarget == source, "mixRetune targetF0<=0 returns source");
}

// OpenTune::AudioEditingScheme::resolveParameterTarget 优先级契约。
static void testResolveParameterTarget()
{
    using namespace OpenTune::AudioEditingScheme;

    check(resolveParameterTarget({true, true}) == ParameterTarget::SelectedNotes,
          "hasSelectedNotes takes priority over hasFrameSelection");
    check(resolveParameterTarget({false, true}) == ParameterTarget::FrameSelection,
          "frame selection alone resolves to FrameSelection");
    check(resolveParameterTarget({false, false}) == ParameterTarget::None,
          "no selection resolves to None");
}

int main()
{
    std::printf("PitchParameterContractTests:\n");
    testNoteDefaultsMatchConfigConstants();
    testNoteCopyPreservesParams();
    testMixRetune();
    testResolveParameterTarget();
    if (failures == 0) { std::printf("All tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d test(s) FAILED.\n", failures);
    return 1;
}

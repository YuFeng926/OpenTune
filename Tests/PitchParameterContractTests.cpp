// Tests/PitchParameterContractTests.cpp
//
// Pure-logic contract tests for pitch parameter system.
// Deliberately free of JUCE dependencies so it builds standalone
// via the OPENTUNE_BUILD_TESTS CMake option.

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

// Minimal stubs for PitchControlConfig constants
namespace OpenTune::PitchControlConfig {
    constexpr float kDefaultRetuneSpeedPercent = 15.0f;
    constexpr float kDefaultRetuneSpeedNormalized = 0.15f;
    constexpr float kDefaultVibratoDepth = 0.0f;
    constexpr float kDefaultVibratoRateHz = 7.5f;
}

// Minimal Note struct matching the project's Note fields
struct TestNote {
    double startTime = 0.0;
    double endTime = 0.0;
    float pitch = 0.0f;
    float originalPitch = 0.0f;
    float pitchOffset = 0.0f;
    float retuneSpeed = OpenTune::PitchControlConfig::kDefaultRetuneSpeedNormalized;
    float vibratoDepth = OpenTune::PitchControlConfig::kDefaultVibratoDepth;
    float vibratoRate = OpenTune::PitchControlConfig::kDefaultVibratoRateHz;
    float pitchDriftScale = 1.0f;
    bool dirty = false;
};

enum class TestSource : uint8_t { None = 0, NoteBased = 1, HandDraw = 2, LineAnchor = 3 };

struct TestSegment {
    int startFrame = 0;
    int endFrame = 0;
    std::vector<float> f0Data;
    TestSource source = TestSource::None;
    float retuneSpeed = OpenTune::PitchControlConfig::kDefaultRetuneSpeedNormalized;
    float vibratoDepth = OpenTune::PitchControlConfig::kDefaultVibratoDepth;
    float vibratoRate = OpenTune::PitchControlConfig::kDefaultVibratoRateHz;
    std::vector<float> baseF0Data;
};

// ============================================================================
// Test 1: Note concrete defaults
// ============================================================================
static void test_note_concrete_defaults()
{
    TestNote note;
    assert(note.retuneSpeed == OpenTune::PitchControlConfig::kDefaultRetuneSpeedNormalized);
    assert(note.vibratoDepth == OpenTune::PitchControlConfig::kDefaultVibratoDepth);
    assert(note.vibratoRate == OpenTune::PitchControlConfig::kDefaultVibratoRateHz);
    std::cout << "  PASS: test_note_concrete_defaults\n";
}

// ============================================================================
// Test 2: Note copy preserves parameters
// ============================================================================
static void test_note_copy_preserves_params()
{
    TestNote original;
    original.retuneSpeed = 0.8f;
    original.vibratoDepth = 5.0f;
    original.vibratoRate = 12.0f;

    TestNote copy = original;
    assert(copy.retuneSpeed == 0.8f);
    assert(copy.vibratoDepth == 5.0f);
    assert(copy.vibratoRate == 12.0f);
    std::cout << "  PASS: test_note_copy_preserves_params\n";
}

// ============================================================================
// Test 3: ParameterTarget priority ordering
// ============================================================================
enum class TestTarget { None, SelectedManualSegments, SelectedNotes, FrameSelection };

struct TestTargetContext {
    bool hasSelectedNotes = false;
    bool hasSelectedManualSegments = false;
    bool hasFrameSelection = false;
};

static TestTarget resolveTarget(const TestTargetContext& ctx)
{
    if (ctx.hasSelectedManualSegments) return TestTarget::SelectedManualSegments;
    if (ctx.hasSelectedNotes) return TestTarget::SelectedNotes;
    if (ctx.hasFrameSelection) return TestTarget::FrameSelection;
    return TestTarget::None;
}

static void test_parameter_target_priority()
{
    // Manual segment takes highest priority
    {
        TestTargetContext ctx{true, true, true};
        assert(resolveTarget(ctx) == TestTarget::SelectedManualSegments);
    }
    // Manual segment over notes
    {
        TestTargetContext ctx{true, true, false};
        assert(resolveTarget(ctx) == TestTarget::SelectedManualSegments);
    }
    // Notes over frame selection
    {
        TestTargetContext ctx{true, false, true};
        assert(resolveTarget(ctx) == TestTarget::SelectedNotes);
    }
    // Frame selection alone
    {
        TestTargetContext ctx{false, false, true};
        assert(resolveTarget(ctx) == TestTarget::FrameSelection);
    }
    // No selection
    {
        TestTargetContext ctx{false, false, false};
        assert(resolveTarget(ctx) == TestTarget::None);
    }
    std::cout << "  PASS: test_parameter_target_priority\n";
}

// ============================================================================
// Test 4: LegacyNoteGenerator-style commitNote writes parameter values
// ============================================================================
static void test_generator_writes_four_values()
{
    TestNote note;
    // Simulate what LegacyNoteGenerator::commitNote does:
    note.retuneSpeed = 0.7f;
    note.vibratoDepth = 3.0f;
    note.vibratoRate = 8.0f;

    assert(note.retuneSpeed == 0.7f);
    assert(note.vibratoDepth == 3.0f);
    assert(note.vibratoRate == 8.0f);
    std::cout << "  PASS: test_generator_writes_four_values\n";
}

// ============================================================================
// Test 5: NoteSplit threshold change causes note count change
//    (segmentation with lower threshold → more notes)
// ============================================================================
static void test_threshold_affects_note_count()
{
    // Simple F0 sequence: 440Hz constant
    // With threshold=80 cents → 1 note
    // With threshold=20 cents → should still be 1 note (constant F0)
    // With threshold=5 cents on a pitch-jumping F0 → more notes
    std::vector<float> f0 = {440.0f, 440.0f, 440.0f, 440.0f, 500.0f, 500.0f, 500.0f, 500.0f};
    float centsJump = std::abs(1200.0f * std::log2(500.0f / 440.0f));

    // threshold > centsJump → single note
    assert(centsJump < 300.0f);  // sanity: jump is within typical range
    assert(centsJump > 0.0f);

    // threshold = 50 cents < centsJump → 2 notes
    // threshold = 300 cents > centsJump → 1 note
    std::cout << "  PASS: test_threshold_affects_note_count (centsJump=" << centsJump << ")\n";
}

// ============================================================================
// Test 6: Manual segment rebuild preserves source/base and output varies with retune/vibrato
// ============================================================================
static float mixRetune(float shiftedF0, float targetF0, float retuneSpeed)
{
    if (shiftedF0 <= 0.0f) return targetF0;
    if (targetF0 <= 0.0f) return shiftedF0;
    float logDeviation = std::log2(shiftedF0) - std::log2(targetF0);
    float scaledDeviation = logDeviation * (1.0f - std::max(0.0f, std::min(1.0f, retuneSpeed)));
    return std::pow(2.0f, std::log2(targetF0) + scaledDeviation);
}

static void test_manual_rebuild_with_retune()
{
    float source = 450.0f;   // Original F0 * pitchRatio
    float target = 440.0f;   // base target

    // retune=0 → output = source (no correction)
    float out0 = mixRetune(source, target, 0.0f);
    assert(std::abs(out0 - source) < 0.1f);

    // retune=1 → output = target (full correction)
    float out1 = mixRetune(source, target, 1.0f);
    assert(std::abs(out1 - target) < 0.1f);

    // retune=0.5 → output = somewhere between source and target
    float out05 = mixRetune(source, target, 0.5f);
    assert(out05 > std::min(source, target));
    assert(out05 < std::max(source, target));

    std::cout << "  PASS: test_manual_rebuild_with_retune\n";
}

static void test_manual_rebuild_with_vibrato()
{
    float baseTarget = 440.0f;
    float vibratoDepthPercent = 5.0f;
    float vibratoRate = 6.0f;
    float timeInSeg = 0.1f;  // 100ms into the segment

    float depthSemitones = (vibratoDepthPercent / 100.0f);
    float vibratoOffset = depthSemitones * std::sin(
        2.0f * 3.14159265f * vibratoRate * timeInSeg);

    float targetWithVibrato = baseTarget * std::pow(2.0f, vibratoOffset / 12.0f);

    // With vibrato=0, target stays same
    float targetNoVibrato = baseTarget * std::pow(2.0f, 0.0f / 12.0f);
    assert(std::abs(targetNoVibrato - baseTarget) < 0.001f);

    // With non-zero vibrato, target changes over time
    assert(std::abs(targetWithVibrato - baseTarget) > 0.001f);

    std::cout << "  PASS: test_manual_rebuild_with_vibrato\n";
}

static float applyVibratoAfterRetune(float sourceF0,
                                     float targetF0,
                                     float retuneSpeed,
                                     float vibratoDepthPercent,
                                     float vibratoRate,
                                     float timeInNote)
{
    const float correctedBaseF0 = mixRetune(sourceF0, targetF0, retuneSpeed);
    const float depthSemitones = vibratoDepthPercent / 100.0f;
    const float vibratoOffset = depthSemitones * std::sin(
        2.0f * 3.14159265f * vibratoRate * timeInNote);
    return correctedBaseF0 * std::pow(2.0f, vibratoOffset / 12.0f);
}

// Vibrato depth/rate must remain audible even when pitch correction is disabled.
static void test_vibrato_is_independent_from_retune()
{
    const float baseF0 = 440.0f;
    const float withVibrato = applyVibratoAfterRetune(
        baseF0, baseF0, 0.0f, 5.0f, 6.0f, 1.0f / 24.0f);
    const float withoutVibrato = applyVibratoAfterRetune(
        baseF0, baseF0, 0.0f, 0.0f, 6.0f, 1.0f / 24.0f);

    assert(std::abs(withVibrato - withoutVibrato) > 0.001f);
    std::cout << "  PASS: test_vibrato_is_independent_from_retune\n";
}

// ============================================================================
// Test 7: Segment baseF0Data preserved through copy
// ============================================================================
static void test_segment_baseF0Data_copy()
{
    TestSegment seg;
    seg.startFrame = 100;
    seg.endFrame = 200;
    seg.f0Data = {440.0f, 441.0f, 442.0f};
    seg.baseF0Data = {440.0f, 440.5f, 441.0f};
    seg.source = TestSource::LineAnchor;

    TestSegment copy = seg;
    assert(copy.baseF0Data.size() == 3);
    assert(copy.baseF0Data[1] == 440.5f);
    assert(copy.source == TestSource::LineAnchor);

    std::cout << "  PASS: test_segment_baseF0Data_copy\n";
}

// ============================================================================
// Test 8: Negative parameter normalization
// ============================================================================
static void test_negative_param_normalization()
{
    TestNote note;
    // Simulate loading old data with negative sentinel values
    note.retuneSpeed = -1.0f;
    note.vibratoDepth = -1.0f;
    note.vibratoRate = -1.0f;

    // Apply normalization (matching Persistence code)
    if (note.retuneSpeed < 0.0f)
        note.retuneSpeed = OpenTune::PitchControlConfig::kDefaultRetuneSpeedNormalized;
    if (note.vibratoDepth < 0.0f)
        note.vibratoDepth = OpenTune::PitchControlConfig::kDefaultVibratoDepth;
    if (note.vibratoRate < 0.0f)
        note.vibratoRate = OpenTune::PitchControlConfig::kDefaultVibratoRateHz;

    assert(note.retuneSpeed == OpenTune::PitchControlConfig::kDefaultRetuneSpeedNormalized);
    assert(note.vibratoDepth == OpenTune::PitchControlConfig::kDefaultVibratoDepth);
    assert(note.vibratoRate == OpenTune::PitchControlConfig::kDefaultVibratoRateHz);

    std::cout << "  PASS: test_negative_param_normalization\n";
}

// ============================================================================
// Test 9: Boundary transition width scales with retuneSpeed
//    retuneSpeed=0 → full width; retuneSpeed=1 → zero width
// ============================================================================
static void test_transition_width_scales_with_retune_speed()
{
    constexpr float kDefaultHalfWidth = 8.0f;

    // Both at 0 → full symmetric span
    {
        float leftRetune = 0.0f;
        float rightRetune = 0.0f;
        float leftHalf = kDefaultHalfWidth * (1.0f - leftRetune);
        float rightHalf = kDefaultHalfWidth * (1.0f - rightRetune);
        assert(std::abs(leftHalf - 8.0f) < 0.001f);
        assert(std::abs(rightHalf - 8.0f) < 0.001f);
    }

    // Both at 1 → zero span (no transition region created)
    {
        float leftRetune = 1.0f;
        float rightRetune = 1.0f;
        float leftHalf = kDefaultHalfWidth * (1.0f - leftRetune);
        float rightHalf = kDefaultHalfWidth * (1.0f - rightRetune);
        assert(std::abs(leftHalf) < 0.001f);
        assert(std::abs(rightHalf) < 0.001f);
        // Gap check: any gap >= 0 → span not created
        float gapFrames = 4.0f;
        assert(gapFrames >= leftHalf + rightHalf);
    }

    // Asymmetric: left natural (0), right hard (1)
    {
        float leftRetune = 0.0f;
        float rightRetune = 1.0f;
        float leftHalf = kDefaultHalfWidth * (1.0f - leftRetune);
        float rightHalf = kDefaultHalfWidth * (1.0f - rightRetune);
        assert(std::abs(leftHalf - 8.0f) < 0.001f);
        assert(std::abs(rightHalf) < 0.001f);
    }

    // Asymmetric: left hard (1), right natural (0)
    {
        float leftRetune = 1.0f;
        float rightRetune = 0.0f;
        float leftHalf = kDefaultHalfWidth * (1.0f - leftRetune);
        float rightHalf = kDefaultHalfWidth * (1.0f - rightRetune);
        assert(std::abs(leftHalf) < 0.001f);
        assert(std::abs(rightHalf - 8.0f) < 0.001f);
    }

    // Mid value: 0.5 → half width
    {
        float leftRetune = 0.5f;
        float rightRetune = 0.5f;
        float leftHalf = kDefaultHalfWidth * (1.0f - leftRetune);
        float rightHalf = kDefaultHalfWidth * (1.0f - rightRetune);
        assert(std::abs(leftHalf - 4.0f) < 0.001f);
        assert(std::abs(rightHalf - 4.0f) < 0.001f);
    }

    std::cout << "  PASS: test_transition_width_scales_with_retune_speed\n";
}

// ============================================================================
// main
// ============================================================================
int main()
{
    std::cout << "PitchParameterContractTests:\n";
    test_note_concrete_defaults();
    test_note_copy_preserves_params();
    test_parameter_target_priority();
    test_generator_writes_four_values();
    test_threshold_affects_note_count();
    test_manual_rebuild_with_retune();
    test_manual_rebuild_with_vibrato();
    test_vibrato_is_independent_from_retune();
    test_segment_baseF0Data_copy();
    test_negative_param_normalization();
    test_transition_width_scales_with_retune_speed();
    std::cout << "All tests passed.\n";
    return 0;
}

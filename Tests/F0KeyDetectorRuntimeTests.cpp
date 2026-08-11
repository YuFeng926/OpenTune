/**
 * F0KeyDetector runtime tests — direct use of the production F0-driven key
 * detector (Source/DSP/F0KeyDetector.h). No mocks and no copied algorithms.
 *
 * 样本纪律：只用完整七音级（大调/自然小调），禁止五声音阶样本——
 * 五声对 Major/Minor 区分弱，会造成假失败。
 */
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "DSP/F0KeyDetector.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

// A4 = 440 Hz = MIDI 69
float midiToFreq(float midiNote)
{
    return 440.0f * std::pow(2.0f, (midiNote - 69.0f) / 12.0f);
}

// 每音级 4 帧的帧序列
std::vector<float> framesPerDegree(const std::vector<float>& midiDegrees, int framesPerDegree)
{
    std::vector<float> frames;
    for (float midiNote : midiDegrees)
        for (int i = 0; i < framesPerDegree; ++i)
            frames.push_back(midiToFreq(midiNote));
    return frames;
}

void testCMajor()
{
    using namespace OpenTune;

    // C4 D4 E4 F4 G4 A4 B4，每音级 4 帧，能量全 1.0
    const auto f0 = framesPerDegree({60.0f, 62.0f, 64.0f, 65.0f, 67.0f, 69.0f, 71.0f}, 4);
    const std::vector<float> energies(f0.size(), 1.0f);

    F0KeyDetector detector;
    const auto key = detector.detect(f0, energies);

    expect(key.root == Key::C, "C major frame sequence detects root C");
    expect(key.scale == Scale::Major, "C major frame sequence detects scale Major");
    expect(key.origin == Origin::Automatic, "detect marks the result as Origin::Automatic");
    expect(key.confidence > 0.0f, "detect reports positive confidence for a clean C major");
}

void testCNaturalMinor()
{
    using namespace OpenTune;

    // C natural minor: C D Eb F G Ab Bb。
    // 均匀帧分布下 C natural minor 与相对大调 Eb major 是同一音集的平移，
    // 信息论上不可区分，任何检测器都无法判定——故按真实旋律分布取样：
    // 根音 C 重权、五度 G 次重、其余音级轻权（主音/属音占优是 KS 方法
    // 正确判别的信息前提）。
    std::vector<float> f0;
    auto add = [&f0](float midiNote, int count) {
        for (int i = 0; i < count; ++i)
            f0.push_back(midiToFreq(midiNote));
    };
    add(60.0f, 8);  // C  根音
    add(67.0f, 5);  // G  五度
    add(62.0f, 3);  // D
    add(63.0f, 3);  // Eb
    add(65.0f, 3);  // F
    add(68.0f, 3);  // Ab
    add(70.0f, 3);  // Bb
    const std::vector<float> energies(f0.size(), 1.0f);

    F0KeyDetector detector;
    const auto key = detector.detect(f0, energies);

    expect(key.root == Key::C, "C natural minor frame sequence detects root C");
    expect(key.scale == Scale::Minor, "C natural minor frame sequence detects scale Minor");
    expect(key.origin == Origin::Automatic, "detect marks the minor result as Origin::Automatic");
    expect(key.confidence > 0.0f, "detect reports positive confidence for a clean C minor");
}

void testNoValidFrames()
{
    using namespace OpenTune;

    F0KeyDetector detector;

    // 全 0 频率：无有效帧
    const std::vector<float> zeros(28, 0.0f);
    const std::vector<float> energies(28, 1.0f);
    expect(detector.detect(zeros, energies).origin == Origin::Unset,
           "all-zero F0 frames yield an unset detection");

    // 全 NaN 频率：无有效帧
    const std::vector<float> nans(28, std::numeric_limits<float>::quiet_NaN());
    expect(detector.detect(nans, energies).origin == Origin::Unset,
           "all-NaN F0 frames yield an unset detection");
}

void testOriginMigration()
{
    using namespace OpenTune;

    expect(DetectedKey::originFromLegacyConfidence(0.0f) == Origin::Unset,
           "legacy confidence 0 migrates to Unset");
    expect(DetectedKey::originFromLegacyConfidence(0.5f) == Origin::Automatic,
           "legacy confidence in (0,1) migrates to Automatic");
    expect(DetectedKey::originFromLegacyConfidence(1.0f) == Origin::Manual,
           "legacy confidence 1 migrates to Manual");
    expect(DetectedKey::originFromLegacyConfidence(1.5f) == Origin::Unset,
           "legacy confidence >1 migrates to Unset, never Manual");
    expect(DetectedKey::originFromLegacyConfidence(-0.5f) == Origin::Unset,
           "negative legacy confidence migrates to Unset");
}

} // namespace

int main()
{
    testCMajor();
    testCNaturalMinor();
    testNoValidFrames();
    testOriginMigration();

    if (failures != 0) {
        std::cerr << failures << " F0KeyDetector test(s) failed\n";
        return 1;
    }

    std::cout << "F0KeyDetector tests passed\n";
    return 0;
}

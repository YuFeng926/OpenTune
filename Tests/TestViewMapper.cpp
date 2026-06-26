#include "../Source/Standalone/UI/ViewMapper.h"

#include <cmath>
#include <iostream>

using namespace OpenTune;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cout << "[FAIL] " << message << "\n";
        return false;
    }
    return true;
}

bool expectNear(double actual, double expected, double tolerance, const char* message)
{
    if (std::abs(actual - expected) > tolerance) {
        std::cout << "[FAIL] " << message << " expected=" << expected
                  << " actual=" << actual << " tolerance=" << tolerance << "\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToX — 基本转换
// 公式: contentStartX + llround((absoluteSeconds - visibleStartSeconds) * pixelsPerSecond)
// ---------------------------------------------------------------------------
bool timeToX_basicConversion()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // 1.0 秒 * 100 pps = 100 像素
    if (!expect(vm.timeToX(1.0) == 100, "timeToX_basicConversion: 1.0s at 100pps = 100px"))
        return false;

    // 2.5 秒 * 100 pps = 250 像素
    if (!expect(vm.timeToX(2.5) == 250, "timeToX_basicConversion: 2.5s at 100pps = 250px"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToX — contentStartX 偏移
// ---------------------------------------------------------------------------
bool timeToX_withContentStartX()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 40;  // pianoKeyWidth

    // 1.0 秒 * 100 pps + 40 = 140
    if (!expect(vm.timeToX(1.0) == 140, "timeToX_withContentStartX: 1.0s + 40 offset = 140px"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToX — visibleStartSeconds 偏移
// ---------------------------------------------------------------------------
bool timeToX_withVisibleStart()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 2.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // (3.0 - 2.0) * 100 = 100
    if (!expect(vm.timeToX(3.0) == 100, "timeToX_withVisibleStart: (3.0-2.0)*100 = 100px"))
        return false;

    // visibleStart 本身 → X=0
    if (!expect(vm.timeToX(2.0) == 0, "timeToX_withVisibleStart: visibleStart itself = 0px"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper xToTime — timeToX 的逆
// 公式: visibleStartSeconds + (x - contentStartX) / pixelsPerSecond
// ---------------------------------------------------------------------------
bool xToTime_inverseOfTimeToX()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // xToTime(100) = 0 + 100/100 = 1.0
    if (!expectNear(vm.xToTime(100), 1.0, 1e-9, "xToTime_inverseOfTimeToX: 100px = 1.0s"))
        return false;

    return true;
}

bool xToTime_withContentStartX()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 40;

    // xToTime(140) = 0 + (140-40)/100 = 1.0
    if (!expectNear(vm.xToTime(140), 1.0, 1e-9, "xToTime_withContentStartX: 140px with 40 offset = 1.0s"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToX — llround 半像素边界
// 0.005s * 100pps = 0.5px → llround(0.5) = 1（银行家舍入时可能为 0，但 std::llround 是 round-half-away-from-zero）
// ---------------------------------------------------------------------------
bool timeToX_roundingHalfPixel()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // 0.005 * 100 = 0.5 → llround(0.5) = 1 (round half away from zero)
    if (!expect(vm.timeToX(0.005) == 1, "timeToX_roundingHalfPixel: 0.005s*100 = 0.5px → llround = 1"))
        return false;

    // 0.004 * 100 = 0.4 → llround(0.4) = 0
    if (!expect(vm.timeToX(0.004) == 0, "timeToX_roundingHalfPixel: 0.004s*100 = 0.4px → llround = 0"))
        return false;

    // 0.006 * 100 = 0.6 → llround(0.6) = 1
    if (!expect(vm.timeToX(0.006) == 1, "timeToX_roundingHalfPixel: 0.006s*100 = 0.6px → llround = 1"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToX — 负时间（visibleStart 之前）
// ---------------------------------------------------------------------------
bool timeToX_negativeTime()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 1.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // (0.5 - 1.0) * 100 = -50 → llround(-50) = -50
    if (!expect(vm.timeToX(0.5) == -50, "timeToX_negativeTime: (0.5-1.0)*100 = -50px (before viewport)"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToX — 负半像素 llround 边界
// std::llround(-0.5) = -1 (round half away from zero)
// ---------------------------------------------------------------------------
bool timeToX_negativeHalfPixelRounding()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // -0.005 * 100 = -0.5 → llround(-0.5) = -1 (round half away from zero)
    if (!expect(vm.timeToX(-0.005) == -1, "timeToX_negativeHalfPixel: -0.005s*100 = -0.5px → llround = -1"))
        return false;

    // -0.004 * 100 = -0.4 → llround(-0.4) = 0
    if (!expect(vm.timeToX(-0.004) == 0, "timeToX_negativeHalfPixel: -0.004s*100 = -0.4px → llround = 0"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper xToTime — x < contentStartX（内容区域左侧）
// ---------------------------------------------------------------------------
bool xToTime_beforeContentStart()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 40;

    // xToTime(20) = 0 + (20-40)/100 = -0.2（负时间，在内容区域左侧）
    if (!expectNear(vm.xToTime(20), -0.2, 1e-9, "xToTime_beforeContentStart: (20-40)/100 = -0.2s"))
        return false;

    // xToTime(40) = 0 + (40-40)/100 = 0.0（内容区域起始）
    if (!expectNear(vm.xToTime(40), 0.0, 1e-9, "xToTime_beforeContentStart: contentStartX itself = 0.0s"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToXWithScroll — 不减 visibleStartSeconds
// 即使 visibleStart != 0，timeToXWithScroll 仍用绝对秒直接乘 pps
// ---------------------------------------------------------------------------
bool timeToXWithScroll_ignoresVisibleStart()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 5.0;  // 设置非零 visibleStart
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // timeToXWithScroll(1.0, 0) = 0 + llround(1.0*100) - 0 = 100
    // 如果错误地减了 visibleStart，会得到 (1.0-5.0)*100 = -400
    if (!expect(vm.timeToXWithScroll(1.0, 0) == 100,
                "timeToXWithScroll_ignoresVisibleStart: uses absolute seconds, not (abs-visibleStart)"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToContentX — 非整除 pps
// ---------------------------------------------------------------------------
bool timeToContentX_nonIntegerPps()
{
    ViewMapper vm;
    vm.pixelsPerSecond = 73.0;
    vm.contentStartX = 0;

    // 1.0 * 73 = 73
    if (!expect(vm.timeToContentX(1.0) == 73, "timeToContentX_nonIntegerPps: 1.0s*73pps = 73px"))
        return false;

    // 0.5 * 73 = 36.5 → llround(36.5) = 37 (round half away from zero)
    if (!expect(vm.timeToContentX(0.5) == 37, "timeToContentX_nonIntegerPps: 0.5s*73pps = 36.5 → llround = 37"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToX / xToTime round-trip
// 对任意 X: timeToX(xToTime(X)) == X（在舍入误差内）
// ---------------------------------------------------------------------------
bool roundTrip_timeToX_xToTime()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 5.0;
    vm.pixelsPerSecond = 73.0;  // 非整除 pps，测试舍入
    vm.contentStartX = 30;

    const int testXs[] = {30, 100, 250, 500, 1000};
    for (const int originalX : testXs) {
        const double t = vm.xToTime(originalX);
        const int roundTrippedX = vm.timeToX(t);
        // llround 可能引入 ±1 像素误差
        if (!expect(std::abs(roundTrippedX - originalX) <= 1,
                    "roundTrip_timeToX_xToTime: round-trip should be within 1px"))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToContentX — 不含 contentStartX 偏移
// 公式: llround(absoluteSeconds * pixelsPerSecond)
// ---------------------------------------------------------------------------
bool timeToContentX_noOffset()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 5.0;  // 不影响 contentX
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 40;  // 不影响 contentX

    // 1.0 * 100 = 100（不受 visibleStart/contentStartX 影响）
    if (!expect(vm.timeToContentX(1.0) == 100, "timeToContentX_noOffset: 1.0s*100pps = 100 (no offsets)"))
        return false;

    // 0 秒 → 0
    if (!expect(vm.timeToContentX(0.0) == 0, "timeToContentX_noOffset: 0s = 0px"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper timeToXWithScroll — 投影 scroll 变体
// 公式: contentStartX + llround(absoluteSeconds * pixelsPerSecond) - projectedScrollOffset
// 注意：不减 visibleStartSeconds，用绝对秒直接乘 pps
// ---------------------------------------------------------------------------
bool timeToXWithScroll_projectedScroll()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 0;

    // 1.0*100 - 50 = 50
    if (!expect(vm.timeToXWithScroll(1.0, 50) == 50,
                "timeToXWithScroll_projectedScroll: 1.0s*100 - 50scroll = 50px"))
        return false;

    // 2.0*100 - 150 = 50
    if (!expect(vm.timeToXWithScroll(2.0, 150) == 50,
                "timeToXWithScroll_projectedScroll: 2.0s*100 - 150scroll = 50px"))
        return false;

    return true;
}

bool timeToXWithScroll_withContentStartX()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 0.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 40;

    // 40 + 1.0*100 - 50 = 90
    if (!expect(vm.timeToXWithScroll(1.0, 50) == 90,
                "timeToXWithScroll_withContentStartX: 40 + 100 - 50 = 90px"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper midiToY / yToMidi — 垂直坐标转换
// midiToY: (maxMidi - midi) * pixelsPerSemitone - verticalScrollOffset
// yToMidi: maxMidi - (y + verticalScrollOffset) / pixelsPerSemitone
// ---------------------------------------------------------------------------
bool midiToY_yToMidi_basic()
{
    ViewMapper vm;
    vm.maxMidi = 127.0f;
    vm.pixelsPerSemitone = 10.0f;
    vm.verticalScrollOffset = 0.0f;

    // midiToY(60) = (127-60)*10 - 0 = 670
    if (!expectNear(vm.midiToY(60.0f), 670.0f, 1e-5f, "midiToY_basic: (127-60)*10 = 670"))
        return false;

    // yToMidi(670) = 127 - (670+0)/10 = 127 - 67 = 60
    if (!expectNear(vm.yToMidi(670.0f), 60.0f, 1e-5f, "yToMidi_basic: 127 - 670/10 = 60"))
        return false;

    return true;
}

bool midiToY_yToMidi_withScrollOffset()
{
    ViewMapper vm;
    vm.maxMidi = 127.0f;
    vm.pixelsPerSemitone = 10.0f;
    vm.verticalScrollOffset = 50.0f;

    // midiToY(60) = (127-60)*10 - 50 = 670 - 50 = 620
    if (!expectNear(vm.midiToY(60.0f), 620.0f, 1e-5f, "midiToY_withScroll: (127-60)*10 - 50 = 620"))
        return false;

    // yToMidi(620) = 127 - (620+50)/10 = 127 - 67 = 60
    if (!expectNear(vm.yToMidi(620.0f), 60.0f, 1e-5f, "yToMidi_withScroll: 127 - (620+50)/10 = 60"))
        return false;

    return true;
}

bool midiToY_yToMidi_roundTrip()
{
    ViewMapper vm;
    vm.maxMidi = 127.0f;
    vm.pixelsPerSemitone = 8.5f;
    vm.verticalScrollOffset = 33.0f;

    const float testMidis[] = {0.0f, 30.0f, 60.0f, 69.0f, 96.0f, 127.0f};
    for (const float originalMidi : testMidis) {
        const float y = vm.midiToY(originalMidi);
        const float roundTrippedMidi = vm.yToMidi(y);
        if (!expectNear(roundTrippedMidi, originalMidi, 1e-4f,
                        "midiToY_yToMidi_roundTrip: round-trip should preserve midi"))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper freqToMidi / midiToFreq — 频率↔MIDI 转换
// freqToMidi: 12*log2(hz/440) + 69 - 0.5  (centering: note N occupies [N-0.5, N+0.5))
// midiToFreq: getMidiNoteInHertz(roundToInt(midi + 0.5))
// ---------------------------------------------------------------------------
bool freqToMidi_a4_440hz()
{
    ViewMapper vm;

    // 440 Hz = MIDI 69, centered → 68.5
    const float midi = vm.freqToMidi(440.0f);
    if (!expectNear(midi, 68.5f, 1e-4f, "freqToMidi_a4: 440Hz = midi 68.5 (69 - 0.5 centering)"))
        return false;

    return true;
}

bool midiToFreq_a4_68_5()
{
    ViewMapper vm;

    // midi 68.5 + 0.5 = 69 → getMidiNoteInHertz(69) = 440
    const float freq = vm.midiToFreq(68.5f);
    if (!expectNear(freq, 440.0f, 0.01f, "midiToFreq_a4: midi 68.5 → 440Hz"))
        return false;

    return true;
}

bool freqToMidi_midiToFreq_roundTrip()
{
    ViewMapper vm;

    // 对标准音高做 round-trip：freqToMidi(hz) → midiToFreq → 应回到原 hz（量化到最近音符）
    const struct { float hz; int expectedMidiNote; } cases[] = {
        {261.63f, 60},   // C4 ≈ 261.63 Hz
        {293.66f, 62},   // D4 ≈ 293.66 Hz
        {329.63f, 64},   // E4 ≈ 329.63 Hz
        {440.00f, 69},   // A4 = 440 Hz
        {880.00f, 81},   // A5 = 880 Hz
    };

    for (const auto& c : cases) {
        const float midi = vm.freqToMidi(c.hz);
        const float freq = vm.midiToFreq(midi);
        // midiToFreq 量化到最近整数 MIDI 音符
        const float expectedFreq = static_cast<float>(
            juce::MidiMessage::getMidiNoteInHertz(c.expectedMidiNote));
        if (!expectNear(freq, expectedFreq, 0.1f,
                        "freqToMidi_midiToFreq_roundTrip: round-trip should quantize to nearest note"))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper freqToY / yToFreq — 组合转换 round-trip
// freqToY = midiToY(freqToMidi(hz))
// yToFreq = midiToFreq(yToMidi(y))
// ---------------------------------------------------------------------------
bool freqToY_yToFreq_roundTrip()
{
    ViewMapper vm;
    vm.maxMidi = 127.0f;
    vm.pixelsPerSemitone = 10.0f;
    vm.verticalScrollOffset = 0.0f;

    // 440 Hz → midi 68.5 → Y = (127-68.5)*10 = 585
    const float y = vm.freqToY(440.0f);
    if (!expectNear(y, 585.0f, 0.1f, "freqToY: 440Hz → midi 68.5 → Y=585"))
        return false;

    // Y=585 → midi 68.5 → freq 440
    const float freq = vm.yToFreq(585.0f);
    if (!expectNear(freq, 440.0f, 0.1f, "yToFreq: Y=585 → midi 68.5 → 440Hz"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper withBand — 带状渲染映射器
// withBand(bandStartContentX, bandWidth, bandHeight):
//   bandVisibleStartSeconds = bandStartContentX / pixelsPerSecond
//   contentStartX = 0 (band-local)
//   contentWidth = bandWidth
//   contentHeight = bandHeight
//   其余字段继承
// ---------------------------------------------------------------------------
bool withBand_createsCorrectMapper()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 5.0;
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 40;
    vm.contentWidth = 800;
    vm.contentHeight = 600;
    vm.pixelsPerSemitone = 10.0f;
    vm.verticalScrollOffset = 20.0f;
    vm.maxMidi = 127.0f;

    const auto band = vm.withBand(200, 50, 100);

    // bandVisibleStartSeconds = 200 / 100 = 2.0
    if (!expectNear(band.visibleStartSeconds, 2.0, 1e-9, "withBand: visibleStart = 200/100 = 2.0s"))
        return false;

    // contentStartX = 0 (band-local)
    if (!expect(band.contentStartX == 0, "withBand: contentStartX = 0 (band-local)"))
        return false;

    // contentWidth = 50
    if (!expect(band.contentWidth == 50, "withBand: contentWidth = 50"))
        return false;

    // contentHeight = 100
    if (!expect(band.contentHeight == 100, "withBand: contentHeight = 100"))
        return false;

    // pixelsPerSecond 继承
    if (!expectNear(band.pixelsPerSecond, 100.0, 1e-9, "withBand: pps inherited = 100"))
        return false;

    // pixelsPerSemitone 继承
    if (!expectNear(band.pixelsPerSemitone, 10.0f, 1e-5f, "withBand: pixelsPerSemitone inherited = 10"))
        return false;

    // verticalScrollOffset 继承
    if (!expectNear(band.verticalScrollOffset, 20.0f, 1e-5f, "withBand: verticalScrollOffset inherited = 20"))
        return false;

    // maxMidi 继承
    if (!expectNear(band.maxMidi, 127.0f, 1e-5f, "withBand: maxMidi inherited = 127"))
        return false;

    return true;
}

bool withBand_timeToX_usesBandVisibleStart()
{
    ViewMapper vm;
    vm.visibleStartSeconds = 5.0;  // 父 mapper 的 visibleStart，不应影响 band
    vm.pixelsPerSecond = 100.0;
    vm.contentStartX = 40;

    const auto band = vm.withBand(200, 50, 100);

    // band.timeToX(2.0) = 0 + llround((2.0 - 2.0) * 100) = 0
    if (!expect(band.timeToX(2.0) == 0, "withBand_timeToX: band start time = 0px (band-local)"))
        return false;

    // band.timeToX(2.5) = 0 + llround((2.5 - 2.0) * 100) = 50
    if (!expect(band.timeToX(2.5) == 50, "withBand_timeToX: 0.5s into band = 50px"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// TimelineViewportCamera — 默认值和常量
// ---------------------------------------------------------------------------
bool camera_defaultValues()
{
    TimelineViewportCamera camera;

    if (!expectNear(camera.visibleStartSeconds, 0.0, 1e-9,
                    "camera_defaultValues: visibleStartSeconds = 0.0"))
        return false;

    if (!expectNear(camera.pixelsPerSecond, 100.0, 1e-9,
                    "camera_defaultValues: pixelsPerSecond = 100.0 (kDefault)"))
        return false;

    return true;
}

bool camera_kDefaultPixelsPerSecond()
{
    if (!expectNear(TimelineViewportCamera::kDefaultPixelsPerSecond, 100.0, 1e-9,
                    "camera_kDefaultPixelsPerSecond: constant = 100.0"))
        return false;

    // 默认构造的 camera 应使用该常量
    TimelineViewportCamera camera;
    if (!expectNear(camera.pixelsPerSecond,
                    TimelineViewportCamera::kDefaultPixelsPerSecond, 1e-9,
                    "camera_kDefaultPixelsPerSecond: default pps == kDefaultPixelsPerSecond"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// ViewMapper 默认初始化值
// ---------------------------------------------------------------------------
bool viewMapper_defaultValues()
{
    ViewMapper vm;

    if (!expectNear(vm.visibleStartSeconds, 0.0, 1e-9, "viewMapper_default: visibleStartSeconds = 0.0"))
        return false;
    if (!expectNear(vm.pixelsPerSecond, TimelineViewportCamera::kDefaultPixelsPerSecond, 1e-9,
                    "viewMapper_default: pixelsPerSecond = kDefaultPixelsPerSecond"))
        return false;
    if (!expect(vm.contentStartX == 0, "viewMapper_default: contentStartX = 0"))
        return false;
    if (!expect(vm.contentWidth == 0, "viewMapper_default: contentWidth = 0"))
        return false;
    if (!expect(vm.contentHeight == 0, "viewMapper_default: contentHeight = 0"))
        return false;
    if (!expectNear(vm.pixelsPerSemitone, 1.0f, 1e-5f, "viewMapper_default: pixelsPerSemitone = 1.0"))
        return false;
    if (!expectNear(vm.verticalScrollOffset, 0.0f, 1e-5f, "viewMapper_default: verticalScrollOffset = 0.0"))
        return false;
    if (!expectNear(vm.maxMidi, 127.0f, 1e-5f, "viewMapper_default: maxMidi = 127.0"))
        return false;

    return true;
}

} // namespace

int main()
{
    int failed = 0;

    auto run = [&](bool (*testFn)(), const char* name) {
        try {
            if (!testFn()) {
                ++failed;
            }
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << name << " uncaught exception: " << e.what() << "\n";
            ++failed;
        }
    };

    // --- timeToX ---
    run(timeToX_basicConversion,              "timeToX_basicConversion");
    run(timeToX_withContentStartX,            "timeToX_withContentStartX");
    run(timeToX_withVisibleStart,             "timeToX_withVisibleStart");
    run(timeToX_roundingHalfPixel,            "timeToX_roundingHalfPixel");
    run(timeToX_negativeTime,                 "timeToX_negativeTime");
    run(timeToX_negativeHalfPixelRounding,    "timeToX_negativeHalfPixelRounding");

    // --- xToTime ---
    run(xToTime_inverseOfTimeToX,             "xToTime_inverseOfTimeToX");
    run(xToTime_withContentStartX,            "xToTime_withContentStartX");
    run(xToTime_beforeContentStart,           "xToTime_beforeContentStart");

    // --- round-trip ---
    run(roundTrip_timeToX_xToTime,            "roundTrip_timeToX_xToTime");

    // --- timeToContentX ---
    run(timeToContentX_noOffset,              "timeToContentX_noOffset");
    run(timeToContentX_nonIntegerPps,         "timeToContentX_nonIntegerPps");

    // --- timeToXWithScroll ---
    run(timeToXWithScroll_projectedScroll,    "timeToXWithScroll_projectedScroll");
    run(timeToXWithScroll_withContentStartX,  "timeToXWithScroll_withContentStartX");
    run(timeToXWithScroll_ignoresVisibleStart,"timeToXWithScroll_ignoresVisibleStart");

    // --- midiToY / yToMidi ---
    run(midiToY_yToMidi_basic,                "midiToY_yToMidi_basic");
    run(midiToY_yToMidi_withScrollOffset,     "midiToY_yToMidi_withScrollOffset");
    run(midiToY_yToMidi_roundTrip,            "midiToY_yToMidi_roundTrip");

    // --- freqToMidi / midiToFreq ---
    run(freqToMidi_a4_440hz,                  "freqToMidi_a4_440hz");
    run(midiToFreq_a4_68_5,                   "midiToFreq_a4_68_5");
    run(freqToMidi_midiToFreq_roundTrip,      "freqToMidi_midiToFreq_roundTrip");

    // --- freqToY / yToFreq ---
    run(freqToY_yToFreq_roundTrip,            "freqToY_yToFreq_roundTrip");

    // --- withBand ---
    run(withBand_createsCorrectMapper,        "withBand_createsCorrectMapper");
    run(withBand_timeToX_usesBandVisibleStart,"withBand_timeToX_usesBandVisibleStart");

    // --- TimelineViewportCamera ---
    run(camera_defaultValues,                 "camera_defaultValues");
    run(camera_kDefaultPixelsPerSecond,       "camera_kDefaultPixelsPerSecond");

    // --- ViewMapper defaults ---
    run(viewMapper_defaultValues,             "viewMapper_defaultValues");

    if (failed != 0) {
        std::cout << failed << " ViewMapper tests failed.\n";
        return 1;
    }

    std::cout << "All 27 ViewMapper tests passed.\n";
    return 0;
}

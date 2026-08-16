/**
 * NoteEqProcessor runtime tests — 直接调用生产 DSP（Source/DSP/NoteEqProcessor.h），
 * 无 mock、无复制算法。
 *
 * 覆盖契约 §3/§9：
 * - 两端（LowCut / HighCut）8 阶 Butterworth 48 dB/oct 斜率
 * - active=false 原样旁通
 * - 双声道一致（含 mono 与立体声左声道一致）
 * - Note 内分块处理连续（分块输出与整块输出逐位一致）
 * - reset 后跨 Note 状态等于新实例（绝不延续前一个 Note 的状态）
 * - 因果无前振铃（信号起始前输出精确为零）
 * - 系数按 prepare 传入采样率设计（生产调用固定 RenderCache::kSampleRate）
 *
 * 数值容差口径：斜率测量改用 NoteEqProcessor::magnitudeDb 解析频响（double 计算，
 * 按 z = e^{-jw} 遍历已绑定二阶段求总 |H|），不受 float32 时域量化噪声地板影响；
 * 测量点远离 Nyquist，控制双线性弯折偏差；±1 dB 容差保留。
 * 本文件不设置任何性能阈值，也不做绝对时间单位换算（时间一律以采样数为单位）。
 */
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "DSP/NoteEqProcessor.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

constexpr double kSampleRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kNumSamples = 44100;  // 时域测试信号长度

// 纯正弦测试信号（双精度生成后写入 float32 缓冲）
juce::AudioBuffer<float> makeSine(int numChannels, double frequency)
{
    juce::AudioBuffer<float> buffer(numChannels, kNumSamples);
    for (int c = 0; c < numChannels; ++c) {
        auto* data = buffer.getWritePointer(c);
        for (int i = 0; i < kNumSamples; ++i)
            data[i] = static_cast<float>(std::sin(2.0 * kPi * frequency * i / kSampleRate));
    }
    return buffer;
}

bool bitExact(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels()
        || a.getNumSamples() != b.getNumSamples())
        return false;

    for (int c = 0; c < a.getNumChannels(); ++c) {
        const auto* da = a.getReadPointer(c);
        const auto* db = b.getReadPointer(c);
        for (int i = 0; i < a.getNumSamples(); ++i) {
            if (da[i] != db[i])
                return false;
        }
    }
    return true;
}

// 阻带两个相隔 1 oct 频点的频响幅值差（dB/oct）。
// 用 NoteEqProcessor::magnitudeDb 解析计算整链 |H|（double 计算），不做时域处理，
// 不受 float32 量化噪声地板影响；测量点远离 Nyquist，控制双线性弯折偏差。
// 默认增益全 0 dB 时 LowShelf / Peak / HighShelf 的 RBJ 系数精确为单位增益，
// 斜率测量只受被测 Cut 影响。
// 8 阶 Butterworth 阻带渐近斜率 = 20*8*log10(2) ≈ 48.16 dB/oct。
double stopbandSlopeDb(const OpenTune::EqSettings& settings,
                       double lowerFrequency, double upperFrequency)
{
    OpenTune::NoteEqProcessor processor;
    processor.prepare(kSampleRate, settings);

    return processor.magnitudeDb(lowerFrequency) - processor.magnitudeDb(upperFrequency);
}

void testCutSlopes()
{
    using namespace OpenTune;

    // LowCut 阻带（fc=200 Hz，测 fc/2 与 fc/4）：低频更衰减，差值为 -48.16 dB/oct
    {
        EqSettings settings;  // 默认值：Cut 80/12000 Hz，增益全 0
        settings.lowCutFrequencyHz = 200.0f;
        const double slopeDb = stopbandSlopeDb(settings, 50.0, 100.0);
        expect(std::abs(std::abs(slopeDb) - 48.16) < 1.0,
               "LowCut stopband rolls off at 48 dB/oct (8th-order Butterworth)");
    }

    // HighCut 阻带（fc=500 Hz，测 2*fc 与 4*fc）：高频更衰减，差值为 +48.16 dB/oct；
    // 测量点 1000/2000 Hz 远离 Nyquist，避免双线性弯折压缩斜率
    {
        EqSettings settings;
        settings.highCutFrequencyHz = 500.0f;
        const double slopeDb = stopbandSlopeDb(settings, 1000.0, 2000.0);
        expect(std::abs(std::abs(slopeDb) - 48.16) < 1.0,
               "HighCut stopband rolls off at 48 dB/oct (8th-order Butterworth)");
    }
}

void testInactivePassthrough()
{
    using namespace OpenTune;

    EqSettings settings;
    settings.active = false;  // 保留参数但全局旁通
    settings.lowCutFrequencyHz = 200.0f;
    settings.peakGainDb = 6.0f;

    NoteEqProcessor processor;
    processor.prepare(kSampleRate, settings);
    expect(!processor.isActive(), "active=false keeps the processor bypassed");

    auto input = makeSine(2, 440.0);
    const auto reference = input;
    processor.process(input);
    expect(bitExact(input, reference),
           "active=false passes audio through bit-exactly");
}

void testStereoConsistency()
{
    using namespace OpenTune;

    EqSettings settings;
    settings.lowCutFrequencyHz = 200.0f;
    settings.peakGainDb = 5.0f;
    settings.highCutFrequencyHz = 8000.0f;

    NoteEqProcessor processor;
    processor.prepare(kSampleRate, settings);

    auto stereo = makeSine(2, 440.0);
    processor.process(stereo);

    // 左右声道输入相同 → 输出逐位一致（两套独立状态，互不共享）
    const auto* left = stereo.getReadPointer(0);
    const auto* right = stereo.getReadPointer(1);
    bool identical = true;
    for (int i = 0; i < stereo.getNumSamples(); ++i)
        identical = identical && (left[i] == right[i]);
    expect(identical,
           "identical stereo input yields bit-identical processed channels");

    // mono 处理：单声道走左声道固定状态，结果与立体声左声道逐位一致
    auto mono = makeSine(1, 440.0);
    NoteEqProcessor monoProcessor;
    monoProcessor.prepare(kSampleRate, settings);
    monoProcessor.process(mono);

    auto* leftPtr = stereo.getWritePointer(0);
    const juce::AudioBuffer<float> leftView(&leftPtr, 1, 0, stereo.getNumSamples());
    expect(bitExact(mono, leftView),
           "mono processing matches the stereo left channel bit-exactly");
}

void testChunkedContinuity()
{
    using namespace OpenTune;

    EqSettings settings;
    settings.lowCutFrequencyHz = 200.0f;
    settings.peakGainDb = 5.0f;

    const auto input = makeSine(2, 330.0);

    // 整块一次处理
    auto whole = input;
    NoteEqProcessor wholeProcessor;
    wholeProcessor.prepare(kSampleRate, settings);
    wholeProcessor.process(whole);

    // 同一 Note 内分块连续处理：状态跨块延续，输出必须与整块逐位一致
    auto chunked = input;
    NoteEqProcessor chunkedProcessor;
    chunkedProcessor.prepare(kSampleRate, settings);

    const int chunkSizes[] = { 137, 512, 1024, 7 };
    int offset = 0;
    int chunkIndex = 0;
    while (offset < kNumSamples) {
        const int chunkSize =
            std::min(chunkSizes[chunkIndex % 4], kNumSamples - offset);
        std::vector<float*> pointers;
        for (int c = 0; c < 2; ++c)
            pointers.push_back(chunked.getWritePointer(c));
        juce::AudioBuffer<float> view(pointers.data(), 2, offset, chunkSize);
        chunkedProcessor.process(view);
        offset += chunkSize;
        ++chunkIndex;
    }

    expect(bitExact(whole, chunked),
           "chunked processing inside a note matches a single pass bit-exactly");
}

void testResetAcrossNotes()
{
    using namespace OpenTune;

    EqSettings settings;
    settings.lowCutFrequencyHz = 200.0f;
    settings.peakGainDb = 5.0f;

    const auto note1 = makeSine(2, 220.0);
    const auto note2 = makeSine(2, 440.0);

    // 连续处理两个 Note：第二个 Note 前 reset()，绝不延续第一个 Note 的滤波状态
    auto note1Audio = note1;
    auto note2Audio = note2;
    NoteEqProcessor processor;
    processor.prepare(kSampleRate, settings);
    processor.process(note1Audio);
    processor.reset();
    processor.process(note2Audio);

    // 新实例直接处理同一输入：跨 Note reset 后的输出必须与新实例逐位一致
    auto freshAudio = note2;
    NoteEqProcessor fresh;
    fresh.prepare(kSampleRate, settings);
    fresh.process(freshAudio);

    expect(bitExact(note2Audio, freshAudio),
           "reset() makes the next note bit-identical to a fresh processor instance");
}

void testCausalNoPreRing()
{
    using namespace OpenTune;

    EqSettings settings;
    settings.lowCutFrequencyHz = 200.0f;
    settings.peakGainDb = 5.0f;
    settings.highCutFrequencyHz = 8000.0f;

    constexpr int kSilenceSamples = kNumSamples / 2;  // 前一半静音

    // 信号起始前为精确零，之后为 440 Hz 正弦
    juce::AudioBuffer<float> buffer(2, kNumSamples);
    for (int c = 0; c < 2; ++c) {
        auto* data = buffer.getWritePointer(c);
        for (int i = 0; i < kSilenceSamples; ++i)
            data[i] = 0.0f;
        for (int i = kSilenceSamples; i < kNumSamples; ++i)
            data[i] = static_cast<float>(std::sin(2.0 * kPi * 440.0 * i / kSampleRate));
    }

    NoteEqProcessor processor;
    processor.prepare(kSampleRate, settings);
    processor.process(buffer);

    // 因果性：零输入 + 零初始状态 → 信号起始前输出必须精确为零；
    // 任何非零即说明存在前振铃（非因果泄漏到过去）
    bool silentBeforeOnset = true;
    for (int c = 0; c < 2; ++c) {
        const auto* data = buffer.getReadPointer(c);
        for (int i = 0; i < kSilenceSamples; ++i)
            silentBeforeOnset = silentBeforeOnset && (data[i] == 0.0f);
    }
    expect(silentBeforeOnset,
           "output before signal onset is exactly zero (causal, no pre-ringing)");

    // sanity：信号区确实被 EQ 处理（非全零输出）
    bool audibleAfterOnset = false;
    for (int c = 0; c < 2; ++c) {
        const auto* data = buffer.getReadPointer(c);
        for (int i = kSilenceSamples; i < kNumSamples; ++i)
            audibleAfterOnset = audibleAfterOnset || (data[i] != 0.0f);
    }
    expect(audibleAfterOnset, "the EQ processes the signal region");
}

} // namespace

int main()
{
    testCutSlopes();
    testInactivePassthrough();
    testStereoConsistency();
    testChunkedContinuity();
    testResetAcrossNotes();
    testCausalNoPreRing();

    if (failures != 0) {
        std::cerr << failures << " NoteEq DSP runtime test(s) failed\n";
        return 1;
    }

    std::cout << "NoteEq DSP runtime tests passed\n";
    return 0;
}

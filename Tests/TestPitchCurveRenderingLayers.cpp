#include "../Source/Utils/PitchCurve.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cout << "[FAIL] " << message << "\n";
        return false;
    }
    return true;
}

bool testRenderFinalF0RangeWithCorrection()
{
    OpenTune::PitchCurve curve;
    curve.setHopSize(512);
    curve.setSampleRate(44100.0);

    std::vector<float> originalF0(50, 110.0f);
    curve.setOriginalF0(originalF0);

    std::vector<float> correctedF0(10, 220.0f);
    curve.setManualCorrectionRange(20, 30, correctedF0, OpenTune::PitchCorrectionSegment::Source::HandDraw);

    std::vector<float> rendered;
    curve.renderFinalF0Range(15, 35, [&](int, const float* data, int count) {
        for (int i = 0; i < count; ++i) {
            rendered.push_back(data[i]);
        }
    });

    if (!expect(rendered.size() == 20, "renderFinalF0Range must produce 20 frames"))
        return false;

    for (int i = 0; i < 5; ++i) {
        if (!expect(std::abs(rendered[i] - 110.0f) < 0.01f, "renderFinalF0Range must output original F0 before correction"))
            return false;
    }

    for (int i = 5; i < 15; ++i) {
        if (!expect(std::abs(rendered[i] - 220.0f) < 0.01f, "renderFinalF0Range must output corrected F0 in correction range"))
            return false;
    }

    for (int i = 15; i < 20; ++i) {
        if (!expect(std::abs(rendered[i] - 110.0f) < 0.01f, "renderFinalF0Range must output original F0 after correction"))
            return false;
    }

    return true;
}

bool testRenderCorrectionLayerF0RangeWithCorrection()
{
    OpenTune::PitchCurve curve;
    curve.setHopSize(512);
    curve.setSampleRate(44100.0);

    std::vector<float> originalF0(50, 110.0f);
    curve.setOriginalF0(originalF0);

    std::vector<float> correctedF0(10, 220.0f);
    curve.setManualCorrectionRange(20, 30, correctedF0, OpenTune::PitchCorrectionSegment::Source::HandDraw);

    std::vector<float> rendered;
    curve.renderCorrectionLayerF0Range(15, 35, [&](int, const float* data, int count) {
        for (int i = 0; i < count; ++i) {
            rendered.push_back(data[i]);
        }
    });

    if (!expect(rendered.size() == 20, "renderCorrectionLayerF0Range must produce 20 frames"))
        return false;

    for (int i = 0; i < 5; ++i) {
        if (!expect(std::abs(rendered[i]) < 0.01f, "renderCorrectionLayerF0Range must output 0 before correction"))
            return false;
    }

    for (int i = 5; i < 15; ++i) {
        if (!expect(std::abs(rendered[i] - 220.0f) < 0.01f, "renderCorrectionLayerF0Range must output corrected F0 in correction range"))
            return false;
    }

    for (int i = 15; i < 20; ++i) {
        if (!expect(std::abs(rendered[i]) < 0.01f, "renderCorrectionLayerF0Range must output 0 after correction"))
            return false;
    }

    return true;
}

bool testRenderFinalF0RangeNoCorrection()
{
    OpenTune::PitchCurve curve;
    curve.setHopSize(512);
    curve.setSampleRate(44100.0);

    std::vector<float> originalF0(50, 110.0f);
    curve.setOriginalF0(originalF0);

    std::vector<float> rendered;
    curve.renderFinalF0Range(10, 30, [&](int, const float* data, int count) {
        for (int i = 0; i < count; ++i) {
            rendered.push_back(data[i]);
        }
    });

    if (!expect(rendered.size() == 20, "renderFinalF0Range must produce 20 frames"))
        return false;

    for (int i = 0; i < 20; ++i) {
        if (!expect(std::abs(rendered[i] - 110.0f) < 0.01f, "renderFinalF0Range must output original F0 when no correction"))
            return false;
    }

    return true;
}

bool testRenderCorrectionLayerF0RangeNoCorrection()
{
    OpenTune::PitchCurve curve;
    curve.setHopSize(512);
    curve.setSampleRate(44100.0);

    std::vector<float> originalF0(50, 110.0f);
    curve.setOriginalF0(originalF0);

    std::vector<float> rendered;
    curve.renderCorrectionLayerF0Range(10, 30, [&](int, const float* data, int count) {
        for (int i = 0; i < count; ++i) {
            rendered.push_back(data[i]);
        }
    });

    if (!expect(rendered.size() == 20, "renderCorrectionLayerF0Range must produce 20 frames"))
        return false;

    for (int i = 0; i < 20; ++i) {
        if (!expect(std::abs(rendered[i]) < 0.01f, "renderCorrectionLayerF0Range must output 0 when no correction"))
            return false;
    }

    return true;
}

bool testHasFinalF0Data()
{
    OpenTune::PitchCurve curve;
    curve.setHopSize(512);
    curve.setSampleRate(44100.0);

    if (!expect(!curve.hasFinalF0Data(), "hasFinalF0Data must return false when no F0 data"))
        return false;

    curve.setOriginalF0(std::vector<float>(50, 110.0f));

    if (!expect(curve.hasFinalF0Data(), "hasFinalF0Data must return true when F0 data exists"))
        return false;

    return true;
}

bool testHasCorrectionLayer()
{
    OpenTune::PitchCurve curve;
    curve.setHopSize(512);
    curve.setSampleRate(44100.0);
    curve.setOriginalF0(std::vector<float>(50, 110.0f));

    if (!expect(!curve.hasCorrectionLayer(), "hasCorrectionLayer must return false when no correction"))
        return false;

    std::vector<float> correctedF0(10, 220.0f);
    curve.setManualCorrectionRange(20, 30, correctedF0, OpenTune::PitchCorrectionSegment::Source::HandDraw);

    if (!expect(curve.hasCorrectionLayer(), "hasCorrectionLayer must return true when correction exists"))
        return false;

    return true;
}

} // namespace

int main()
{
    if (!testRenderFinalF0RangeWithCorrection())
        return 1;

    if (!testRenderCorrectionLayerF0RangeWithCorrection())
        return 1;

    if (!testRenderFinalF0RangeNoCorrection())
        return 1;

    if (!testRenderCorrectionLayerF0RangeNoCorrection())
        return 1;

    if (!testHasFinalF0Data())
        return 1;

    if (!testHasCorrectionLayer())
        return 1;

    std::cout << "[PASS] PitchCurve rendering layers\n";
    return 0;
}
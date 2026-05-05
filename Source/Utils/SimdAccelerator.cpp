#include "SimdAccelerator.h"
#include "AppLogger.h"
#include <cmath>
#include <vector>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace OpenTune {

SimdAccelerator& SimdAccelerator::getInstance() {
    static SimdAccelerator instance;
    return instance;
}

// 构造时一次性绑定后端，之后只读——线程安全由 static local 保证
SimdAccelerator::SimdAccelerator()
#if defined(__APPLE__)
    : dotProductFunc_(dotProduct_Accelerate)
    , vectorLogFunc_(vectorLog_Accelerate)
    , vectorExpFunc_(vectorExp_Accelerate)
    , vectorSqrtFunc_(vectorSqrt_Accelerate)
    , complexMagnitudeFunc_(complexMagnitude_Accelerate)
    , backendName_("Apple Accelerate")
#else
    : dotProductFunc_(dotProduct_Scalar)
    , vectorLogFunc_(vectorLog_Scalar)
    , vectorExpFunc_(vectorExp_Scalar)
    , vectorSqrtFunc_(vectorSqrt_Scalar)
    , complexMagnitudeFunc_(complexMagnitude_Scalar)
    , backendName_("Scalar")
#endif
{
    AppLogger::info("[SimdAccelerator] Backend: " + juce::String(backendName_));
}

// ── 公共接口 ──────────────────────────────────────

float SimdAccelerator::dotProduct(const float* a, const float* b, size_t count) const {
    return dotProductFunc_(a, b, count);
}

void SimdAccelerator::vectorLog(float* result, const float* input, size_t count) const {
    vectorLogFunc_(result, input, count);
}

void SimdAccelerator::vectorExp(float* result, const float* input, size_t count) const {
    vectorExpFunc_(result, input, count);
}

void SimdAccelerator::vectorSqrt(float* result, const float* input, size_t count) const {
    vectorSqrtFunc_(result, input, count);
}

void SimdAccelerator::complexMagnitude(float* result, const float* complexData, size_t complexCount) const {
    complexMagnitudeFunc_(result, complexData, complexCount);
}

// ── 标量回退实现 ──────────────────────────────────

float SimdAccelerator::dotProduct_Scalar(const float* a, const float* b, size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) sum += a[i] * b[i];
    return sum;
}

void SimdAccelerator::vectorLog_Scalar(float* result, const float* input, size_t count) {
    for (size_t i = 0; i < count; ++i) result[i] = std::log(input[i]);
}

void SimdAccelerator::vectorExp_Scalar(float* result, const float* input, size_t count) {
    for (size_t i = 0; i < count; ++i) result[i] = std::exp(input[i]);
}

void SimdAccelerator::vectorSqrt_Scalar(float* result, const float* input, size_t count) {
    for (size_t i = 0; i < count; ++i) result[i] = std::sqrt(input[i]);
}

void SimdAccelerator::complexMagnitude_Scalar(float* result, const float* complexData, size_t complexCount) {
    for (size_t i = 0; i < complexCount; ++i) {
        float real = complexData[i * 2];
        float imag = complexData[i * 2 + 1];
        result[i] = std::sqrt(real * real + imag * imag);
    }
}

// ── Apple Accelerate 实现 ─────────────────────────

#if defined(__APPLE__)

float SimdAccelerator::dotProduct_Accelerate(const float* a, const float* b, size_t count) {
    float result = 0.0f;
    vDSP_dotpr(a, 1, b, 1, &result, static_cast<vDSP_Length>(count));
    return result;
}

void SimdAccelerator::vectorLog_Accelerate(float* result, const float* input, size_t count) {
    if (count == 0) return;
    const int n = static_cast<int>(count);
    vvlogf(result, input, &n);
}

void SimdAccelerator::vectorExp_Accelerate(float* result, const float* input, size_t count) {
    if (count == 0) return;
    const int n = static_cast<int>(count);
    vvexpf(result, input, &n);
}

void SimdAccelerator::vectorSqrt_Accelerate(float* result, const float* input, size_t count) {
    if (count == 0) return;
    const int n = static_cast<int>(count);
    vvsqrtf(result, input, &n);
}

void SimdAccelerator::complexMagnitude_Accelerate(float* result, const float* complexData, size_t complexCount) {
    if (complexCount == 0) return;
    std::vector<float> realPart(complexCount);
    std::vector<float> imagPart(complexCount);
    for (size_t i = 0; i < complexCount; ++i) {
        realPart[i] = complexData[i * 2];
        imagPart[i] = complexData[i * 2 + 1];
    }
    vDSP_vdist(realPart.data(), 1, imagPart.data(), 1,
               result, 1, static_cast<vDSP_Length>(complexCount));
}

#endif // __APPLE__

} // namespace OpenTune

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
    , backendName_("Apple Accelerate")
#else
    : dotProductFunc_(dotProduct_Scalar)
    , vectorLogFunc_(vectorLog_Scalar)
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

// ── 标量回退实现 ──────────────────────────────────

float SimdAccelerator::dotProduct_Scalar(const float* a, const float* b, size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) sum += a[i] * b[i];
    return sum;
}

void SimdAccelerator::vectorLog_Scalar(float* result, const float* input, size_t count) {
    for (size_t i = 0; i < count; ++i) result[i] = std::log(input[i]);
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

#endif // __APPLE__

} // namespace OpenTune

#include "SimdAccelerator.h"
#include "CpuFeatures.h"
#include "AppLogger.h"
#include <cmath>
#include <algorithm>

#ifdef _MSC_VER
#include <intrin.h>
#include <immintrin.h>
#endif

namespace OpenTune {

SimdAccelerator& SimdAccelerator::getInstance() {
    static SimdAccelerator instance;
    return instance;
}

void SimdAccelerator::detect() {
    if (detected_) return;
    
    auto& cpu = CpuFeatures::getInstance();
    
    // 根据CPU特性确定SIMD级别
    if (cpu.hasAVX512()) {
        simdLevel_ = SimdLevel::AVX512;
        sumOfSquaresFunc_ = sumOfSquares_AVX512;
        dotProductFunc_ = dotProduct_AVX512;
        multiplyAddFunc_ = multiplyAdd_AVX512;
        multiplyFunc_ = multiply_AVX512;
        addFunc_ = add_AVX512;
        absMaxFunc_ = absMax_AVX512;
        findMinMaxFunc_ = findMinMax_AVX512;
    } else if (cpu.hasAVX2()) {
        simdLevel_ = SimdLevel::AVX2;
        sumOfSquaresFunc_ = sumOfSquares_AVX;
        dotProductFunc_ = dotProduct_AVX;
        multiplyAddFunc_ = multiplyAdd_AVX;
        multiplyFunc_ = multiply_AVX;
        addFunc_ = add_AVX;
        absMaxFunc_ = absMax_AVX;
        findMinMaxFunc_ = findMinMax_AVX;
    } else if (cpu.hasAVX()) {
        simdLevel_ = SimdLevel::AVX;
        sumOfSquaresFunc_ = sumOfSquares_AVX;
        dotProductFunc_ = dotProduct_AVX;
        multiplyAddFunc_ = multiplyAdd_AVX;
        multiplyFunc_ = multiply_AVX;
        addFunc_ = add_AVX;
        absMaxFunc_ = absMax_AVX;
        findMinMaxFunc_ = findMinMax_AVX;
    } else if (cpu.hasSSE2()) {
        simdLevel_ = SimdLevel::SSE2;
        // SSE2 使用标量实现（JUCE 内部已优化），fallback 到标量实现
    } else {
        simdLevel_ = SimdLevel::None;
    }
    
    detected_ = true;
    AppLogger::debug("[SimdAccelerator] SIMD level: " + juce::String(getSimdLevelName()) 
              + " (vector width: " + juce::String(getVectorWidth()) + ")");
}

std::string SimdAccelerator::getSimdLevelName() const {
    switch (simdLevel_) {
        case SimdLevel::AVX512: return "AVX-512";
        case SimdLevel::AVX2: return "AVX2";
        case SimdLevel::AVX: return "AVX";
        case SimdLevel::SSE2: return "SSE2";
        case SimdLevel::None: return "None";
    }
    return "Unknown";
}

int SimdAccelerator::getVectorWidth() const {
    switch (simdLevel_) {
        case SimdLevel::AVX512: return 16;  // 512-bit = 16 floats
        case SimdLevel::AVX2:
        case SimdLevel::AVX: return 8;      // 256-bit = 8 floats
        case SimdLevel::SSE2: return 4;     // 128-bit = 4 floats
        case SimdLevel::None: return 1;
    }
    return 1;
}

// ==============================================================================
// 公共接口实现
// ==============================================================================

float SimdAccelerator::sumOfSquares(const float* data, size_t count) const {
    return sumOfSquaresFunc_(data, count);
}

float SimdAccelerator::dotProduct(const float* a, const float* b, size_t count) const {
    return dotProductFunc_(a, b, count);
}

void SimdAccelerator::multiplyAdd(float* result, const float* a, const float* b, float c, size_t count) const {
    multiplyAddFunc_(result, a, b, c, count);
}

void SimdAccelerator::multiply(float* result, const float* a, const float* b, size_t count) const {
    multiplyFunc_(result, a, b, count);
}

void SimdAccelerator::add(float* result, const float* a, const float* b, size_t count) const {
    addFunc_(result, a, b, count);
}

float SimdAccelerator::absMax(const float* data, size_t count) const {
    return absMaxFunc_(data, count);
}

void SimdAccelerator::findMinMax(const float* data, size_t count, float& outMin, float& outMax) const {
    findMinMaxFunc_(data, count, outMin, outMax);
}

// ==============================================================================
// 标量实现
// ==============================================================================

float SimdAccelerator::sumOfSquares_Scalar(const float* data, size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        sum += data[i] * data[i];
    }
    return sum;
}

float SimdAccelerator::dotProduct_Scalar(const float* a, const float* b, size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

void SimdAccelerator::multiplyAdd_Scalar(float* result, const float* a, const float* b, float c, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        result[i] = a[i] * b[i] + c;
    }
}

void SimdAccelerator::multiply_Scalar(float* result, const float* a, const float* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        result[i] = a[i] * b[i];
    }
}

void SimdAccelerator::add_Scalar(float* result, const float* a, const float* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        result[i] = a[i] + b[i];
    }
}

float SimdAccelerator::absMax_Scalar(const float* data, size_t count) {
    float maxVal = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        float absVal = std::fabs(data[i]);
        if (absVal > maxVal) maxVal = absVal;
    }
    return maxVal;
}

void SimdAccelerator::findMinMax_Scalar(const float* data, size_t count, float& outMin, float& outMax) {
    if (count == 0) {
        outMin = outMax = 0.0f;
        return;
    }
    outMin = outMax = data[0];
    for (size_t i = 1; i < count; ++i) {
        if (data[i] < outMin) outMin = data[i];
        if (data[i] > outMax) outMax = data[i];
    }
}

// ==============================================================================
// AVX 实现 (256-bit)
// ==============================================================================

#ifdef __AVX__
float SimdAccelerator::sumOfSquares_AVX(const float* data, size_t count) {
    float sum = 0.0f;
    size_t i = 0;
    
    // AVX 处理 8 个 float
    __m256 sumVec = _mm256_setzero_ps();
    for (; i + 8 <= count; i += 8) {
        __m256 v = _mm256_loadu_ps(data + i);
        sumVec = _mm256_fmadd_ps(v, v, sumVec);
    }
    
    // 水平求和
    __m128 hi = _mm256_extractf128_ps(sumVec, 1);
    __m128 lo = _mm256_castps256_ps128(sumVec);
    __m128 sum128 = _mm_add_ps(lo, hi);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum = _mm_cvtss_f32(sum128);
    
    // 处理剩余元素
    for (; i < count; ++i) {
        sum += data[i] * data[i];
    }
    
    return sum;
}

float SimdAccelerator::dotProduct_AVX(const float* a, const float* b, size_t count) {
    float sum = 0.0f;
    size_t i = 0;
    
    __m256 sumVec = _mm256_setzero_ps();
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sumVec = _mm256_fmadd_ps(va, vb, sumVec);
    }
    
    __m128 hi = _mm256_extractf128_ps(sumVec, 1);
    __m128 lo = _mm256_castps256_ps128(sumVec);
    __m128 sum128 = _mm_add_ps(lo, hi);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum = _mm_cvtss_f32(sum128);
    
    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }
    
    return sum;
}

void SimdAccelerator::multiplyAdd_AVX(float* result, const float* a, const float* b, float c, size_t count) {
    size_t i = 0;
    __m256 cVec = _mm256_set1_ps(c);
    
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 vr = _mm256_fmadd_ps(va, vb, cVec);
        _mm256_storeu_ps(result + i, vr);
    }
    
    for (; i < count; ++i) {
        result[i] = a[i] * b[i] + c;
    }
}

void SimdAccelerator::multiply_AVX(float* result, const float* a, const float* b, size_t count) {
    size_t i = 0;
    
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 vr = _mm256_mul_ps(va, vb);
        _mm256_storeu_ps(result + i, vr);
    }
    
    for (; i < count; ++i) {
        result[i] = a[i] * b[i];
    }
}

void SimdAccelerator::add_AVX(float* result, const float* a, const float* b, size_t count) {
    size_t i = 0;
    
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 vr = _mm256_add_ps(va, vb);
        _mm256_storeu_ps(result + i, vr);
    }
    
    for (; i < count; ++i) {
        result[i] = a[i] + b[i];
    }
}

float SimdAccelerator::absMax_AVX(const float* data, size_t count) {
    if (count == 0) return 0.0f;
    
    size_t i = 0;
    __m256 maxVec = _mm256_setzero_ps();
    
    for (; i + 8 <= count; i += 8) {
        __m256 v = _mm256_loadu_ps(data + i);
        __m256 absV = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), v);
        maxVec = _mm256_max_ps(maxVec, absV);
    }
    
    __m128 hi = _mm256_extractf128_ps(maxVec, 1);
    __m128 lo = _mm256_castps256_ps128(maxVec);
    __m128 max128 = _mm_max_ps(lo, hi);
    max128 = _mm_max_ps(max128, _mm_shuffle_ps(max128, max128, _MM_SHUFFLE(2, 3, 0, 1)));
    max128 = _mm_max_ps(max128, _mm_shuffle_ps(max128, max128, _MM_SHUFFLE(0, 1, 2, 3)));
    float maxVal = _mm_cvtss_f32(max128);
    
    for (; i < count; ++i) {
        float absVal = std::fabs(data[i]);
        if (absVal > maxVal) maxVal = absVal;
    }
    
    return maxVal;
}

void SimdAccelerator::findMinMax_AVX(const float* data, size_t count, float& outMin, float& outMax) {
    if (count == 0) {
        outMin = outMax = 0.0f;
        return;
    }
    
    size_t i = 0;
    __m256 minVec = _mm256_set1_ps(data[0]);
    __m256 maxVec = _mm256_set1_ps(data[0]);
    
    for (; i + 8 <= count; i += 8) {
        __m256 v = _mm256_loadu_ps(data + i);
        minVec = _mm256_min_ps(minVec, v);
        maxVec = _mm256_max_ps(maxVec, v);
    }
    
    __m128 hiMin = _mm256_extractf128_ps(minVec, 1);
    __m128 loMin = _mm256_castps256_ps128(minVec);
    __m128 min128 = _mm_min_ps(loMin, hiMin);
    min128 = _mm_min_ps(min128, _mm_shuffle_ps(min128, min128, _MM_SHUFFLE(2, 3, 0, 1)));
    min128 = _mm_min_ps(min128, _mm_shuffle_ps(min128, min128, _MM_SHUFFLE(0, 1, 2, 3)));
    outMin = _mm_cvtss_f32(min128);
    
    __m128 hiMax = _mm256_extractf128_ps(maxVec, 1);
    __m128 loMax = _mm256_castps256_ps128(maxVec);
    __m128 max128 = _mm_max_ps(loMax, hiMax);
    max128 = _mm_max_ps(max128, _mm_shuffle_ps(max128, max128, _MM_SHUFFLE(2, 3, 0, 1)));
    max128 = _mm_max_ps(max128, _mm_shuffle_ps(max128, max128, _MM_SHUFFLE(0, 1, 2, 3)));
    outMax = _mm_cvtss_f32(max128);
    
    for (; i < count; ++i) {
        if (data[i] < outMin) outMin = data[i];
        if (data[i] > outMax) outMax = data[i];
    }
}
#else
// 如果没有 AVX 支持，使用标量实现
float SimdAccelerator::sumOfSquares_AVX(const float* data, size_t count) { return sumOfSquares_Scalar(data, count); }
float SimdAccelerator::dotProduct_AVX(const float* a, const float* b, size_t count) { return dotProduct_Scalar(a, b, count); }
void SimdAccelerator::multiplyAdd_AVX(float* result, const float* a, const float* b, float c, size_t count) { multiplyAdd_Scalar(result, a, b, c, count); }
void SimdAccelerator::multiply_AVX(float* result, const float* a, const float* b, size_t count) { multiply_Scalar(result, a, b, count); }
void SimdAccelerator::add_AVX(float* result, const float* a, const float* b, size_t count) { add_Scalar(result, a, b, count); }
float SimdAccelerator::absMax_AVX(const float* data, size_t count) { return absMax_Scalar(data, count); }
void SimdAccelerator::findMinMax_AVX(const float* data, size_t count, float& outMin, float& outMax) { findMinMax_Scalar(data, count, outMin, outMax); }
#endif

// ==============================================================================
// AVX-512 实现 (512-bit)
// ==============================================================================

#ifdef __AVX512F__
float SimdAccelerator::sumOfSquares_AVX512(const float* data, size_t count) {
    float sum = 0.0f;
    size_t i = 0;
    
    __m512 sumVec = _mm512_setzero_ps();
    for (; i + 16 <= count; i += 16) {
        __m512 v = _mm512_loadu_ps(data + i);
        sumVec = _mm512_fmadd_ps(v, v, sumVec);
    }
    
    sum = _mm512_reduce_add_ps(sumVec);
    
    for (; i < count; ++i) {
        sum += data[i] * data[i];
    }
    
    return sum;
}

float SimdAccelerator::dotProduct_AVX512(const float* a, const float* b, size_t count) {
    float sum = 0.0f;
    size_t i = 0;
    
    __m512 sumVec = _mm512_setzero_ps();
    for (; i + 16 <= count; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        sumVec = _mm512_fmadd_ps(va, vb, sumVec);
    }
    
    sum = _mm512_reduce_add_ps(sumVec);
    
    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }
    
    return sum;
}

void SimdAccelerator::multiplyAdd_AVX512(float* result, const float* a, const float* b, float c, size_t count) {
    size_t i = 0;
    __m512 cVec = _mm512_set1_ps(c);
    
    for (; i + 16 <= count; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        __m512 vr = _mm512_fmadd_ps(va, vb, cVec);
        _mm512_storeu_ps(result + i, vr);
    }
    
    for (; i < count; ++i) {
        result[i] = a[i] * b[i] + c;
    }
}

void SimdAccelerator::multiply_AVX512(float* result, const float* a, const float* b, size_t count) {
    size_t i = 0;
    
    for (; i + 16 <= count; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        __m512 vr = _mm512_mul_ps(va, vb);
        _mm512_storeu_ps(result + i, vr);
    }
    
    for (; i < count; ++i) {
        result[i] = a[i] * b[i];
    }
}

void SimdAccelerator::add_AVX512(float* result, const float* a, const float* b, size_t count) {
    size_t i = 0;
    
    for (; i + 16 <= count; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        __m512 vr = _mm512_add_ps(va, vb);
        _mm512_storeu_ps(result + i, vr);
    }
    
    for (; i < count; ++i) {
        result[i] = a[i] + b[i];
    }
}

float SimdAccelerator::absMax_AVX512(const float* data, size_t count) {
    if (count == 0) return 0.0f;
    
    size_t i = 0;
    __m512 maxVec = _mm512_setzero_ps();
    
    for (; i + 16 <= count; i += 16) {
        __m512 v = _mm512_loadu_ps(data + i);
        __m512 absV = _mm512_abs_ps(v);
        maxVec = _mm512_max_ps(maxVec, absV);
    }
    
    float maxVal = _mm512_reduce_max_ps(maxVec);
    
    for (; i < count; ++i) {
        float absVal = std::fabs(data[i]);
        if (absVal > maxVal) maxVal = absVal;
    }
    
    return maxVal;
}

void SimdAccelerator::findMinMax_AVX512(const float* data, size_t count, float& outMin, float& outMax) {
    if (count == 0) {
        outMin = outMax = 0.0f;
        return;
    }
    
    size_t i = 0;
    __m512 minVec = _mm512_set1_ps(data[0]);
    __m512 maxVec = _mm512_set1_ps(data[0]);
    
    for (; i + 16 <= count; i += 16) {
        __m512 v = _mm512_loadu_ps(data + i);
        minVec = _mm512_min_ps(minVec, v);
        maxVec = _mm512_max_ps(maxVec, v);
    }
    
    outMin = _mm512_reduce_min_ps(minVec);
    outMax = _mm512_reduce_max_ps(maxVec);
    
    for (; i < count; ++i) {
        if (data[i] < outMin) outMin = data[i];
        if (data[i] > outMax) outMax = data[i];
    }
}
#else
// 如果没有 AVX-512 支持，使用 AVX 实现
float SimdAccelerator::sumOfSquares_AVX512(const float* data, size_t count) { return sumOfSquares_AVX(data, count); }
float SimdAccelerator::dotProduct_AVX512(const float* a, const float* b, size_t count) { return dotProduct_AVX(a, b, count); }
void SimdAccelerator::multiplyAdd_AVX512(float* result, const float* a, const float* b, float c, size_t count) { multiplyAdd_AVX(result, a, b, c, count); }
void SimdAccelerator::multiply_AVX512(float* result, const float* a, const float* b, size_t count) { multiply_AVX(result, a, b, count); }
void SimdAccelerator::add_AVX512(float* result, const float* a, const float* b, size_t count) { add_AVX(result, a, b, count); }
float SimdAccelerator::absMax_AVX512(const float* data, size_t count) { return absMax_AVX(data, count); }
void SimdAccelerator::findMinMax_AVX512(const float* data, size_t count, float& outMin, float& outMax) { findMinMax_AVX(data, count, outMin, outMax); }
#endif

} // namespace OpenTune

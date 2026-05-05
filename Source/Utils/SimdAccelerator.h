#pragma once
/**
 * SimdAccelerator - 统一 SIMD 向量化数学运算
 *
 * macOS:   Apple Accelerate 框架 (vvlogf, vvexpf, vvsqrtf, vDSP)
 * Windows: 标量回退（编译器 AVX2 自动向量化）
 *
 * 单例模式，线程安全（构造时一次性绑定后端，之后只读）。
 */

#include <cstddef>

namespace OpenTune {

class SimdAccelerator {
public:
    static SimdAccelerator& getInstance();

    // ── 向量化数学运算 ─────────────────────────────
    float dotProduct(const float* a, const float* b, size_t count) const;
    void vectorLog(float* result, const float* input, size_t count) const;
    void vectorExp(float* result, const float* input, size_t count) const;
    void vectorSqrt(float* result, const float* input, size_t count) const;
    void complexMagnitude(float* result, const float* complexData, size_t complexCount) const;

    const char* getBackendName() const { return backendName_; }

private:
    SimdAccelerator();  // 构造时绑定后端

    // ── 函数指针类型 ──
    using DotProductFunc       = float(*)(const float*, const float*, size_t);
    using VectorLogFunc        = void(*)(float*, const float*, size_t);
    using VectorExpFunc        = void(*)(float*, const float*, size_t);
    using VectorSqrtFunc       = void(*)(float*, const float*, size_t);
    using ComplexMagnitudeFunc = void(*)(float*, const float*, size_t);

    // ── 标量回退 ──
    static float dotProduct_Scalar(const float* a, const float* b, size_t count);
    static void vectorLog_Scalar(float* result, const float* input, size_t count);
    static void vectorExp_Scalar(float* result, const float* input, size_t count);
    static void vectorSqrt_Scalar(float* result, const float* input, size_t count);
    static void complexMagnitude_Scalar(float* result, const float* complexData, size_t complexCount);

#if defined(__APPLE__)
    // ── Apple Accelerate ──
    static float dotProduct_Accelerate(const float* a, const float* b, size_t count);
    static void vectorLog_Accelerate(float* result, const float* input, size_t count);
    static void vectorExp_Accelerate(float* result, const float* input, size_t count);
    static void vectorSqrt_Accelerate(float* result, const float* input, size_t count);
    static void complexMagnitude_Accelerate(float* result, const float* complexData, size_t complexCount);
#endif

    // ── 绑定的函数指针（构造后不可变） ──
    DotProductFunc       dotProductFunc_;
    VectorLogFunc        vectorLogFunc_;
    VectorExpFunc        vectorExpFunc_;
    VectorSqrtFunc       vectorSqrtFunc_;
    ComplexMagnitudeFunc complexMagnitudeFunc_;

    const char* backendName_;
};

} // namespace OpenTune

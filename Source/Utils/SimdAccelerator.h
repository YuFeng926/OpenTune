#pragma once

/**
 * SIMD加速器
 * 
 * 提供运行时检测AVX512/AVX2/SSE并自动选择最优实现。
 * 用于CPU密集型计算（如Mel滤波器、RMS计算、峰值检测等）。
 */

#include <cstddef>
#include <string>

namespace OpenTune {

/**
 * SimdAccelerator - SIMD加速器类
 * 
 * 单例模式，在程序启动时检测SIMD支持级别，
 * 并通过函数指针动态分派到最优实现。
 */
class SimdAccelerator {
public:
    /**
     * SimdLevel - SIMD支持级别枚举
     */
    enum class SimdLevel {
        None,       // 无SIMD支持
        SSE2,       // SSE2 (128-bit)
        AVX,        // AVX (256-bit)
        AVX2,       // AVX2 (256-bit 整数 + 浮点)
        AVX512      // AVX-512 (512-bit)
    };

    /**
     * 获取单例实例
     */
    static SimdAccelerator& getInstance();

    /**
     * 检测SIMD支持级别（在程序启动时调用一次）
     */
    void detect();

    /**
     * 获取当前SIMD支持级别
     */
    SimdLevel getSimdLevel() const { return simdLevel_; }

    /**
     * 获取SIMD级别名称
     */
    std::string getSimdLevelName() const;

    /**
     * 获取向量宽度（同时处理的元素数量）
     */
    int getVectorWidth() const;

    // ==============================================================================
    // SIMD 加速函数
    // ==============================================================================

    /**
     * 计算数组的平方和
     * @param data 输入数组
     * @param count 元素数量
     * @return 平方和
     */
    float sumOfSquares(const float* data, size_t count) const;

    /**
     * 计算数组的点积
     * @param a 第一个数组
     * @param b 第二个数组
     * @param count 元素数量
     * @return 点积结果
     */
    float dotProduct(const float* a, const float* b, size_t count) const;

    /**
     * 向量乘加运算：result = a * b + c
     * @param result 结果数组
     * @param a 第一个输入数组
     * @param b 第二个输入数组
     * @param c 标量
     * @param count 元素数量
     */
    void multiplyAdd(float* result, const float* a, const float* b, float c, size_t count) const;

    /**
     * 向量逐元素乘法
     * @param result 结果数组
     * @param a 第一个输入数组
     * @param b 第二个输入数组
     * @param count 元素数量
     */
    void multiply(float* result, const float* a, const float* b, size_t count) const;

    /**
     * 向量逐元素加法
     * @param result 结果数组
     * @param a 第一个输入数组
     * @param b 第二个输入数组
     * @param count 元素数量
     */
    void add(float* result, const float* a, const float* b, size_t count) const;

    /**
     * 计算数组的绝对值最大值
     * @param data 输入数组
     * @param count 元素数量
     * @return 绝对值最大值
     */
    float absMax(const float* data, size_t count) const;

    /**
     * 计算数组的最小值和最大值
     * @param data 输入数组
     * @param count 元素数量
     * @param outMin 输出最小值
     * @param outMax 输出最大值
     */
    void findMinMax(const float* data, size_t count, float& outMin, float& outMax) const;

private:
    SimdAccelerator() = default;
    ~SimdAccelerator() = default;

    // 禁止拷贝
    SimdAccelerator(const SimdAccelerator&) = delete;
    SimdAccelerator& operator=(const SimdAccelerator&) = delete;

    // SIMD实现函数指针类型定义
    using SumOfSquaresFunc = float(*)(const float*, size_t);
    using DotProductFunc = float(*)(const float*, const float*, size_t);
    using MultiplyAddFunc = void(*)(float*, const float*, const float*, float, size_t);
    using MultiplyFunc = void(*)(float*, const float*, const float*, size_t);
    using AddFunc = void(*)(float*, const float*, const float*, size_t);
    using AbsMaxFunc = float(*)(const float*, size_t);
    using FindMinMaxFunc = void(*)(const float*, size_t, float&, float&);

    // 标量实现
    static float sumOfSquares_Scalar(const float* data, size_t count);
    static float dotProduct_Scalar(const float* a, const float* b, size_t count);
    static void multiplyAdd_Scalar(float* result, const float* a, const float* b, float c, size_t count);
    static void multiply_Scalar(float* result, const float* a, const float* b, size_t count);
    static void add_Scalar(float* result, const float* a, const float* b, size_t count);
    static float absMax_Scalar(const float* data, size_t count);
    static void findMinMax_Scalar(const float* data, size_t count, float& outMin, float& outMax);

    // AVX实现
    static float sumOfSquares_AVX(const float* data, size_t count);
    static float dotProduct_AVX(const float* a, const float* b, size_t count);
    static void multiplyAdd_AVX(float* result, const float* a, const float* b, float c, size_t count);
    static void multiply_AVX(float* result, const float* a, const float* b, size_t count);
    static void add_AVX(float* result, const float* a, const float* b, size_t count);
    static float absMax_AVX(const float* data, size_t count);
    static void findMinMax_AVX(const float* data, size_t count, float& outMin, float& outMax);

    // AVX-512实现
    static float sumOfSquares_AVX512(const float* data, size_t count);
    static float dotProduct_AVX512(const float* a, const float* b, size_t count);
    static void multiplyAdd_AVX512(float* result, const float* a, const float* b, float c, size_t count);
    static void multiply_AVX512(float* result, const float* a, const float* b, size_t count);
    static void add_AVX512(float* result, const float* a, const float* b, size_t count);
    static float absMax_AVX512(const float* data, size_t count);
    static void findMinMax_AVX512(const float* data, size_t count, float& outMin, float& outMax);

    // 缓存的检测结果
    bool detected_ = false;
    SimdLevel simdLevel_ = SimdLevel::None;

    // 函数指针
    SumOfSquaresFunc sumOfSquaresFunc_ = sumOfSquares_Scalar;
    DotProductFunc dotProductFunc_ = dotProduct_Scalar;
    MultiplyAddFunc multiplyAddFunc_ = multiplyAdd_Scalar;
    MultiplyFunc multiplyFunc_ = multiply_Scalar;
    AddFunc addFunc_ = add_Scalar;
    AbsMaxFunc absMaxFunc_ = absMax_Scalar;
    FindMinMaxFunc findMinMaxFunc_ = findMinMax_Scalar;
};

} // namespace OpenTune

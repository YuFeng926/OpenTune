#pragma once

#include "../Utils/DetectedKey.h"
#include <array>
#include <vector>

namespace OpenTune {

/**
 * F0KeyDetector - 基于 F0 轨迹直方图的调式检测器（Krumhansl-Schmuckler 24 候选）
 *
 * 将 RMVPE F0 轨迹折叠为 12 维 pitch class 直方图（帧能量加权），
 * 与 12 root × Major/Minor 旋转后的 K-S profile 做余弦相似度匹配。
 * 无状态，detect 为 const，可跨线程共享。
 */
class F0KeyDetector {
public:
    // f0Frequencies: RMVPE 轨迹（Hz），无效帧（非有限或 <=0）跳过
    // energies: 与 f0 同长的 [0,1] RMS 帧能量，可为空（为空则权重 1.0）
    // 返回: origin=Automatic, confidence=bestCosine-secondBestCosine；无有效帧返回 {C,Major,0,Unset}
    DetectedKey detect(const std::vector<float>& f0Frequencies,
                       const std::vector<float>& energies) const;

private:
    static float cosineSimilarity(const std::array<float, 12>& a,
                                  const std::array<float, 12>& b);
    static void rotateProfile(const std::array<float, 12>& base,
                              int semitones,
                              std::array<float, 12>& result);
    static const std::array<float, 12> kKSMajorProfile;
    static const std::array<float, 12> kKSMinorProfile;
};

} // namespace OpenTune

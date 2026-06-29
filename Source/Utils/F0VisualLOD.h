#pragma once

#include <vector>
#include <cstdint>
#include <memory>

namespace OpenTune {

/**
 * F0 Visual LOD (Level of Detail)
 * 
 * 固定 LOD levels：1, 2, 4, 8, 16, 32, 64, 128 frames per point
 * 
 * 每个 level 存储：
 * - voiced runs（连续 voiced 区间）
 * - unvoiced intervals（unvoiced 区间）
 * - corrected voiced runs（如果有 correction）
 */
class F0VisualLOD {
public:
    static constexpr int kNumLevels = 8;
    static constexpr int kFramesPerPoint[kNumLevels] = {1, 2, 4, 8, 16, 32, 64, 128};
    
    struct VoicedRun {
        int startFrame;
        int endFrameExclusive;
        std::vector<float> f0Values;  // 降采样后的 F0 值
        
        bool isValid() const { return endFrameExclusive > startFrame; }
    };
    
    struct UnvoicedInterval {
        int startFrame;
        int endFrameExclusive;
        
        bool isValid() const { return endFrameExclusive > startFrame; }
    };
    
    struct Level {
        int framesPerPoint = 1;
        std::vector<VoicedRun> originalVoicedRuns;
        std::vector<VoicedRun> correctedVoicedRuns;  // 如果有 correction
        std::vector<UnvoicedInterval> unvoicedIntervals;
    };
    
    F0VisualLOD() = default;
    
    // 从原始 F0 数据构建 LOD
    void build(const std::vector<float>& originalF0,
               const std::vector<float>& correctedF0,  // 可以为空
               int hopSize,
               double sampleRate);
    
    // 根据 pixelsPerSecond 选择最佳 level
    const Level& selectBestLevel(double pixelsPerSecond, double sampleRate, int hopSize) const;
    
    bool isEmpty() const { return levels_[0].originalVoicedRuns.empty(); }
    
private:
    Level levels_[kNumLevels];
    
    void buildLevel(int levelIndex, 
                    const std::vector<float>& originalF0,
                    const std::vector<float>& correctedF0);
    
    void extractVoicedRuns(const std::vector<float>& f0,
                           int framesPerPoint,
                           std::vector<VoicedRun>& voicedRuns,
                           std::vector<UnvoicedInterval>& unvoicedIntervals);
};

} // namespace OpenTune

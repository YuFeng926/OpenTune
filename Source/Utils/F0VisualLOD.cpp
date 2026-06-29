#include "F0VisualLOD.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

void F0VisualLOD::build(const std::vector<float>& originalF0,
                        const std::vector<float>& correctedF0,
                        int /*hopSize*/,
                        double /*sampleRate*/) {
    for (int level = 0; level < kNumLevels; ++level) {
        buildLevel(level, originalF0, correctedF0);
    }
}

void F0VisualLOD::buildLevel(int levelIndex,
                             const std::vector<float>& originalF0,
                             const std::vector<float>& correctedF0) {
    const int framesPerPoint = kFramesPerPoint[levelIndex];
    levels_[levelIndex].framesPerPoint = framesPerPoint;
    
    // 提取 original voiced runs
    extractVoicedRuns(originalF0, framesPerPoint,
                      levels_[levelIndex].originalVoicedRuns,
                      levels_[levelIndex].unvoicedIntervals);
    
    // 提取 corrected voiced runs（如果有）
    if (!correctedF0.empty()) {
        std::vector<UnvoicedInterval> dummy;
        extractVoicedRuns(correctedF0, framesPerPoint,
                          levels_[levelIndex].correctedVoicedRuns,
                          dummy);
    }
}

void F0VisualLOD::extractVoicedRuns(const std::vector<float>& f0,
                                    int framesPerPoint,
                                    std::vector<VoicedRun>& voicedRuns,
                                    std::vector<UnvoicedInterval>& unvoicedIntervals) {
    voicedRuns.clear();
    unvoicedIntervals.clear();
    
    const int totalFrames = static_cast<int>(f0.size());
    int runStart = -1;
    int unvoicedStart = -1;
    
    for (int frame = 0; frame < totalFrames; ++frame) {
        const bool isVoiced = f0[frame] > 0.0f;
        
        if (isVoiced) {
            if (runStart < 0) {
                // 结束 unvoiced interval
                if (unvoicedStart >= 0) {
                    unvoicedIntervals.push_back({unvoicedStart, frame});
                    unvoicedStart = -1;
                }
                runStart = frame;
            }
        } else {
            if (runStart >= 0) {
                // 结束 voiced run
                VoicedRun run;
                run.startFrame = runStart;
                run.endFrameExclusive = frame;
                
                // 降采样 F0 值
                for (int f = runStart; f < frame; f += framesPerPoint) {
                    run.f0Values.push_back(f0[f]);
                }
                
                voicedRuns.push_back(std::move(run));
                runStart = -1;
            }
            
            if (unvoicedStart < 0) {
                unvoicedStart = frame;
            }
        }
    }
    
    // 处理结尾
    if (runStart >= 0) {
        VoicedRun run;
        run.startFrame = runStart;
        run.endFrameExclusive = totalFrames;
        
        for (int f = runStart; f < totalFrames; f += framesPerPoint) {
            run.f0Values.push_back(f0[f]);
        }
        
        voicedRuns.push_back(std::move(run));
    }
    
    if (unvoicedStart >= 0) {
        unvoicedIntervals.push_back({unvoicedStart, totalFrames});
    }
}

const F0VisualLOD::Level& F0VisualLOD::selectBestLevel(double pixelsPerSecond, 
                                                        double sampleRate, 
                                                        int hopSize) const {
    // 计算每像素对应的帧数
    const double framesPerSecond = sampleRate / hopSize;
    const double framesPerPixel = framesPerSecond / pixelsPerSecond;
    
    // 选择最接近的 level：当 framesPerPoint <= framesPerPixel * 2 时选择
    int bestLevel = 0;
    for (int level = 0; level < kNumLevels; ++level) {
        if (kFramesPerPoint[level] <= framesPerPixel * 2.0) {
            bestLevel = level;
        }
    }
    
    return levels_[bestLevel];
}

} // namespace OpenTune

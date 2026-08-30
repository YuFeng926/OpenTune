#pragma once

#include "IF0Extractor.h"
#include "../Utils/Error.h"
#include "../Utils/VocoderModelWeight.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>

namespace OpenTune {

class ResamplingManager;

/** @brief 可用声码器权重信息（下拉框列表项）。 */
struct VocoderWeightInfo {
    std::string fileName;      // 权重文件名（含 .onnx），即持久化标识
    std::string displayName;   // 显示名（去扩展名）
};

class ModelFactory {
public:
    using F0ExtractorResult = Result<std::unique_ptr<IF0Extractor>>;

    static F0ExtractorResult createF0Extractor(
        F0ModelType type,
        const std::string& modelDir,
        Ort::Env& env,
        std::shared_ptr<ResamplingManager> resampler
    );

    static std::string getModelPath(F0ModelType type, const std::string& modelDir);

    static bool isModelAvailable(F0ModelType type, const std::string& modelDir);

    static std::vector<F0ModelInfo> getAvailableF0Models(const std::string& modelDir);

    /** @brief 扫描可用声码器权重：内置（models/ 根目录）+ vocoder_weights/*.onnx，按文件名排序。 */
    static std::vector<VocoderWeightInfo> getAvailableVocoderWeights(const std::string& modelDir);

    static Ort::SessionOptions createF0SessionOptions(
        F0ModelType type,
        bool& outGpuMode,
        bool forceCpu = false);

private:
    static std::unique_ptr<Ort::Session> loadF0Session(
        const std::string& modelPath,
        Ort::Env& env,
        F0ModelType type,
        bool& outGpuMode
    );
};

} // namespace OpenTune

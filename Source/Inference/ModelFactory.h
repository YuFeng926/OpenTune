#pragma once

#include "IF0Extractor.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/Error.h"
#include <onnxruntime_cxx_api.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

class ResamplingManager;

class ModelFactory {
public:
    using F0ExtractorResult = Result<std::unique_ptr<IF0Extractor>>;

    static F0ExtractorResult createF0Extractor(
        F0ModelType type,
        const std::string& modelPath,
        Ort::Env& env,
        std::shared_ptr<ResamplingManager> resampler,
        AccelerationDetector::BackendSelection backend);

    static std::string getModelPath(F0ModelType type, const std::string& modelDir);
    static std::optional<std::array<uint8_t, 32>> getModelWeightDigest(const std::string& modelPath);

    static bool isModelAvailable(F0ModelType type, const std::string& modelDir);

    static std::vector<F0ModelInfo> getAvailableF0Models(const std::string& modelDir);

private:
    static Ort::SessionOptions createF0SessionOptions(
        F0ModelType type,
        bool& outGpuMode,
        AccelerationDetector::BackendSelection backend);

    static std::unique_ptr<Ort::Session> loadF0Session(
        const std::string& modelPath,
        Ort::Env& env,
        F0ModelType type,
        bool& outGpuMode,
        AccelerationDetector::BackendSelection backend);
};

} // namespace OpenTune

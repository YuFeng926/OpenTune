#pragma once

#include "IF0Extractor.h"
#include "../Utils/Error.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>

namespace OpenTune {

class ResamplingManager;

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

#pragma once

#include "IF0Extractor.h"
#include "IVadExtractor.h"   // ⚡️ vocal-time-stretch §4.4
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
    using VadExtractorResult = Result<std::unique_ptr<IVadExtractor>>;

    static F0ExtractorResult createF0Extractor(
        F0ModelType type,
        const std::string& modelDir,
        Ort::Env& env,
        std::shared_ptr<ResamplingManager> resampler
    );

    /**
     * ⚡️ vocal-time-stretch §4.4 — VAD extractor factory.
     *
     * Currently supports SileroV5 only.  Returns OK with a usable extractor
     * when the ONNX model is found and loads; returns a soft failure (with
     * `outAvailable=false` semantics) when the model is missing.  Callers
     * (PhonemeClassifier path) are expected to degrade gracefully to
     * RMVPE-only V/U fusion.
     */
    static VadExtractorResult createVadExtractor(
        VadModelType type,
        const std::string& modelDir
    );

    static std::string getModelPath(F0ModelType type, const std::string& modelDir);
    static std::string getVadModelPath(VadModelType type, const std::string& modelDir);

    static bool isModelAvailable(F0ModelType type, const std::string& modelDir);
    static bool isVadModelAvailable(VadModelType type, const std::string& modelDir);

    static std::vector<F0ModelInfo>  getAvailableF0Models(const std::string& modelDir);
    static std::vector<VadModelInfo> getAvailableVadModels(const std::string& modelDir);

    static Ort::SessionOptions createF0SessionOptions(bool& outGpuMode);
private:
    static std::unique_ptr<Ort::Session> loadF0Session(
        const std::string& modelPath,
        Ort::Env& env,
        bool& outGpuMode
    );
};

} // namespace OpenTune

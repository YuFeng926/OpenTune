#pragma once

#if defined(__APPLE__)

#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace OpenTune {

/**
    Registers the CoreML execution provider on the given session options.

    ONNX Runtime >= 1.20 recognises "CoreML" in the generic provider-options
    API (which also accepts ModelFormat/MLComputeUnits). The macOS-pinned
    ORT 1.19.2 does not know that name there, so fall back to the flag-based
    C API (MLProgram). Throws std::runtime_error when both paths fail so
    callers keep their existing CPU fallback handling.
*/
inline void appendCoreMlExecutionProvider(Ort::SessionOptions& sessionOptions)
{
    std::unordered_map<std::string, std::string> coremlOptions;
    coremlOptions["ModelFormat"] = "MLProgram";
    coremlOptions["MLComputeUnits"] = "CPUAndGPU";

    try
    {
        sessionOptions.AppendExecutionProvider("CoreML", coremlOptions);
        return;
    }
    catch (const Ort::Exception&)
    {
        // ORT 1.19.x: "CoreML" is unknown to the provider-options API.
    }

    const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_CoreML(
        sessionOptions, COREML_FLAG_CREATE_MLPROGRAM);
    if (status != nullptr)
    {
        const std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error(message);
    }
}

} // namespace OpenTune

#endif // __APPLE__

#pragma once

#if defined(__APPLE__)

#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>

#include <stdexcept>
#include <string>

namespace OpenTune {

/**
    macOS 12 uses the older Core ML NeuralNetwork model format. It is supported
    by the macOS 12 Core ML runtime and avoids making the first-generation
    MLProgram path a deployment requirement for the oldest supported system.
    Newer systems use MLProgram and all available Core ML compute units so that
    Apple Neural Engine devices (including M-series Macs) are not limited to
    CPU/GPU.
*/
inline bool runningOnMacOs12()
{
#if defined(__APPLE__)
    // The application deployment target is macOS 12.0, so "not macOS 13"
    // precisely identifies the macOS 12 compatibility path.
    if (__builtin_available(macOS 13.0, *))
        return false;
    return true;
#else
    return false;
#endif
}

inline const char* coreMlProviderDescription()
{
    return runningOnMacOs12() ? "NeuralNetwork/macOS 12" : "MLProgram/macOS 13+";
}

/**
    Registers the CoreML execution provider on the given session options.

    The public CoreML API in the ORT 1.20.x package is the flag-based C API.
    The default flag set selects the NeuralNetwork format and all available
    compute units; MLProgram is explicitly enabled only on macOS 13+.
*/
inline void appendCoreMlExecutionProvider(Ort::SessionOptions& sessionOptions)
{
    const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    const uint32_t coremlFlags = runningOnMacOs12()
        ? COREML_FLAG_USE_NONE
        : COREML_FLAG_CREATE_MLPROGRAM;
    OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_CoreML(
        sessionOptions, coremlFlags);
    if (status != nullptr)
    {
        const std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error(message);
    }
}

} // namespace OpenTune

#endif // __APPLE__

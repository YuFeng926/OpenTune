#pragma once

#include "VocoderInterface.h"
#include "DmlConfig.h"
#include "../Utils/Error.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>

namespace OpenTune {

enum class VocoderBackend {
    CPU,
    DML
};

struct VocoderCreationResult {
    std::unique_ptr<VocoderInterface> vocoder;
    VocoderBackend backend;
    std::string errorMessage;
    
    static VocoderCreationResult success(std::unique_ptr<VocoderInterface> v, VocoderBackend b) {
        return { std::move(v), b, "" };
    }
    
    static VocoderCreationResult failure(VocoderBackend b, const std::string& err) {
        return { nullptr, b, err };
    }
    
    bool success() const { return vocoder != nullptr; }
};

class VocoderFactory {
public:
    static VocoderCreationResult create(
        const std::string& modelPath,
        Ort::Env& env
    );
    
private:
    VocoderFactory() = delete;
};

} // namespace OpenTune

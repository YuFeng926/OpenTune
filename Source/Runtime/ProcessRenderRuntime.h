#pragma once

#include "../Content/EditableContentSnapshot.h"
#include "../Render/ContentRenderService.h"
#include "../Utils/VocoderModelWeight.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Ort { struct Env; }

namespace OpenTune {

class VocoderDomain;

class ProcessRenderRuntime
{
public:
    struct CompletionContext
    {
        std::shared_ptr<std::atomic<bool>> alive;
        std::function<void(ContentKey, uint64_t)> chunkPublished;
    };

    static ProcessRenderRuntime& getInstance();

    void processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                               RenderJob& job,
                               std::shared_ptr<const EditableContentSnapshot> contentSnap,
                               bool lightPitchEnabled,
                               CompletionContext completion);

    bool setVocoderModelWeight(VocoderModelWeight weight);
    void resetVocoder();
    VocoderDomain* getVocoderDomain() const noexcept { return vocoderDomain_.get(); }

private:
    ProcessRenderRuntime() = default;
    ~ProcessRenderRuntime();

    bool ensureVocoderReady();
    static std::string modelPathForWeight(const std::string& modelDir, VocoderModelWeight weight);

    std::shared_ptr<Ort::Env> ortEnv_;
    std::unique_ptr<VocoderDomain> vocoderDomain_;
    VocoderModelWeight currentVocoderModelWeight_{VocoderModelWeight::Community};
    std::mutex vocoderMutex_;

    ProcessRenderRuntime(const ProcessRenderRuntime&) = delete;
    ProcessRenderRuntime& operator=(const ProcessRenderRuntime&) = delete;
};

} // namespace OpenTune

#pragma once

#include "../Content/EditableContentSnapshot.h"
#include "../Inference/VocoderDomain.h"
#include "../Render/ContentRenderService.h"
#include "../Utils/VocoderModelWeight.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace OpenTune {

class ProcessRenderRuntime
{
public:
    struct CompletionGate
    {
        std::mutex mutex;
        bool closed{false};
    };

    struct CompletionContext
    {
        std::shared_ptr<CompletionGate> gate;
        std::function<void(ContentKey)> chunkSettled;
    };

    static ProcessRenderRuntime& getInstance();

    // 客户端租约：Processor / ARA DocumentController 构造时 attach、析构时 detach。
    // 最后一个客户端 detach 在同一临界区内销毁 vocoder domain（正常析构上下文，
    // join 安全），避免静态析构（DLL detach 持 loader lock）时 join 工作线程。
    void attach();
    void detach();

    void processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                               RenderJob& job,
                               std::shared_ptr<const EditableContentSnapshot> contentSnap,
                               bool lightPitchEnabled,
                               CompletionContext completion);

    bool setVocoderModelWeight(VocoderModelWeight weight);
    void resetVocoder();

    /**
     * Vocoder submission / query entry points. All accesses to the underlying
     * VocoderDomain go through these (each locks vocoderMutex_), so a
     * concurrent resetVocoder()/setVocoderModelWeight() can never destroy the
     * domain while it is being used (no raw pointer escapes).
     *
     * submitVocoderJob requires expectedGeneration to match the current domain
     * generation: the caller captures the full configuration
     * (generation/hop/melBins/fMax) in a single locked snapshot via
     * acquireVocoderConfig(), so a job is never submitted to a domain rebuilt
     * since then with stale configuration.
     */
    bool submitVocoderJob(VocoderDomain::Job job, uint64_t expectedGeneration);
    bool isVocoderReady() const noexcept;

private:
    ProcessRenderRuntime() = default;
    ~ProcessRenderRuntime() = default;

    static std::string modelPathForWeight(const std::string& modelDir, VocoderModelWeight weight);

    // 锁前置条件：调用方已持有 vocoderMutex_。销毁 domain（其析构已调用 shutdown）
    // 并推进 generation，使旧配置快照失效。
    void resetVocoderLocked();

    // 一次锁内“确保 domain 并返回 generation/hop/melBins/fMax”：配置与 domain
    // 同代生成，杜绝跨域混用（旧 hop 配置配新 generation 等）。
    struct VocoderConfig
    {
        uint64_t generation{0};
        int hopSize{0};
        int melBins{0};
        float fMax{16000.0f};
    };
    bool acquireVocoderConfig(VocoderConfig& out);

    std::unique_ptr<VocoderDomain> vocoderDomain_;
    VocoderModelWeight currentVocoderModelWeight_{VocoderModelWeight::Community};
    mutable std::mutex vocoderMutex_;
    int clientCount_{0};      // vocoderMutex_ 保护：客户端租约计数
    uint64_t vocoderGeneration_{0};  // vocoderMutex_ 保护

    ProcessRenderRuntime(const ProcessRenderRuntime&) = delete;
    ProcessRenderRuntime& operator=(const ProcessRenderRuntime&) = delete;
};

} // namespace OpenTune

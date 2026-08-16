#pragma once

#include "../Content/EditableContentSnapshot.h"
#include "../Inference/VocoderDomain.h"
#include "../Render/ContentRenderService.h"
#include "../Utils/VocoderModelWeight.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    // 运行时是进程寿命的（VST3 构建模块被 pin，Standalone 跟随进程）：detach()
    // 只递减计数，最后一个客户端也不销毁 vocoder domain，后续实例复用同一
    // domain 与 generation，避免重建 ORT Session。显式重置只经由
    // setVocoderModelWeight() / resetVocoder() / resetInferenceBackend()。
    void attach();
    void detach();

    void processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                               RenderJob& job,
                               std::shared_ptr<const EditableContentSnapshot> contentSnap,
                               bool lightPitchEnabled,
                               CompletionContext completion);

    /**
     * 异步模型切换 / 后端重置 API（UI 线程调用）：
     * 只投递命令到进程寿命 control worker 并立即返回。control worker 串行执行
     * 耗时 Session 销毁、按当前配置重建、AccelerationDetector reset/detect。
     * completion 经 MessageManager::callAsync 投递回消息线程；调用方在其
     * completion gate 关闭后直接丢弃（不访问 owner）。
     */
    void setVocoderModelWeight(VocoderModelWeight weight, std::function<void()> completion);
    void resetVocoder(std::function<void()> completion);
    void resetInferenceBackend(bool forceCpu, std::function<void()> completion);

    /**
     * Vocoder submission / query entry points. All accesses to the underlying
     * VocoderDomain go through these (each locks vocoderMutex_), so a
     * concurrent control-worker reset/rebuild can never destroy the domain
     * while it is being used (no raw pointer escapes).
     *
     * submitVocoderJob requires expectedGeneration to match the current domain
     * generation: the caller captures the full configuration
     * (generation/melBins/fMax) in a single locked snapshot via
     * acquireVocoderConfig(), so a job is never submitted to a domain rebuilt
     * since then with stale configuration.
     */
    bool submitVocoderJob(VocoderDomain::Job job, uint64_t expectedGeneration);
    bool isVocoderReady() const noexcept;

private:
    // 进程寿命 heap 单例（getInstance 显式 new、永不析构）：control worker
    // 与命令队列随进程存活，绝不在实例卸载路径 join 或销毁。
    ProcessRenderRuntime();
    ~ProcessRenderRuntime() = default;

    static std::string modelPathForWeight(const std::string& modelDir, VocoderModelWeight weight);

    // 在锁外创建并初始化完整 domain。模型加载、Session 创建和失败清理均不得
    // 持有 vocoderMutex_，保证 UI 查询与实例 detach 永远只经历短临界区。
    std::unique_ptr<VocoderDomain> createVocoderDomain(VocoderModelWeight weight);

    // 一次锁内“确保 domain 并返回 generation/melBins/fMax”：配置与 domain
    // 同代生成，杜绝跨域混用（旧 generation 配置配新 domain 等）。
    struct VocoderConfig
    {
        uint64_t generation{0};
        int melBins{0};
        float fMax{16000.0f};
    };
    bool acquireVocoderConfig(VocoderConfig& out);

    // ---- 进程寿命 control worker：模型切换/后端重置的唯一执行者 ----
    struct ControlCommand
    {
        enum class Type : uint8_t
        {
            SetVocoderWeight,
            ResetVocoder,
            ResetInferenceBackend
        };

        Type type{Type::ResetVocoder};
        VocoderModelWeight weight{VocoderModelWeight::Community};
        bool forceCpu{false};
        std::function<void()> completion;
    };

    // control worker 唯一重配入口：短锁内摘除旧 domain 并推进 generation，
    // 锁外销毁旧 Session，随后锁外创建新 Session，最后短锁发布。
    void reconfigureVocoder(const ControlCommand& command);

    void controlWorkerLoop();
    void postControlCommand(ControlCommand command);

    std::thread controlWorker_;
    std::mutex controlMutex_;
    std::condition_variable controlCv_;
    std::deque<ControlCommand> controlQueue_;

    std::unique_ptr<VocoderDomain> vocoderDomain_;
    VocoderModelWeight currentVocoderModelWeight_{VocoderModelWeight::Community};
    mutable std::mutex vocoderMutex_;
    std::condition_variable vocoderStateCv_;
    bool vocoderReconfiguring_{false}; // vocoderMutex_ 保护：control worker 已摘除旧 domain
    bool vocoderInitializing_{false};  // vocoderMutex_ 保护：RenderWorker 正在锁外首次创建
    int clientCount_{0};      // vocoderMutex_ 保护：客户端租约计数（仅计数，不触发释放）
    uint64_t vocoderGeneration_{0};  // vocoderMutex_ 保护

    ProcessRenderRuntime(const ProcessRenderRuntime&) = delete;
    ProcessRenderRuntime& operator=(const ProcessRenderRuntime&) = delete;
};

} // namespace OpenTune

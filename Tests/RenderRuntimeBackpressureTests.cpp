#include "../Source/Inference/VocoderRenderScheduler.h"
#include "../Source/Inference/VocoderInferenceService.h"
#include <onnxruntime_cxx_api.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

int main()
{
    const juce::File model = juce::File(OPENTUNE_HIFIGAN_MODEL_PATH);
    if (!model.existsAsFile())
    {
        std::fprintf(stderr, "{\"event\":\"backpressure_boundary\",\"result\":\"failure\","
                     "\"reason\":\"safe_vocoder_model_unavailable\",\"model\":\"%s\"}\n",
                     model.getFullPathName().toRawUTF8());
        return 1;
    }

    auto env = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTuneBackpressureTest");
    OpenTune::VocoderInferenceService service(env);
    OpenTune::VocoderRenderScheduler scheduler;
    if (!scheduler.initialize(&service))
    {
        std::fprintf(stderr, "{\"event\":\"backpressure_boundary\",\"result\":\"failure\",\"reason\":\"worker_start\"}\n");
        return 1;
    }

    if (!service.initialize(model.getFullPathName().toStdString()))
    {
        scheduler.shutdown();
        std::fprintf(stderr,
                     "{\"event\":\"backpressure_boundary\",\"result\":\"failure\","
                     "\"reason\":\"safe_vocoder_model_unavailable\",\"model\":\"%s\"}\n",
                     model.getFullPathName().toRawUTF8());
        return 1;
    }

    std::atomic<int> completions{0};
    for (int i = 0; i < OpenTune::VocoderRenderScheduler::kMaxQueueDepth + 2; ++i)
    {
        OpenTune::VocoderRenderScheduler::Job job;
        job.f0 = {440.0f, 440.0f};
        job.uv = {1.0f, 1.0f};
        job.conditioning.assign(static_cast<size_t>(service.getConditioningBins()) * job.f0.size(), 0.0f);
        job.onComplete = [&completions](auto, const juce::String&, const std::vector<float>&) {
            ++completions;
        };
        if (!scheduler.submit(std::move(job)))
        {
            std::fprintf(stderr, "{\"event\":\"backpressure_boundary\",\"result\":\"failure\",\"reason\":\"submit\"}\n");
            scheduler.shutdown();
            return 1;
        }
    }
    scheduler.shutdown();
    if (completions.load() != OpenTune::VocoderRenderScheduler::kMaxQueueDepth + 2)
    {
        std::fprintf(stderr, "{\"event\":\"backpressure_boundary\",\"result\":\"failure\",\"reason\":\"completion_accounting\"}\n");
        return 1;
    }
    std::printf("{\"event\":\"backpressure_boundary\",\"result\":\"pass\",\"completions\":%d}\n", completions.load());
    return 0;
}

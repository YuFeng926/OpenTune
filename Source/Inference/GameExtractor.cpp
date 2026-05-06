#include "GameExtractor.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/CpuBudgetManager.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif


namespace OpenTune {

// ==============================================================================
// Construction / Destruction
// ==============================================================================

GameExtractor::GameExtractor(Ort::Env& env)
    : env_(env)
{
    memoryInfo_ = std::make_unique<Ort::MemoryInfo>(
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault)
    );
}

GameExtractor::~GameExtractor() = default;

// ==============================================================================
// Session Management
// ==============================================================================

Ort::SessionOptions GameExtractor::createSessionOptions()
{
    Ort::SessionOptions sessionOptions;

    sessionOptions.DisableMemPattern();
    sessionOptions.DisableCpuMemArena();

    bool gpuMode = false;

#ifdef _WIN32
    auto& gpu = AccelerationDetector::getInstance();
    if (gpu.getSelectedBackend() == AccelerationDetector::AccelBackend::DirectML)
    {
        const int adapterIndex = gpu.getDirectMLDeviceId();
        auto& api = Ort::GetApi();
        const OrtDmlApi* dmlApi = nullptr;
        OrtStatus* probeStatus = api.GetExecutionProviderApi(
            "DML", ORT_API_VERSION,
            reinterpret_cast<const void**>(&dmlApi));
        if (probeStatus != nullptr) {
            const char* msg = api.GetErrorMessage(probeStatus);
            AppLogger::warn("[GameExtractor] DML API not available: " + juce::String(msg != nullptr ? msg : "(no message)") + " — falling back to CPU");
            api.ReleaseStatus(probeStatus);
        } else if (dmlApi != nullptr) {
            OrtStatus* dmlStatus = dmlApi->SessionOptionsAppendExecutionProvider_DML(
                sessionOptions, adapterIndex);
            if (dmlStatus != nullptr) {
                const char* msg = api.GetErrorMessage(dmlStatus);
                AppLogger::warn("[GameExtractor] DML EP append failed: " + juce::String(msg != nullptr ? msg : "(no message)") + " — falling back to CPU");
                api.ReleaseStatus(dmlStatus);
            } else {
                gpuMode = true;
                AppLogger::info("[GameExtractor] DML EP added (adapterIndex=" + juce::String(adapterIndex) + ")");
            }
        }
    }
#endif

#if defined(__APPLE__)
    try {
        std::unordered_map<std::string, std::string> coremlOptions;
        coremlOptions["ModelFormat"] = "MLProgram";
        coremlOptions["MLComputeUnits"] = "CPUAndGPU";
        sessionOptions.AppendExecutionProvider("CoreML", coremlOptions);
        gpuMode = true;
        AppLogger::info("[GameExtractor] CoreML EP added (macOS)");
    } catch (const Ort::Exception& e) {
        AppLogger::warn("[GameExtractor] Failed to add CoreML EP: " + juce::String(e.what()));
    } catch (const std::exception& e) {
        AppLogger::warn("[GameExtractor] Failed to add CoreML EP: " + juce::String(e.what()));
    } catch (...) {
        AppLogger::warn("[GameExtractor] Failed to add CoreML EP (unknown error)");
    }
#endif

    const auto budget = CpuBudgetManager::buildConfig(gpuMode);
    sessionOptions.SetIntraOpNumThreads(budget.onnxIntra);
    sessionOptions.SetInterOpNumThreads(budget.onnxInter);
    sessionOptions.SetExecutionMode(budget.onnxSequential ? ExecutionMode::ORT_SEQUENTIAL : ExecutionMode::ORT_PARALLEL);
    sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", budget.allowSpinning ? "1" : "0");
    sessionOptions.AddConfigEntry("session.inter_op.allow_spinning", budget.allowSpinning ? "1" : "0");
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    return sessionOptions;
}

static std::unique_ptr<Ort::Session> loadOneSession(
    Ort::Env& env,
    const std::string& modelPath,
    Ort::SessionOptions& sessionOptions,
    const char* modelLabel)
{
#ifdef _WIN32
    juce::File modelFile(modelPath);
    std::wstring wModelPath = modelFile.getFullPathName().toWideCharPointer();
    auto session = std::make_unique<Ort::Session>(env, wModelPath.c_str(), sessionOptions);
#else
    auto session = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif
    AppLogger::info("[GameExtractor] Loaded " + juce::String(modelLabel) + " session: " + juce::String(modelPath));
    return session;
}

bool GameExtractor::loadSessions(const std::string& modelDir)
{
    loadConfig(modelDir);
    try {
        auto sessionOptions = createSessionOptions();

        encoderSession_    = loadOneSession(env_, modelDir + "/encoder.onnx",    sessionOptions, "encoder");
        segmenterSession_  = loadOneSession(env_, modelDir + "/segmenter.onnx",  sessionOptions, "segmenter");
        estimatorSession_  = loadOneSession(env_, modelDir + "/estimator.onnx",  sessionOptions, "estimator");
        dur2bdSession_     = loadOneSession(env_, modelDir + "/dur2bd.onnx",     sessionOptions, "dur2bd");
        bd2durSession_     = loadOneSession(env_, modelDir + "/bd2dur.onnx",     sessionOptions, "bd2dur");

        AppLogger::info("[GameExtractor] All 5 GAME sessions loaded successfully");
        return true;

    } catch (const Ort::Exception& e) {
        AppLogger::error("[GameExtractor] ONNX error loading sessions: " + juce::String(e.what()));
        releaseSessions();
        return false;
    } catch (const std::exception& e) {
        AppLogger::error("[GameExtractor] Error loading sessions: " + juce::String(e.what()));
        releaseSessions();
        return false;
    } catch (...) {
        AppLogger::error("[GameExtractor] Unknown error loading sessions");
        releaseSessions();
        return false;
    }
}

void GameExtractor::releaseSessions()
{
    encoderSession_.reset();
    segmenterSession_.reset();
    estimatorSession_.reset();
    dur2bdSession_.reset();
    bd2durSession_.reset();
    AppLogger::info("[GameExtractor] All GAME sessions released");
}

bool GameExtractor::loadConfig(const std::string& modelDir)
{
    juce::File configFile(juce::String(modelDir) + "/config.json");
    if (!configFile.existsAsFile()) return false;

    auto content = configFile.loadFileAsString();
    auto json = juce::JSON::parse(content);
    if (!json.isObject()) return false;

    auto languages = json.getProperty("languages", {});
    if (languages.isObject()) {
        auto* obj = languages.getDynamicObject();
        if (obj != nullptr) {
            for (const auto& prop : obj->getProperties()) {
                languageMap_[prop.name.toString().toStdString()] = static_cast<int64_t>(prop.value);
            }
        }
    }

    AppLogger::info("[GameExtractor] Loaded config.json: " + juce::String(static_cast<int>(languageMap_.size())) + " languages");
    return true;
}

int64_t GameExtractor::getLanguageId(const std::string& languageName) const
{
    auto it = languageMap_.find(languageName);
    return (it != languageMap_.end()) ? it->second : 0;
}

bool GameExtractor::isLoaded() const
{
    return encoderSession_ && segmenterSession_ && estimatorSession_
        && dur2bdSession_ && bd2durSession_;
}

// ==============================================================================
// Helper: convert vector<bool> to uint8_t buffer for ORT bool tensors
// ==============================================================================

namespace {

std::vector<uint8_t> boolVecToU8(const std::vector<bool>& v)
{
    std::vector<uint8_t> u8(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        u8[i] = v[i] ? 1 : 0;
    }
    return u8;
}

} // anonymous namespace

// ==============================================================================
// Pipeline Step 1: Encoder
// ==============================================================================

GameExtractor::EncoderOutput GameExtractor::runEncoder(const float* audio, size_t numSamples)
{
    EncoderOutput result;

    const int64_t L = static_cast<int64_t>(numSamples);

    // Input: waveform float32[1, L]
    std::vector<int64_t> waveShape = {1, L};
    auto waveTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_,
        const_cast<float*>(audio),
        numSamples,
        waveShape.data(),
        waveShape.size()
    );

    // Input: duration float32[1]
    float durationSec = static_cast<float>(numSamples) / static_cast<float>(kSampleRate);
    std::vector<int64_t> durShape = {1};
    auto durTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_,
        &durationSec,
        1,
        durShape.data(),
        durShape.size()
    );

    const char* inputNames[]  = {"waveform", "duration"};
    const char* outputNames[] = {"x_seg", "x_est", "maskT"};

    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(waveTensor));
    inputTensors.push_back(std::move(durTensor));

    auto outputTensors = encoderSession_->Run(
        Ort::RunOptions{nullptr},
        inputNames, inputTensors.data(), 2,
        outputNames, 3
    );

    // Extract shapes — x_seg shape is [1, T, C]
    const auto xSegShape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();
    result.T = xSegShape[1];
    result.C = xSegShape[2];

    const size_t tcCount = static_cast<size_t>(result.T * result.C);

    // Copy x_seg [T, C]
    const float* xSegData = outputTensors[0].GetTensorData<float>();
    result.x_seg.assign(xSegData, xSegData + tcCount);

    // Copy x_est [T, C]
    const float* xEstData = outputTensors[1].GetTensorData<float>();
    result.x_est.assign(xEstData, xEstData + tcCount);

    // Copy maskT [T] — ORT bool stored as uint8_t (1 byte per element)
    const uint8_t* maskTData = outputTensors[2].GetTensorData<uint8_t>();
    result.maskT.resize(static_cast<size_t>(result.T));
    for (int64_t i = 0; i < result.T; ++i) {
        result.maskT[static_cast<size_t>(i)] = (maskTData[i] != 0);
    }

    AppLogger::debug("[GameExtractor] Encoder output: T=" + juce::String(result.T)
        + " C=" + juce::String(result.C));

    return result;
}

// ==============================================================================
// Pipeline Step 2: Dur2Bd (durations → boundaries)
// ==============================================================================

std::vector<bool> GameExtractor::runDur2Bd(
    const std::vector<float>& durations,
    const std::vector<bool>& maskT,
    int64_t T)
{
    const int64_t N = static_cast<int64_t>(durations.size());

    // Input: durations float32[1, N]
    std::vector<int64_t> durShape = {1, N};
    auto durTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_,
        const_cast<float*>(durations.data()),
        static_cast<size_t>(N),
        durShape.data(),
        durShape.size()
    );

    // Input: maskT bool[1, T]
    std::vector<uint8_t> maskTU8 = boolVecToU8(maskT);
    std::vector<int64_t> maskTShape = {1, T};
    auto maskTTensor = Ort::Value::CreateTensor(
        *memoryInfo_,
        maskTU8.data(),
        maskTU8.size(),
        maskTShape.data(),
        maskTShape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
    );

    const char* inputNames[]  = {"durations", "maskT"};
    const char* outputNames[] = {"boundaries"};

    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(durTensor));
    inputTensors.push_back(std::move(maskTTensor));

    auto outputTensors = dur2bdSession_->Run(
        Ort::RunOptions{nullptr},
        inputNames, inputTensors.data(), 2,
        outputNames, 1
    );

    const uint8_t* boundariesData = outputTensors[0].GetTensorData<uint8_t>();
    std::vector<bool> boundaries(static_cast<size_t>(T));
    for (int64_t i = 0; i < T; ++i) {
        boundaries[static_cast<size_t>(i)] = (boundariesData[i] != 0);
    }

    return boundaries;
}

// ==============================================================================
// Pipeline Step 3: Segmenter (D3PM iterative refinement)
// ==============================================================================

std::vector<bool> GameExtractor::runSegmenter(
    const std::vector<float>& x_seg, int64_t T, int64_t C,
    const std::vector<bool>& knownBoundaries,
    const std::vector<bool>& maskT,
    const GameConfig& config)
{
    const int nsteps = config.d3pmSteps;
    const float fThreshold = config.boundaryThreshold;
    const int64_t iRadius = static_cast<int64_t>(config.boundaryRadius);
    const int64_t iLanguage = static_cast<int64_t>(config.language);

    const int64_t tcCount = T * C;
    std::vector<bool> boundaries = knownBoundaries;

    // Pre-compute constant tensor data buffers
    std::vector<int64_t> xSegShape = {1, T, C};
    std::vector<int64_t> bdShape = {1, T};
    std::vector<int64_t> scalarShape = {1};

    const std::vector<uint8_t> knownU8 = boolVecToU8(knownBoundaries);
    const std::vector<uint8_t> maskU8  = boolVecToU8(maskT);

    const char* inputNames[]  = {"x_seg", "language", "known_boundaries", "prev_boundaries", "t", "maskT", "threshold", "radius"};
    const char* outputNames[] = {"boundaries"};

    for (int step = 0; step < nsteps; ++step)
    {
        const float tVal = static_cast<float>(step) / static_cast<float>(nsteps);

        // Build fresh input tensors each iteration
        auto xSegTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_,
            const_cast<float*>(x_seg.data()),
            static_cast<size_t>(tcCount),
            xSegShape.data(),
            xSegShape.size()
        );

        auto langTensor = Ort::Value::CreateTensor<int64_t>(
            *memoryInfo_,
            const_cast<int64_t*>(&iLanguage),
            1,
            scalarShape.data(),
            scalarShape.size()
        );

        auto knownTensor = Ort::Value::CreateTensor(
            *memoryInfo_,
            const_cast<uint8_t*>(knownU8.data()),
            knownU8.size(),
            bdShape.data(),
            bdShape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
        );

        std::vector<uint8_t> prevU8 = boolVecToU8(boundaries);
        auto prevTensor = Ort::Value::CreateTensor(
            *memoryInfo_,
            prevU8.data(),
            prevU8.size(),
            bdShape.data(),
            bdShape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
        );

        auto tTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_,
            const_cast<float*>(&tVal),
            1,
            scalarShape.data(),
            scalarShape.size()
        );

        auto maskTTensor = Ort::Value::CreateTensor(
            *memoryInfo_,
            const_cast<uint8_t*>(maskU8.data()),
            maskU8.size(),
            bdShape.data(),
            bdShape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
        );

        auto threshTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_,
            const_cast<float*>(&fThreshold),
            1,
            scalarShape.data(),
            scalarShape.size()
        );

        auto radiusTensor = Ort::Value::CreateTensor<int64_t>(
            *memoryInfo_,
            const_cast<int64_t*>(&iRadius),
            1,
            scalarShape.data(),
            scalarShape.size()
        );

        std::vector<Ort::Value> inputTensors;
        inputTensors.reserve(8);
        inputTensors.push_back(std::move(xSegTensor));
        inputTensors.push_back(std::move(langTensor));
        inputTensors.push_back(std::move(knownTensor));
        inputTensors.push_back(std::move(prevTensor));
        inputTensors.push_back(std::move(tTensor));
        inputTensors.push_back(std::move(maskTTensor));
        inputTensors.push_back(std::move(threshTensor));
        inputTensors.push_back(std::move(radiusTensor));

        auto outputTensors = segmenterSession_->Run(
            Ort::RunOptions{nullptr},
            inputNames, inputTensors.data(), 8,
            outputNames, 1
        );

        // Update boundaries for next iteration
        const uint8_t* outBd = outputTensors[0].GetTensorData<uint8_t>();
        boundaries.resize(static_cast<size_t>(T));
        for (int64_t i = 0; i < T; ++i) {
            boundaries[static_cast<size_t>(i)] = (outBd[i] != 0);
        }
    }

    return boundaries;
}

// ==============================================================================
// Pipeline Step 4: Bd2Dur (boundaries → durations + maskN)
// ==============================================================================

GameExtractor::Bd2DurOutput GameExtractor::runBd2Dur(
    const std::vector<bool>& boundaries,
    const std::vector<bool>& maskT,
    int64_t T)
{
    Bd2DurOutput result;

    // Input: boundaries bool[1, T]
    std::vector<uint8_t> bdU8 = boolVecToU8(boundaries);
    std::vector<int64_t> bdShape = {1, T};
    auto bdTensor = Ort::Value::CreateTensor(
        *memoryInfo_,
        bdU8.data(),
        bdU8.size(),
        bdShape.data(),
        bdShape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
    );

    // Input: maskT bool[1, T]
    std::vector<uint8_t> maskU8 = boolVecToU8(maskT);
    auto maskTTensor = Ort::Value::CreateTensor(
        *memoryInfo_,
        maskU8.data(),
        maskU8.size(),
        bdShape.data(),
        bdShape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
    );

    const char* inputNames[]  = {"boundaries", "maskT"};
    const char* outputNames[] = {"durations", "maskN"};

    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(bdTensor));
    inputTensors.push_back(std::move(maskTTensor));

    auto outputTensors = bd2durSession_->Run(
        Ort::RunOptions{nullptr},
        inputNames, inputTensors.data(), 2,
        outputNames, 2
    );

    // Extract N from durations shape [1, N]
    const auto durShape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();
    result.N = durShape[1];

    // Copy durations float32[1, N]
    const float* durData = outputTensors[0].GetTensorData<float>();
    result.durations.assign(durData, durData + static_cast<size_t>(result.N));

    // Copy maskN bool[1, N]
    const uint8_t* maskNData = outputTensors[1].GetTensorData<uint8_t>();
    result.maskN.resize(static_cast<size_t>(result.N));
    for (int64_t i = 0; i < result.N; ++i) {
        result.maskN[static_cast<size_t>(i)] = (maskNData[i] != 0);
    }

    AppLogger::debug("[GameExtractor] Bd2Dur output: N=" + juce::String(result.N));

    return result;
}

// ==============================================================================
// Pipeline Step 5: Estimator (presence + scores)
// ==============================================================================

GameExtractor::EstimatorOutput GameExtractor::runEstimator(
    const std::vector<float>& x_est, int64_t T, int64_t C,
    const std::vector<bool>& boundaries,
    const std::vector<bool>& maskT,
    const std::vector<bool>& maskN,
    int64_t N,
    float presenceThreshold)
{
    EstimatorOutput result;
    result.N = N;

    const size_t tcCount = static_cast<size_t>(T * C);

    // Input: x_est float32[1, T, C]
    std::vector<int64_t> xEstShape = {1, T, C};
    auto xEstTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_,
        const_cast<float*>(x_est.data()),
        tcCount,
        xEstShape.data(),
        xEstShape.size()
    );

    // Input: boundaries bool[1, T]
    std::vector<uint8_t> bdU8 = boolVecToU8(boundaries);
    std::vector<int64_t> bdShape = {1, T};
    auto bdTensor = Ort::Value::CreateTensor(
        *memoryInfo_,
        bdU8.data(),
        bdU8.size(),
        bdShape.data(),
        bdShape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
    );

    // Input: maskT bool[1, T]
    std::vector<uint8_t> maskTU8 = boolVecToU8(maskT);
    auto maskTTensor = Ort::Value::CreateTensor(
        *memoryInfo_,
        maskTU8.data(),
        maskTU8.size(),
        bdShape.data(),
        bdShape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
    );

    // Input: maskN bool[1, N]
    std::vector<uint8_t> maskNU8 = boolVecToU8(maskN);
    std::vector<int64_t> nShape = {1, N};
    auto maskNTensor = Ort::Value::CreateTensor(
        *memoryInfo_,
        maskNU8.data(),
        maskNU8.size(),
        nShape.data(),
        nShape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL
    );

    // Input: threshold float32 scalar
    std::vector<int64_t> scalarShape = {1};
    auto threshTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_,
        const_cast<float*>(&presenceThreshold),
        1,
        scalarShape.data(),
        scalarShape.size()
    );

    const char* inputNames[]  = {"x_est", "boundaries", "maskT", "maskN", "threshold"};
    const char* outputNames[] = {"presence", "scores"};

    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(xEstTensor));
    inputTensors.push_back(std::move(bdTensor));
    inputTensors.push_back(std::move(maskTTensor));
    inputTensors.push_back(std::move(maskNTensor));
    inputTensors.push_back(std::move(threshTensor));

    auto outputTensors = estimatorSession_->Run(
        Ort::RunOptions{nullptr},
        inputNames, inputTensors.data(), 5,
        outputNames, 2
    );

    // Copy presence bool[1, N]
    const uint8_t* presData = outputTensors[0].GetTensorData<uint8_t>();
    result.presence.resize(static_cast<size_t>(N));
    for (int64_t i = 0; i < N; ++i) {
        result.presence[static_cast<size_t>(i)] = (presData[i] != 0);
    }

    // Copy scores float32[1, N]
    const float* scoresData = outputTensors[1].GetTensorData<float>();
    result.scores.assign(scoresData, scoresData + static_cast<size_t>(N));

    return result;
}

// ==============================================================================
// Orchestration: extractChunk
// ==============================================================================

GameExtractor::ChunkResult GameExtractor::extractChunk(
    const float* audio,
    size_t numSamples,
    const std::vector<float>& knownDurations,
    const GameConfig& config,
    std::function<void(float)> progressCallback)
{
    ChunkResult result;

    if (!isLoaded()) {
        AppLogger::error("[GameExtractor] Cannot extract chunk: sessions not loaded");
        return result;
    }

    if (audio == nullptr || numSamples == 0) {
        AppLogger::error("[GameExtractor] Cannot extract chunk: empty audio input");
        return result;
    }

    try {
        // ---- Step 1: Encoder ----
        if (progressCallback) progressCallback(0.05f);
        auto enc = runEncoder(audio, numSamples);

        // ---- Step 2: Known durations → boundaries ----
        if (progressCallback) progressCallback(0.12f);
        auto knownBoundaries = runDur2Bd(knownDurations, enc.maskT, enc.T);

        // ---- Step 3: Segmenter (D3PM iterative refinement) ----
        if (progressCallback) progressCallback(0.18f);
        auto boundaries = runSegmenter(enc.x_seg, enc.T, enc.C, knownBoundaries, enc.maskT, config);
        if (progressCallback) progressCallback(0.60f);

        // ---- Step 4: Boundaries → durations + maskN ----
        if (progressCallback) progressCallback(0.65f);
        auto bd2durOut = runBd2Dur(boundaries, enc.maskT, enc.T);

        // ---- Step 5: Estimator (presence + scores) ----
        if (progressCallback) progressCallback(0.80f);
        auto est = runEstimator(
            enc.x_est, enc.T, enc.C,
            boundaries,
            enc.maskT,
            bd2durOut.maskN,
            bd2durOut.N,
            config.presenceThreshold
        );

        // ---- Step 6: Convert to ReferenceNote ----
        if (progressCallback) progressCallback(0.92f);

        double onset = 0.0;
        for (int64_t i = 0; i < bd2durOut.N; ++i)
        {
            const size_t idx = static_cast<size_t>(i);
            onset += static_cast<double>(bd2durOut.durations[idx]);
            if (bd2durOut.maskN[idx]) {
                ReferenceNote note;
                note.onset  = onset - static_cast<double>(bd2durOut.durations[idx]);
                note.offset = onset;
                note.midiPitch = est.scores[idx];
                note.voiced = est.presence[idx];
                result.notes.push_back(note);
            }
        }

        if (progressCallback) progressCallback(1.0f);

        AppLogger::info("[GameExtractor] Chunk extraction complete: "
            + juce::String(static_cast<int>(result.notes.size())) + " notes, "
            + "T=" + juce::String(enc.T) + ", N=" + juce::String(bd2durOut.N));

    } catch (const Ort::Exception& e) {
        AppLogger::error("[GameExtractor] ONNX error during extraction: " + juce::String(e.what()));
        return {};
    } catch (const std::exception& e) {
        AppLogger::error("[GameExtractor] Error during extraction: " + juce::String(e.what()));
        return {};
    } catch (...) {
        AppLogger::error("[GameExtractor] Unknown error during extraction");
        return {};
    }

    return result;
}

} // namespace OpenTune

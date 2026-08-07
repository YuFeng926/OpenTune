#include "OpenTuneDocumentController.h"

#include "OpenTuneEditorView.h"
#include "OpenTunePlaybackRenderer.h"

#include "../Inference/F0InferenceService.h"
#include "../Runtime/ProcessF0Runtime.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Services/F0ExtractionService.h"
#include "../Services/ImportedClipF0Extraction.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"
#include "../Utils/SilentGapDetector.h"
#include "../Utils/PitchCurve.h"
#include "../Inference/RenderCache.h"
#include "../Utils/SourceWindow.h"
#include "../Utils/AppLogger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <utility>

namespace OpenTune {

namespace {

constexpr int kContentPayloadArchiveMagic = 0x4F544143;
constexpr int kContentPayloadArchiveVersion = 4; // v4 unifies note gain and sibilant gain into VolumeEnvelope
constexpr int kContentPayloadArchiveVersionMin = 3;
constexpr int kMaxContentPayloadRecords = 4096;

} // namespace

OpenTuneDocumentController::OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                                        const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation(entry, instance)
    , contentRenderService_(std::make_shared<ContentRenderService>())
    , resamplingManager_(std::make_shared<ResamplingManager>())
    , contentF0ExtractionService_(std::make_unique<F0ExtractionService>(
        1, 64, [] { return ProcessF0Runtime::getInstance().getF0Service(); }))
{
    asyncLeaseToken_ = std::make_shared<std::atomic<bool>>(true);
    installDocumentRenderExecution();

    // 进程级运行时客户端租约（仅计数，不触发释放）
    ProcessF0Runtime::getInstance().attach();
    ProcessRenderRuntime::getInstance().attach();
}

OpenTuneDocumentController::~OpenTuneDocumentController()
{
    // 撤销服务租约，防止异步 F0 completion 写回已析构的 DC
    if (asyncLeaseToken_)
        asyncLeaseToken_->store(false, std::memory_order_release);

    // 最前段关闭 F0 owner：丢弃排队任务、清空 active、终止本 owner 的活跃
    // F0 Run（SetTerminate 加速返回）。不 join worker —— worker 是 detached
    // 进程常驻执行器，见 shutdownStarted_ 后自行退出，期间只访问进程级 F0
    // 服务与提交时捕获的纯数据，绝不访问已析构的 DC/service。
    contentF0ExtractionService_.reset();

    // 停止渲染服务：detach execution lease（终止操作：清 lease + 丢弃排队 job
    // + 等待执行中的回调完成）。不等待 asyncInFlight_：vocoder 推理不可取消，
    // 其 onComplete 经 shared_ptr 持有 ContentRenderService，必然回调。
    if (contentRenderService_)
        contentRenderService_->detachExecutionLease(this);

    // Owner-driven detach: before clearing playbackRenderers_, walk the list
    // and call detachDocumentController(*this) on each renderer.
    for (auto* renderer : playbackRenderers_)
    {
        if (renderer != nullptr)
            renderer->detachDocumentController(*this);
    }
    playbackRenderers_.clear();

    // 进程级运行时客户端租约释放（仅递减计数，不触发任何释放）
    ProcessRenderRuntime::getInstance().detach();
    ProcessF0Runtime::getInstance().detach();
}

namespace {
void serializeAudioModificationContent(const AudioModification& mod, juce::XmlElement& el)
{
    el.setAttribute("contentRevision", juce::String(static_cast<juce::int64>(mod.content->contentRevision)));
    el.setAttribute("birthRevision", juce::String(static_cast<juce::int64>(mod.birthRevision)));

    auto* sw = new juce::XmlElement("SourceWindow");
    sw->setAttribute("sourcePersistentId", mod.content->sourceWindow.sourcePersistentId);
    // Note: numeric sourceId is NOT serialized in ARA domain (it's for Capture/Standalone only)
    // ARA domain uses AudioSource persistentID only
    sw->setAttribute("startSeconds", mod.content->sourceWindow.sourceStartSeconds);
    sw->setAttribute("durationSeconds", mod.content->sourceWindow.durationSeconds());
    el.addChildElement(sw);

    auto* editable = new juce::XmlElement("EditableContent");
    editable->setAttribute("notesRevision", juce::String(static_cast<juce::int64>(mod.content->editable.notesRevision)));
    editable->setAttribute("pitchRevision", juce::String(static_cast<juce::int64>(mod.content->editable.pitchRevision)));
    editable->setAttribute("timeGridRevision", juce::String(static_cast<juce::int64>(mod.content->editable.timeGridRevision)));
    editable->setAttribute("pitchShiftRevision", juce::String(static_cast<juce::int64>(mod.content->editable.pitchShiftRevision)));
    editable->setAttribute("contentRevision", juce::String(static_cast<juce::int64>(mod.content->editable.contentRevision)));

    for (const auto& note : mod.content->editable.notes)
    {
        auto* n = new juce::XmlElement("Note");
        n->setAttribute("start", note.startTime);
        n->setAttribute("end", note.endTime);
        n->setAttribute("pitch", note.pitch);
        n->setAttribute("originalPitch", note.originalPitch);
        n->setAttribute("pitchOffset", note.pitchOffset);
        n->setAttribute("retuneSpeed", note.retuneSpeed);
        n->setAttribute("pitchDriftScale", note.pitchDriftScale);
        n->setAttribute("vibratoDepth", note.vibratoDepth);
        n->setAttribute("vibratoRate", note.vibratoRate);
        n->setAttribute("outputGainDb", note.outputGainDb);
        n->setAttribute("isVoiced", note.isVoiced ? 1 : 0);
        editable->addChildElement(n);
    }

    // Volume Envelope（AutomationLane，revision 不落盘）
    auto* env = new juce::XmlElement("VolumeEnvelope");
    env->setAttribute("pointCount", static_cast<int>(mod.content->editable.volumeEnvelope.points().size()));
    for (const auto& point : mod.content->editable.volumeEnvelope.points())
    {
        auto* p = new juce::XmlElement("Point");
        p->setAttribute("time", point.timeSeconds);
        p->setAttribute("gainDb", point.gainDb);
        env->addChildElement(p);
    }
    editable->addChildElement(env);

    if (mod.content->analysis.pitchCurve != nullptr)
    {
        const auto pitchSnapshot = mod.content->analysis.pitchCurve->getSnapshot();
        for (const auto& seg : pitchSnapshot->getCorrectionSegments())
        {
            auto* s = new juce::XmlElement("PitchCorrectionSegment");
            s->setAttribute("startFrame", seg.startFrame);
            s->setAttribute("endFrame", seg.endFrame);
            s->setAttribute("source", static_cast<int>(seg.source));
            s->setAttribute("retuneSpeed", seg.retuneSpeed);
            s->setAttribute("pitchDriftScale", seg.pitchDriftScale);
            s->setAttribute("vibratoDepth", seg.vibratoDepth);
            s->setAttribute("vibratoRate", seg.vibratoRate);
            const juce::MemoryBlock f0Data(seg.f0Data.data(), seg.f0Data.size() * sizeof(float));
            s->setAttribute("f0Base64", f0Data.toBase64Encoding());
            editable->addChildElement(s);
        }
    }

    auto* ps = new juce::XmlElement("PitchShiftSettings");
    ps->setAttribute("semitone", mod.content->editable.pitchShiftSettings.semitone);
    ps->setAttribute("cents", mod.content->editable.pitchShiftSettings.cents);
    editable->addChildElement(ps);

    // TimeGrid: owner invariant 保证完整 content 的 timeGrid 非空，无条件写出
    {
        auto* tg = new juce::XmlElement("TimeGrid");
        for (const auto& h : mod.content->editable.timeGrid->handles())
        {
            auto* he = new juce::XmlElement("Handle");
            he->setAttribute("id", juce::String(static_cast<juce::int64>(h.id)));
            he->setAttribute("sourceSeconds", h.source_seconds);
            he->setAttribute("outputSeconds", h.output_seconds);
            he->setAttribute("kind", static_cast<int>(h.kind));
            // confidence：同步 Confidence 枚举（0=default, 1=high）
            he->setAttribute("confidence", static_cast<int>(h.confidence));
            tg->addChildElement(he);
        }
        editable->addChildElement(tg);
    }
    el.addChildElement(editable);

    auto* analysis = new juce::XmlElement("AnalysisState");
    analysis->setAttribute("f0Lifecycle", static_cast<int>(mod.content->analysis.f0Lifecycle));
    analysis->setAttribute("pitchLifecycle", static_cast<int>(mod.content->analysis.pitchLifecycle));
    analysis->setAttribute("analysisRevision", juce::String(static_cast<juce::int64>(mod.content->analysis.analysisRevision)));
    analysis->setAttribute("originalF0State", static_cast<int>(mod.content->analysis.originalF0State));
    
    // DetectedKey
    auto* dk = new juce::XmlElement("DetectedKey");
    dk->setAttribute("root", static_cast<int>(mod.content->analysis.detectedKey.root));
    dk->setAttribute("scale", static_cast<int>(mod.content->analysis.detectedKey.scale));
    dk->setAttribute("confidence", mod.content->analysis.detectedKey.confidence);
    analysis->addChildElement(dk);
    
    // SilentGaps
    for (const auto& gap : mod.content->analysis.silentGaps)
    {
        auto* sg = new juce::XmlElement("SilentGap");
        sg->setAttribute("startSample", juce::String(static_cast<juce::int64>(gap.startSample)));
        sg->setAttribute("endSampleExclusive", juce::String(static_cast<juce::int64>(gap.endSampleExclusive)));
        sg->setAttribute("minLevel_dB", gap.minLevel_dB);
        analysis->addChildElement(sg);
    }
    
    // ReferenceFeatures
    if (mod.content->analysis.referenceFeatures.status != ReferenceFeatureStatus::NotRequested)
    {
        auto* rf = new juce::XmlElement("ReferenceFeatures");
        rf->setAttribute("analysisRevision", mod.content->analysis.referenceFeatures.analysisRevision);
        rf->setAttribute("status", static_cast<int>(mod.content->analysis.referenceFeatures.status));
        rf->setAttribute("producer", static_cast<int>(mod.content->analysis.referenceFeatures.producer));
        rf->setAttribute("inputFingerprint", juce::String(static_cast<juce::int64>(mod.content->analysis.referenceFeatures.inputFingerprint)));
        rf->setAttribute("sourceDurationSeconds", mod.content->analysis.referenceFeatures.sourceDurationSeconds);
        if (mod.content->analysis.referenceFeatures.errorMessage.isNotEmpty())
            rf->setAttribute("errorMessage", mod.content->analysis.referenceFeatures.errorMessage);
        
        // Pitch notes
        for (const auto& note : mod.content->analysis.referenceFeatures.pitch.notes)
        {
            auto* n = new juce::XmlElement("PitchNote");
            n->setAttribute("start", note.startTime);
            n->setAttribute("end", note.endTime);
            n->setAttribute("pitch", note.pitch);
            n->setAttribute("originalPitch", note.originalPitch);
            n->setAttribute("pitchOffset", note.pitchOffset);
            n->setAttribute("retuneSpeed", note.retuneSpeed);
            n->setAttribute("pitchDriftScale", note.pitchDriftScale);
            n->setAttribute("vibratoDepth", note.vibratoDepth);
            n->setAttribute("vibratoRate", note.vibratoRate);
            n->setAttribute("outputGainDb", note.outputGainDb);
            rf->addChildElement(n);
        }
        
        // Timing anchors
        for (const auto& anchor : mod.content->analysis.referenceFeatures.timing.anchors)
        {
            auto* a = new juce::XmlElement("TimingAnchor");
            a->setAttribute("anchorId", juce::String(static_cast<juce::int64>(anchor.anchorId)));
            a->setAttribute("sourceSeconds", anchor.sourceSeconds);
            a->setAttribute("strength", anchor.strength);
            a->setAttribute("kind", static_cast<int>(anchor.kind));
            a->setAttribute("confidence", anchor.confidence);
            rf->addChildElement(a);
        }
        
        analysis->addChildElement(rf);
    }
    
    if (mod.content->analysis.pitchCurve)
    {
        auto snap = mod.content->analysis.pitchCurve->getSnapshot();
        if (snap)
        {
            auto* pc = new juce::XmlElement("PitchCurve");
            pc->setAttribute("hopSize", snap->getHopSize());
            pc->setAttribute("sampleRate", snap->getSampleRate());
            {
                const auto& f0 = snap->getOriginalF0();
                juce::MemoryBlock mb(f0.data(), f0.size() * sizeof(float));
                pc->setAttribute("f0Base64", mb.toBase64Encoding());
            }
            {
                const auto& energy = snap->getOriginalEnergy();
                if (!energy.empty())
                {
                    juce::MemoryBlock mb(energy.data(), energy.size() * sizeof(float));
                    pc->setAttribute("energyBase64", mb.toBase64Encoding());
                }
            }
            analysis->addChildElement(pc);
        }
    }
    el.addChildElement(analysis);
}

// Parse XML to new AudioModificationContentState with complete replacement semantics.
// Missing optional children naturally result in default/empty state.
// Per ARA2 spec: Maps archived source persistentID to current source persistentID via filter.
std::optional<AudioModificationContentState> restoreAudioModificationContent(const juce::XmlElement& el,
                                                                               const juce::ARARestoreObjectsFilter* filter)
{
    AudioModificationContentState content;
    std::vector<PitchCorrectionSegment> restoredCorrectionSegments;
    content.contentRevision = static_cast<uint64_t>(
        el.getStringAttribute("contentRevision").getLargeIntValue());

    // source window 必须存在
    if (auto* sw = el.getChildByName("SourceWindow"))
    {
        const juce::String archivedSourcePersistentId = sw->getStringAttribute("sourcePersistentId");

        // valid check
        double startSeconds = sw->getDoubleAttribute("startSeconds");
        double durationSeconds = sw->getDoubleAttribute("durationSeconds");

        // finite checks (范围/有效性：start=0.0 是有效起点，duration>0)
        if (!std::isfinite(startSeconds) || !std::isfinite(durationSeconds) || durationSeconds <= 0.0)
            return std::nullopt;
        const double endSeconds = startSeconds + durationSeconds;
        if (!std::isfinite(endSeconds))
            return std::nullopt;

        // Per ARA2 spec (ARAInterface.h:3092): "Any archived states that are either filtered
        // explicitly, or for which there is no object with a matching persistent ID in the
        // current graph are simply ignored."
        // ARA domain uses sourcePersistentId only (not numeric sourceId from Capture/Standalone).
        // Filter miss -> entire SourceWindow stays invalid (all fields zero/empty).
        if (filter == nullptr)
        {
            // Full restore: use archived source persistentID and window as-is
            if (archivedSourcePersistentId.isEmpty())
                return std::nullopt;
            content.sourceWindow.sourcePersistentId = archivedSourcePersistentId;
        }
        else if (archivedSourcePersistentId.isNotEmpty())
        {
            auto* audioSource = filter->getAudioSourceToRestoreStateWithID(
                archivedSourcePersistentId.toRawUTF8());
            if (audioSource == nullptr)
                return std::nullopt;

            const auto& remappedId = audioSource->getPersistentID();
            if (remappedId.empty())
                return std::nullopt;
            content.sourceWindow.sourcePersistentId = juce::String::fromUTF8(remappedId.c_str());
        }
        else
        {
            return std::nullopt;
        }

        content.sourceWindow.sourceStartSeconds = startSeconds;
        content.sourceWindow.sourceEndSeconds = endSeconds;
        // Note: numeric sourceId is NOT restored in ARA domain (it's for Capture/Standalone only)
    }
    else
    {
        return std::nullopt;
    }

    // EditableContent: 必须存在，缺失立即 nullopt
    auto* editable = el.getChildByName("EditableContent");
    if (editable == nullptr)
        return std::nullopt;

    {
        content.editable.notesRevision = static_cast<uint64_t>(editable->getStringAttribute("notesRevision").getLargeIntValue());
        content.editable.pitchRevision = static_cast<uint64_t>(editable->getStringAttribute("pitchRevision").getLargeIntValue());
        content.editable.timeGridRevision = static_cast<uint64_t>(editable->getStringAttribute("timeGridRevision").getLargeIntValue());
        content.editable.pitchShiftRevision = static_cast<uint64_t>(editable->getStringAttribute("pitchShiftRevision").getLargeIntValue());
        content.editable.contentRevision = static_cast<uint64_t>(editable->getStringAttribute("contentRevision").getLargeIntValue());

        // notes parsing with finite checks
        for (auto* n : editable->getChildWithTagNameIterator("Note"))
        {
            Note note;
            note.startTime = n->getDoubleAttribute("start");
            note.endTime = n->getDoubleAttribute("end");

            // 有效性检查：时间必须为有限值，且 endTime > startTime
            if (!std::isfinite(note.startTime) || !std::isfinite(note.endTime) || note.endTime <= note.startTime)
                return std::nullopt;

            // pitch 及后续浮点字段的有限性检查
            note.pitch = static_cast<float>(n->getDoubleAttribute("pitch"));
            note.originalPitch = static_cast<float>(n->getDoubleAttribute("originalPitch"));
            note.pitchOffset = static_cast<float>(n->getDoubleAttribute("pitchOffset"));
            note.retuneSpeed = static_cast<float>(n->getDoubleAttribute("retuneSpeed"));
            note.pitchDriftScale = static_cast<float>(n->getDoubleAttribute("pitchDriftScale", 1.0));
            note.vibratoDepth = static_cast<float>(n->getDoubleAttribute("vibratoDepth"));
            note.vibratoRate = static_cast<float>(n->getDoubleAttribute("vibratoRate"));

            // 验证浮点值的有效性，时间长度与速度、振幅率都必须为有限值
            if (!std::isfinite(note.pitch) || !std::isfinite(note.originalPitch) ||
                !std::isfinite(note.pitchOffset) || !std::isfinite(note.retuneSpeed) ||
                !std::isfinite(note.vibratoDepth) || !std::isfinite(note.vibratoRate) ||
                !std::isfinite(note.pitchDriftScale))
                return std::nullopt;

            note.outputGainDb = static_cast<float>(n->getDoubleAttribute("outputGainDb"));
            if (!std::isfinite(note.outputGainDb))
                return std::nullopt;
            note.isVoiced = n->getIntAttribute("isVoiced") != 0;
            content.editable.notes.push_back(note);
        }

        if (auto* env = editable->getChildByName("VolumeEnvelope")) {
            std::vector<AutomationPoint> points;
            for (auto* p : env->getChildWithTagNameIterator("Point"))
            {
                AutomationPoint point;
                point.timeSeconds = p->getDoubleAttribute("time");
                point.gainDb = static_cast<float>(p->getDoubleAttribute("gainDb"));
                if (!std::isfinite(point.timeSeconds) || !std::isfinite(point.gainDb))
                    return std::nullopt;
                points.push_back(point);
            }
            content.editable.volumeEnvelope = AutomationLane::fromSnapshot(points);
        } else {
            const auto noteGainLane = AutomationLane::fromLegacyNoteGains(content.editable.notes);
            if (auto* legacyEnvelope = editable->getChildByName("SibilantGainEnvelope")) {
                std::vector<AutomationPoint> points;
                for (auto* p : legacyEnvelope->getChildWithTagNameIterator("Point")) {
                    const AutomationPoint point {
                        p->getDoubleAttribute("time"),
                        static_cast<float>(p->getDoubleAttribute("gainDb"))
                    };
                    if (!std::isfinite(point.timeSeconds) || !std::isfinite(point.gainDb))
                        return std::nullopt;
                    points.push_back(point);
                }
                content.editable.volumeEnvelope = AutomationLane::sum(
                    noteGainLane, AutomationLane::fromLegacyStepPoints(points));
            } else {
                content.editable.volumeEnvelope = noteGainLane;
            }
        }
        for (auto& note : content.editable.notes)
            note.outputGainDb = content.editable.volumeEnvelope.evalAt(note.startTime);

        for (auto* s : editable->getChildWithTagNameIterator("PitchCorrectionSegment"))
        {
            PitchCorrectionSegment seg;
            seg.startFrame = s->getIntAttribute("startFrame");
            seg.endFrame = s->getIntAttribute("endFrame");

            // frame 索引有效性：start <= end，非负
            if (seg.startFrame < 0 || seg.endFrame <= seg.startFrame)
                return std::nullopt;

            seg.source = static_cast<PitchCorrectionSegment::Source>(s->getIntAttribute("source"));
            if (seg.source < PitchCorrectionSegment::Source::None || seg.source > PitchCorrectionSegment::Source::LineAnchor)
                return std::nullopt;

            seg.retuneSpeed = static_cast<float>(s->getDoubleAttribute("retuneSpeed"));
            seg.pitchDriftScale = static_cast<float>(s->getDoubleAttribute("pitchDriftScale", 1.0));
            seg.vibratoDepth = static_cast<float>(s->getDoubleAttribute("vibratoDepth"));
            seg.vibratoRate = static_cast<float>(s->getDoubleAttribute("vibratoRate"));

            // 再次验证浮点值的有效性
            if (!std::isfinite(seg.retuneSpeed) || !std::isfinite(seg.vibratoDepth) || !std::isfinite(seg.vibratoRate) || !std::isfinite(seg.pitchDriftScale))
                return std::nullopt;

            juce::MemoryBlock f0Data;
            const auto encodedF0 = s->getStringAttribute("f0Base64");
            const auto expectedBytes = static_cast<size_t>(seg.endFrame - seg.startFrame) * sizeof(float);
            if (!f0Data.fromBase64Encoding(encodedF0) || f0Data.getSize() != expectedBytes)
                return std::nullopt;
            seg.f0Data.resize(expectedBytes / sizeof(float));
            std::memcpy(seg.f0Data.data(), f0Data.getData(), expectedBytes);
            for (const auto value : seg.f0Data)
                if (!std::isfinite(value))
                    return std::nullopt;

            restoredCorrectionSegments.push_back(std::move(seg));
        }

        if (auto* ps = editable->getChildByName("PitchShiftSettings"))
        {
            const int semitone = ps->getIntAttribute("semitone");
            const int cents = ps->getIntAttribute("cents");

            // 有效性检查：半音及音分必须为有限值，且在合理范围
            if (semitone < -24 || semitone > 24 || cents < -99 || cents > 99)
                return std::nullopt;

            content.editable.pitchShiftSettings.semitone = semitone;
            content.editable.pitchShiftSettings.cents = cents;
        }
    }

    // TimeGrid: 必须存在且通过 TimeGridSnapshot 工厂验证，不能 null/identity
    if (auto* tg = editable->getChildByName("TimeGrid"))
    {
        std::vector<TimeHandle> handles;
        for (auto* h : tg->getChildWithTagNameIterator("Handle"))
        {
            TimeHandle handle;
            handle.id = static_cast<uint64_t>(h->getStringAttribute("id").getLargeIntValue());
            if (handle.id == 0)
                return std::nullopt;

            handle.source_seconds = h->getDoubleAttribute("sourceSeconds");
            handle.output_seconds = h->getDoubleAttribute("outputSeconds");
            if (!std::isfinite(handle.source_seconds) || !std::isfinite(handle.output_seconds))
                return std::nullopt;

            const int kind = h->getIntAttribute("kind");
            if (kind < static_cast<int>(HandleKind::ClipStart)
                || kind > static_cast<int>(HandleKind::UserAdded))
                return std::nullopt;
            handle.kind = static_cast<HandleKind>(kind);

            const int confidence = h->getIntAttribute("confidence");
            if (confidence < static_cast<int>(Confidence::Default)
                || confidence > static_cast<int>(Confidence::High))
                return std::nullopt;
            handle.confidence = static_cast<Confidence>(confidence);

            handles.push_back(std::move(handle));
        }

        auto timeGrid = TimeGridSnapshot::makeFromHandles(
            std::move(handles), content.editable.timeGridRevision);
        if (timeGrid == nullptr)
            return std::nullopt;

        content.editable.timeGrid = std::move(timeGrid);

        // 完整性校验：TimeGrid 总时长与 sourceWindow 时长误差 <= 1e-6
        const double timeGridTotal = content.editable.timeGrid->totalDurationSeconds();
        const double sourceWindowDuration = content.sourceWindow.durationSeconds();
        if (std::abs(timeGridTotal - sourceWindowDuration) > 1e-6)
            return std::nullopt;
    }
    else
    {
        return std::nullopt;
    }

    // AnalysisState: 可选，absence 保持默认
    if (auto* analysis = el.getChildByName("AnalysisState"))
    {
        // 枚举值有效性检查
        const int f0Lifecycle = analysis->getIntAttribute("f0Lifecycle");
        const int pitchLifecycle = analysis->getIntAttribute("pitchLifecycle");
        const int originalF0State = analysis->getIntAttribute("originalF0State");

        // 有效性范围检查
        if (f0Lifecycle < static_cast<int>(AnalysisLifecycle::Idle) || f0Lifecycle > static_cast<int>(AnalysisLifecycle::Failed))
            return std::nullopt;
        if (pitchLifecycle < static_cast<int>(AnalysisLifecycle::Idle) || pitchLifecycle > static_cast<int>(AnalysisLifecycle::Failed))
            return std::nullopt;
        if (originalF0State < static_cast<int>(OriginalF0State::NotRequested) || originalF0State > static_cast<int>(OriginalF0State::Failed))
            return std::nullopt;

        content.analysis.f0Lifecycle = static_cast<AnalysisLifecycle>(f0Lifecycle);
        content.analysis.pitchLifecycle = static_cast<AnalysisLifecycle>(pitchLifecycle);
        content.analysis.analysisRevision = static_cast<uint64_t>(analysis->getStringAttribute("analysisRevision").getLargeIntValue());
        content.analysis.originalF0State = static_cast<OriginalF0State>(originalF0State);

        // DetectedKey: 可选，absence 保持默认（未检测）
        if (auto* dk = analysis->getChildByName("DetectedKey"))
        {
            const int root = dk->getIntAttribute("root");
            const int scale = dk->getIntAttribute("scale");
            const double confidence = dk->getDoubleAttribute("confidence");

            // 有效性范围检查
            if (root < 0 || root > 11) return std::nullopt;
            if (scale < 0 || scale > 7) return std::nullopt;
            if (!std::isfinite(confidence)) return std::nullopt;

            content.analysis.detectedKey.root = static_cast<Key>(root);
            content.analysis.detectedKey.scale = static_cast<Scale>(scale);
            content.analysis.detectedKey.confidence = static_cast<float>(confidence);
        }

        // SilentGaps: 可选元素，absence 保持空
        for (auto* sg : analysis->getChildWithTagNameIterator("SilentGap"))
        {
            int64_t startSample = sg->getStringAttribute("startSample").getLargeIntValue();
            int64_t endSampleExclusive = sg->getStringAttribute("endSampleExclusive").getLargeIntValue();
            float minLevel_dB = static_cast<float>(sg->getDoubleAttribute("minLevel_dB"));

            // 有效性范围（假设 startSample >= 0，endSampleExclusive > startSample，minLevel_dB <= 0）
            if (startSample < 0 || endSampleExclusive <= startSample) return std::nullopt;
            if (!std::isfinite(minLevel_dB)) return std::nullopt;

            SilentGap gap;
            gap.startSample = startSample;
            gap.endSampleExclusive = endSampleExclusive;
            gap.minLevel_dB = minLevel_dB;
            content.analysis.silentGaps.push_back(gap);
        }

        // ReferenceFeatures: 可选，absence 保持默认
        if (auto* rf = analysis->getChildByName("ReferenceFeatures"))
        {
            // 字段读写
            content.analysis.referenceFeatures.analysisRevision = rf->getIntAttribute("analysisRevision");

            const int rfStatus = rf->getIntAttribute("status");
            const int rfProducer = rf->getIntAttribute("producer");
            if (rfStatus < static_cast<int>(ReferenceFeatureStatus::NotRequested) ||
                rfStatus > static_cast<int>(ReferenceFeatureStatus::Failed))
                return std::nullopt;
            if (rfProducer != static_cast<int>(ReferenceFeatureProducer::Unknown) &&
                rfProducer != static_cast<int>(ReferenceFeatureProducer::Game) &&
                rfProducer != static_cast<int>(ReferenceFeatureProducer::StandardAuto))
                return std::nullopt;
            content.analysis.referenceFeatures.status = static_cast<ReferenceFeatureStatus>(rfStatus);
            content.analysis.referenceFeatures.producer = static_cast<ReferenceFeatureProducer>(rfProducer);
            content.analysis.referenceFeatures.inputFingerprint = rf->getStringAttribute("inputFingerprint").getLargeIntValue();

            const double sourceDurationSeconds = rf->getDoubleAttribute("sourceDurationSeconds");

            // 有效性检查
            if (!std::isfinite(sourceDurationSeconds) || sourceDurationSeconds <= 0)
                return std::nullopt;

            content.analysis.referenceFeatures.sourceDurationSeconds = sourceDurationSeconds;
            content.analysis.referenceFeatures.errorMessage = rf->getStringAttribute("errorMessage");

            // Pitch notes
            for (auto* n : rf->getChildWithTagNameIterator("PitchNote"))
            {
                Note note;
                note.startTime = n->getDoubleAttribute("start");
                note.endTime = n->getDoubleAttribute("end");
                note.pitch = static_cast<float>(n->getDoubleAttribute("pitch"));
                note.originalPitch = static_cast<float>(n->getDoubleAttribute("originalPitch"));
                note.pitchOffset = static_cast<float>(n->getDoubleAttribute("pitchOffset"));
                note.retuneSpeed = static_cast<float>(n->getDoubleAttribute("retuneSpeed"));
                note.pitchDriftScale = static_cast<float>(n->getDoubleAttribute("pitchDriftScale", 1.0));
                note.vibratoDepth = static_cast<float>(n->getDoubleAttribute("vibratoDepth"));
                note.vibratoRate = static_cast<float>(n->getDoubleAttribute("vibratoRate"));
                note.outputGainDb = static_cast<float>(n->getDoubleAttribute("outputGainDb"));

                if (!std::isfinite(note.startTime) || !std::isfinite(note.endTime) ||
                    !std::isfinite(note.pitch) || !std::isfinite(note.originalPitch) ||
                    !std::isfinite(note.pitchOffset) || !std::isfinite(note.retuneSpeed) ||
                    !std::isfinite(note.vibratoDepth) || !std::isfinite(note.vibratoRate) ||
                    !std::isfinite(note.pitchDriftScale) || !std::isfinite(note.outputGainDb) ||
                    note.endTime <= note.startTime)
                    return std::nullopt;

                content.analysis.referenceFeatures.pitch.notes.push_back(note);
            }

            // Timing anchors
            for (auto* a : rf->getChildWithTagNameIterator("TimingAnchor"))
            {
                const int anchorKind = a->getIntAttribute("kind");
                ReferenceTimingAnchor anchor;
                anchor.anchorId = static_cast<uint64_t>(a->getStringAttribute("anchorId").getLargeIntValue());
                anchor.sourceSeconds = a->getDoubleAttribute("sourceSeconds");
                anchor.strength = static_cast<float>(a->getDoubleAttribute("strength"));
                anchor.kind = static_cast<ReferenceTimingAnchorKind>(anchorKind);
                anchor.confidence = static_cast<float>(a->getDoubleAttribute("confidence"));

                // 有效性检查
                if (anchor.anchorId == 0)
                    return std::nullopt;
                if (anchorKind < static_cast<int>(ReferenceTimingAnchorKind::Onset) ||
                    anchorKind > static_cast<int>(ReferenceTimingAnchorKind::PitchTransition))
                    return std::nullopt;
                if (!std::isfinite(anchor.sourceSeconds) || !std::isfinite(anchor.strength) ||
                    !std::isfinite(anchor.confidence) || anchor.sourceSeconds < 0.0
                    || anchor.sourceSeconds > sourceDurationSeconds)
                    return std::nullopt;
                content.analysis.referenceFeatures.timing.anchors.push_back(anchor);
            }
        }

        // PitchCurve: 可选，absence 保持 nullptr
        if (auto* pc = analysis->getChildByName("PitchCurve"))
        {
            const int hopSize = pc->getIntAttribute("hopSize");
            const double sampleRate = pc->getDoubleAttribute("sampleRate");

            // 有效性检查
            if (hopSize <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0)
                return std::nullopt;

            std::vector<float> f0;
            {
                juce::MemoryBlock mb;
                const auto encoded = pc->getStringAttribute("f0Base64");
                if (encoded.isNotEmpty())
                {
                    if (!mb.fromBase64Encoding(encoded) || mb.getSize() % sizeof(float) != 0)
                        return std::nullopt;
                    f0.resize(mb.getSize() / sizeof(float));
                    std::memcpy(f0.data(), mb.getData(), mb.getSize());
                }
            }

            std::vector<float> energy;
            {
                juce::MemoryBlock mb;
                const auto encoded = pc->getStringAttribute("energyBase64");
                if (encoded.isNotEmpty())
                {
                    if (!mb.fromBase64Encoding(encoded) || mb.getSize() % sizeof(float) != 0)
                        return std::nullopt;
                    energy.resize(mb.getSize() / sizeof(float));
                    std::memcpy(energy.data(), mb.getData(), mb.getSize());
                }
            }

            for (const auto value : f0)
                if (!std::isfinite(value))
                    return std::nullopt;
            for (const auto value : energy)
                if (!std::isfinite(value))
                    return std::nullopt;

            content.analysis.pitchCurve = std::make_shared<PitchCurve>();
            content.analysis.pitchCurve->setHopSize(hopSize);
            content.analysis.pitchCurve->setSampleRate(sampleRate);
            if (!f0.empty())
                content.analysis.pitchCurve->setOriginalF0(std::move(f0));
            if (!energy.empty())
                content.analysis.pitchCurve->setOriginalEnergy(std::move(energy));
        }
    }

    if (!restoredCorrectionSegments.empty())
    {
        if (content.analysis.pitchCurve == nullptr)
            return std::nullopt;

        const int frameCount = static_cast<int>(content.analysis.pitchCurve->size());
        for (const auto& segment : restoredCorrectionSegments)
            if (segment.endFrame > frameCount)
                return std::nullopt;

        content.analysis.pitchCurve->replaceCorrectionSegments(restoredCorrectionSegments);
    }

    return content;
}
} // namespace

const ContentRenderService* OpenTuneDocumentController::getContentRenderService() const noexcept
{
    return contentRenderService_.get();
}

std::shared_ptr<ContentRenderService> OpenTuneDocumentController::getContentRenderServiceShared() const noexcept
{
    return contentRenderService_;
}

bool OpenTuneDocumentController::PlaybackRegionProjection::isPlaybackRenderable() const noexcept
{
    return playbackSourceReady
        && contentKey.isValid()
        && contentDurationSeconds > 0.0
        && durationInPlaybackTime > 0.0
        && durationInModificationTime > 0.0;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getPlaybackRegionProjections() const
{
    return buildProjections();
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getPlaybackRegionProjectionsFor(
    const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const
{
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve(playbackRegions.size());

    for (auto* playbackRegion : playbackRegions)
    {
        const auto* region = findPlaybackRegion(playbackRegion);
        if (region != nullptr && region->hasValidPlacement())
            projections.push_back(makeProjection(*region));
    }

    return projections;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getEditorSelectionPlaybackRegionProjections() const
{
    return getPlaybackRegionProjectionsFor(editorSelectionPlaybackRegions_);
}

std::optional<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getFocusedEditorPlaybackRegionProjection() const
{
    const auto projections = getEditorSelectionPlaybackRegionProjections();
    if (projections.empty())
        return std::nullopt;

    return projections.front();
}

int OpenTuneDocumentController::requestReadAudioForPlaybackRegions()
{
    // User-read entry point: the ONLY method that reads AudioSource samples.
    // Per architecture: sample access enable is permission, not user intent.
    // Only explicit user button press (Record/Read) can read host audio.

    std::set<juce::String> uniqueModIds;
    for (const auto& region : playbackRegions_)
    {
        if (region.hasValidPlacement())
            uniqueModIds.insert(region.audioModificationPersistentId);
    }

    if (uniqueModIds.empty())
        return 0;

    int refreshedCount = 0;

    for (const auto& modId : uniqueModIds)
    {
        auto* modification = findAudioModification(modId);
        if (modification == nullptr)
            continue;

        // 只对有 content 或等待 source 的 modification 调用 birthContentForModification
        if (birthContentForModification(*modification))
            ++refreshedCount;
    }

    if (refreshedCount > 0)
        refreshRegisteredRenderers(publishModelChange());

    return refreshedCount;
}

void OpenTuneDocumentController::requestReadAudioForPlaybackRegionsAsync(
    std::function<void(int)> completionCallback)
{
    // ARA SDK requires DocumentController operations on main thread.
    // This method now executes synchronously to comply with ARA thread constraints.
    // Callers should display a loading overlay before calling if UI responsiveness is needed.
    const int count = requestReadAudioForPlaybackRegions();
    if (completionCallback)
        completionCallback(count);
}

void OpenTuneDocumentController::setEditorViewSelectionPlaybackRegions(
    std::vector<juce::ARAPlaybackRegion*> playbackRegions)
{
    editorSelectionPlaybackRegions_ = std::move(playbackRegions);
    reconcileEditorSelectionPlaybackRegions();
}

void OpenTuneDocumentController::registerPlaybackRenderer(OpenTunePlaybackRenderer& renderer)
{
    if (std::find(playbackRenderers_.begin(), playbackRenderers_.end(), &renderer) == playbackRenderers_.end())
        playbackRenderers_.push_back(&renderer);
}

void OpenTuneDocumentController::unregisterPlaybackRenderer(OpenTunePlaybackRenderer& renderer)
{
    playbackRenderers_.erase(std::remove(playbackRenderers_.begin(), playbackRenderers_.end(), &renderer),
                             playbackRenderers_.end());
}

void OpenTuneDocumentController::didUpdateMusicalContextProperties(juce::ARAMusicalContext* musicalContext)
{
    juce::ignoreUnused(musicalContext);
}

void OpenTuneDocumentController::willBeginEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
}

void OpenTuneDocumentController::didEndEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioModificationProperties(juce::ARAAudioModification* audioModification)
{
    auto& modification = ensureAudioModification(audioModification);
    modification.updateIdentity(audioModification);
    bindAudioModificationIdentity(modification);
    if (auto* source = findAudioSource(audioModification != nullptr ? audioModification->getAudioSource() : nullptr))
        modification.attachSource(*source);
    // Note: birthContentForModification() called lazily on read request, not here

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyAudioModification(juce::ARAAudioModification* audioModification)
{
    auto* mod = findAudioModification(audioModification);
    if (mod == nullptr)
        return;

    // 清理 CRS derived artifacts
    removeCRSArtifactsForModification(*mod);

    // 删除关联的 PlaybackRegions
    const auto persistentId = mod->persistentId;
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [&persistentId](const PlaybackRegion& region)
                                          {
                                              return persistentId.isNotEmpty()
                                                  && region.audioModificationPersistentId == persistentId;
                                          }),
                           playbackRegions_.end());
    reconcileEditorSelectionPlaybackRegions();

    // 直接按 Host 指针 erase，不先把匹配字段置空
    // persistent-id→ContentKey 映射保持稳定（araPersistentIdsByObjectId_ 不动）
    // 新 Host modification 仍创建新 wrapper
    audioModifications_.erase(std::remove_if(audioModifications_.begin(), audioModifications_.end(),
                                             [audioModification](const AudioModification& m)
                                             {
                                                 return m.audioModification == audioModification;
                                             }),
                              audioModifications_.end());

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion)
{
    auto& region = ensurePlaybackRegion(playbackRegion);
    region.updateFrom(playbackRegion);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    removePlaybackRegion(playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    auto& modification = ensureAudioModification(audioModification);
    auto& region = ensurePlaybackRegion(playbackRegion);
    region.updateFrom(playbackRegion);
    if (region.audioModificationPersistentId.isEmpty())
        region.audioModificationPersistentId = modification.persistentId;
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource)
{
    auto& source = ensureAudioSource(audioSource);
    source.updateFrom(audioSource);
    for (auto& modification : audioModifications_)
        if (modification.audioModification != nullptr && modification.audioModification->getAudioSource() == audioSource)
            modification.attachSource(source);

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                                            juce::ARAContentUpdateScopes scopeFlags)
{
    juce::ignoreUnused(scopeFlags);

    if (auto* source = findAudioSource(audioSource))
    {
        for (auto& modification : audioModifications_)
        {
            if (modification.hasContentState()
                && modification.content->sourceWindow.sourcePersistentId == source->getIdentity().persistentId)
            {
                // Invalidate derived artifacts (CRS, analysis cache) but preserve
                // user-editable modification-scoped truth (notes, pitchCurve, timeGrid, etc.)
                removeCRSArtifactsForModification(modification);
                modification.invalidateDerivedContent();
            }
        }
    }

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                    bool enable)
{
    ensureAudioSource(audioSource).setSampleAccessEnabled(enable);
}

void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                     bool enable)
{
    auto& source = ensureAudioSource(audioSource);
    source.setSampleAccessEnabled(enable);
    // Per plan doc: sample access enable is permission only, not read intent.
    // Do NOT create reader lease here. Lease creation is user-commanded only
    // via requestReadAudioForPlaybackRegions() -> recordRequested().

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    juce::ignoreUnused(audioModification);
    removePlaybackRegion(playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyAudioSource(juce::ARAAudioSource* audioSource)
{
    AudioSource* source = findAudioSource(audioSource);
    const auto persistentId = source != nullptr ? source->getIdentity().persistentId : juce::String();
    audioSources_.erase(std::remove_if(audioSources_.begin(), audioSources_.end(),
                                       [audioSource](const AudioSource& source)
                                       {
                                           return source.matches(audioSource);
                                       }),
                        audioSources_.end());
    if (persistentId.isNotEmpty())
    {
        for (auto& modification : audioModifications_)
        {
            if (modification.hasContentState()
                && modification.content->sourceWindow.sourcePersistentId == persistentId)
            {
                removeCRSArtifactsForModification(modification);
                modification.resetContent();
            }
        }
    }

    refreshRegisteredRenderers(publishModelChange());
}

namespace {
juce::String mapRestoredPersistentId(const juce::String& archivedPersistentId,
                                     const juce::ARARestoreObjectsFilter* filter)
{
    if (archivedPersistentId.isEmpty())
        return {};

    if (filter == nullptr)
        return archivedPersistentId;

    auto* audioModification = filter->getAudioModificationToRestoreStateWithID(
        archivedPersistentId.toRawUTF8());
    if (audioModification == nullptr)
        return {};

    const auto& restoredPersistentId = audioModification->getPersistentID();
    return restoredPersistentId.empty() ? juce::String() : juce::String::fromUTF8(restoredPersistentId.c_str());
}
} // namespace

bool OpenTuneDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                                                const juce::ARARestoreObjectsFilter* filter)
{
    const int magic = input.readInt();
    if (magic != kContentPayloadArchiveMagic)
        return false;

    const int version = input.readInt();
    if (version < kContentPayloadArchiveVersionMin || version > kContentPayloadArchiveVersion)
        return false;

    const int bindingCount = input.readInt();
    if (bindingCount < 0 || bindingCount > kMaxContentPayloadRecords)
        return false;

    // pending 元素直接存 {AudioModification* target, AudioModificationContentState state}
    std::vector<std::pair<AudioModification*, AudioModificationContentState>> pending;
    pending.reserve(bindingCount);

    for (int i = 0; i < bindingCount; ++i)
    {
        const auto archivedPersistentId = input.readString();
        const auto restoredPersistentId = mapRestoredPersistentId(archivedPersistentId, filter);
        const juce::String xmlStr = input.readString();
        if (restoredPersistentId.isEmpty())
            continue;

        auto* targetMod = findAudioModification(restoredPersistentId);
        // 映射成功但当前图无 target wrapper：ARA2 spec 允许忽略，不视为失败
        if (targetMod == nullptr)
            continue;

        // target 存在但 Host pointer 为空或 XML 损坏：真正的数据损坏，必须失败
        if (targetMod->audioModification == nullptr || xmlStr.isEmpty())
            return false;

        auto xml = juce::XmlDocument::parse(xmlStr);
        if (xml == nullptr)
            return false;

        // Parse XML to new content state with ARA filter remapping, then validate.
        // Fail early without changing any target.
        auto newContent = restoreAudioModificationContent(*xml, filter);
        if (!newContent.has_value())
            return false;

        const auto* source = findAudioSource(newContent->sourceWindow.sourcePersistentId);
        if (source == nullptr)
            return false;

        const double sourceDuration = source->getShape().durationSeconds();
        if (!std::isfinite(sourceDuration)
            || newContent->sourceWindow.sourceStartSeconds < 0.0
            || newContent->sourceWindow.sourceEndSeconds > sourceDuration + 1.0e-6)
            return false;

        const auto* targetSource = findAudioSource(targetMod->audioModification->getAudioSource());
        if (targetSource == nullptr || targetSource->getIdentity().persistentId != newContent->sourceWindow.sourcePersistentId)
            return false;

        if (targetMod->hasContentState()
            && targetMod->content->sourceWindow.sourcePersistentId != newContent->sourceWindow.sourcePersistentId)
            return false;

        // 存储原子替换数据，避免第二次解析
        pending.emplace_back(targetMod, std::move(*newContent));
    }

    // 全部成功后对 pending 进行原子替换
    for (auto& [targetMod, state] : pending)
    {
        // 先清理 CRS 派生碎片
        removeCRSArtifactsForModification(*targetMod);

        // 原子替换 content
        targetMod->content = std::move(state);
        targetMod->birthState = AudioModificationBirthState::WaitingForSource;

        // F0 #B.4: 提交完成后再递增 birthRevision 以捕获 F0 completion (#B.3)
        ++targetMod->birthRevision;
    }

    // 提交完成后再通知 Host，避免 Host 观察到半提交状态。
    for (const auto& [targetMod, state] : pending)
    {
        juce::ignoreUnused(state);
        if (targetMod->audioModification != nullptr)
            targetMod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    }

    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::doStoreObjectsToStream(juce::ARAOutputStream& output,
                                                          const juce::ARAStoreObjectsFilter* filter)
{
    std::vector<const AudioModification*> bindings;
    bindings.reserve(audioModifications_.size());

    // Per ARA2 spec: AudioModification is persistent model object.
    // Archive modification-scoped state based on ARA store filter and persistentId,
    // not on CRS derived buffer renderability.
    if (filter == nullptr)
    {
        for (const auto& modification : audioModifications_)
            if (modification.persistentId.isNotEmpty() && modification.hasContentState())
                bindings.push_back(&modification);
    }
    else
    {
        const auto& modsToStore = filter->getAudioModificationsToStore();
        for (const auto& modification : audioModifications_)
        {
            if (modification.persistentId.isEmpty() || !modification.hasContentState())
                continue;

            const auto* araMod = modification.audioModification;
            if (araMod == nullptr)
                continue;

            const auto* basePtr = static_cast<const ARA::PlugIn::AudioModification*>(araMod);
            if (std::find(modsToStore.begin(), modsToStore.end(), basePtr) != modsToStore.end())
                bindings.push_back(&modification);
        }
    }

    if (bindings.size() > static_cast<size_t>(kMaxContentPayloadRecords))
        return false;

    bool ok = output.writeInt(kContentPayloadArchiveMagic);
    ok = output.writeInt(kContentPayloadArchiveVersion) && ok;
    ok = output.writeInt(static_cast<int>(bindings.size())) && ok;

    for (const auto* modification : bindings)
    {
        ok = output.writeString(modification->persistentId) && ok;

        juce::XmlElement el("AudioModificationContent");
        serializeAudioModificationContent(*modification, el);
        ok = output.writeString(el.toString()) && ok;
    }

    return ok;
}

juce::ARAPlaybackRenderer* OpenTuneDocumentController::doCreatePlaybackRenderer()
{
    auto* renderer = new OpenTunePlaybackRenderer(getDocumentController(), this);
    registerPlaybackRenderer(*renderer);
    renderer->refreshRenderPlanFromDocument();
    return renderer;
}

juce::ARAEditorView* OpenTuneDocumentController::doCreateEditorView()
{
    return new OpenTuneEditorView(getDocumentController(), *this);
}

AudioSource* OpenTuneDocumentController::findAudioSource(juce::ARAAudioSource* audioSource)
{
    const auto it = std::find_if(audioSources_.begin(), audioSources_.end(),
                                 [audioSource](const AudioSource& source)
                                 {
                                     return source.matches(audioSource);
                                 });
    return it != audioSources_.end() ? &*it : nullptr;
}

AudioSource* OpenTuneDocumentController::findAudioSource(const juce::String& persistentId)
{
    const auto it = std::find_if(audioSources_.begin(), audioSources_.end(),
                                 [&persistentId](const AudioSource& source)
                                 {
                                     return source.getIdentity().persistentId == persistentId;
                                 });
    return it != audioSources_.end() ? &*it : nullptr;
}

const AudioSource* OpenTuneDocumentController::findAudioSource(const juce::String& persistentId) const
{
    const auto it = std::find_if(audioSources_.begin(), audioSources_.end(),
                                 [&persistentId](const AudioSource& source)
                                 {
                                     return source.getIdentity().persistentId == persistentId;
                                 });
    return it != audioSources_.end() ? &*it : nullptr;
}

AudioSource& OpenTuneDocumentController::ensureAudioSource(juce::ARAAudioSource* audioSource)
{
    if (auto* existing = findAudioSource(audioSource))
        return *existing;

    AudioSource source;
    source.updateFrom(audioSource);
    audioSources_.push_back(std::move(source));
    return audioSources_.back();
}

AudioModification* OpenTuneDocumentController::findAudioModification(const juce::String& persistentId)
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [&persistentId](const AudioModification& modification)
                                 {
                                     return modification.persistentId == persistentId;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

const AudioModification* OpenTuneDocumentController::findAudioModification(const juce::String& persistentId) const
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [&persistentId](const AudioModification& modification)
                                 {
                                     return modification.persistentId == persistentId;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

ContentKey OpenTuneDocumentController::makeAudioModificationContentKey(const juce::String& persistentId)
{
    if (persistentId.isEmpty())
        return {};

    const auto existingId = araObjectIdsByPersistentId_.find(persistentId);
    if (existingId != araObjectIdsByPersistentId_.end())
        return {DomainKind::ARAAudioModification, existingId->second, 0};

    uint64_t objectId = static_cast<uint64_t>(persistentId.hashCode64());
    if (objectId == 0)
        objectId = 1469598103934665603ULL;

    while (true)
    {
        const auto existingPersistentId = araPersistentIdsByObjectId_.find(objectId);
        if (existingPersistentId == araPersistentIdsByObjectId_.end()
            || existingPersistentId->second == persistentId)
            break;

        objectId = objectId * 1099511628211ULL + 1469598103934665603ULL;
        if (objectId == 0)
            objectId = 1;
    }

    araPersistentIdsByObjectId_[objectId] = persistentId;
    araObjectIdsByPersistentId_[persistentId] = objectId;
    return {DomainKind::ARAAudioModification, objectId, 0};
}

ContentKey OpenTuneDocumentController::bindAudioModificationIdentity(AudioModification& modification)
{
    modification.contentIdentity = makeAudioModificationContentKey(modification.persistentId);
    return modification.contentIdentity;
}

const juce::String* OpenTuneDocumentController::findPersistentIdForAudioModificationKey(ContentKey key) const
{
    if (key.domainKind != DomainKind::ARAAudioModification || !key.isValid())
        return nullptr;

    const auto it = araPersistentIdsByObjectId_.find(key.objectId);
    return it != araPersistentIdsByObjectId_.end() ? &it->second : nullptr;
}

AudioModification* OpenTuneDocumentController::findAudioModification(juce::ARAAudioModification* audioModification)
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [audioModification](const AudioModification& modification)
                                 {
                                     return modification.audioModification == audioModification;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

AudioModification* OpenTuneDocumentController::findAudioModificationByContentKey(const ContentKey& key)
{
    const auto* persistentId = findPersistentIdForAudioModificationKey(key);
    if (persistentId != nullptr)
        return findAudioModification(*persistentId);

    for (auto& mod : audioModifications_)
        if (mod.contentKey() == key)
            return &mod;
    return nullptr;
}

const AudioModification* OpenTuneDocumentController::findAudioModificationByContentKey(const ContentKey& key) const
{
    const auto* persistentId = findPersistentIdForAudioModificationKey(key);
    if (persistentId != nullptr)
        return findAudioModification(*persistentId);

    for (const auto& mod : audioModifications_)
        if (mod.contentKey() == key)
            return &mod;
    return nullptr;
}

AudioModification& OpenTuneDocumentController::ensureAudioModification(juce::ARAAudioModification* audioModification)
{
    // 1. 先按 pointer 查找活跃 modification
    if (auto* existing = findAudioModification(audioModification))
    {
        existing->updateIdentity(audioModification);
        bindAudioModificationIdentity(*existing);
        return *existing;
    }

    // 2. Host 新 modification 必须直接创建新 wrapper，不再 rebind 已销毁的 wrapper
    AudioModification modification;
    modification.updateIdentity(audioModification);
    bindAudioModificationIdentity(modification);
    if (audioModification != nullptr)
        if (auto* source = findAudioSource(audioModification->getAudioSource()))
            modification.attachSource(*source);
    audioModifications_.push_back(std::move(modification));
    return audioModifications_.back();
}

PlaybackRegion* OpenTuneDocumentController::findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    const auto it = std::find_if(playbackRegions_.begin(), playbackRegions_.end(),
                                 [playbackRegion](const PlaybackRegion& region)
                                 {
                                     return region.playbackRegion == playbackRegion;
                                 });
    return it != playbackRegions_.end() ? &*it : nullptr;
}

const PlaybackRegion* OpenTuneDocumentController::findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) const
{
    const auto it = std::find_if(playbackRegions_.begin(), playbackRegions_.end(),
                                 [playbackRegion](const PlaybackRegion& region)
                                 {
                                     return region.playbackRegion == playbackRegion;
                                 });
    return it != playbackRegions_.end() ? &*it : nullptr;
}

PlaybackRegion& OpenTuneDocumentController::ensurePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    if (auto* existing = findPlaybackRegion(playbackRegion))
        return *existing;

    PlaybackRegion region;
    region.updateFrom(playbackRegion);
    playbackRegions_.push_back(std::move(region));
    return playbackRegions_.back();
}

OpenTuneDocumentController::PlaybackRegionProjection
OpenTuneDocumentController::makeProjection(const PlaybackRegion& placement) const
{
    PlaybackRegionProjection projection;
    projection.playbackRegion = placement.playbackRegion;
    projection.audioModificationPersistentId = placement.audioModificationPersistentId;
    projection.placementRevision = placement.placementRevision;
    projection.startInPlaybackTime = placement.startInPlaybackTime;
    projection.startInModificationTime = placement.startInModificationTime;
    projection.durationInPlaybackTime = placement.durationInPlaybackTime;
    projection.durationInModificationTime = placement.durationInModificationTime;
    projection.timestretchEnabled = placement.timestretchEnabled;
    projection.timestretchReflectingTempo = placement.timestretchReflectingTempo;
    projection.contentBasedFadeAtHead = placement.contentBasedFadeAtHead;
    projection.contentBasedFadeAtTail = placement.contentBasedFadeAtTail;

    const auto* modification = findAudioModification(placement.audioModificationPersistentId);
    if (modification == nullptr || !modification->hasContentState())
        return projection;

    projection.contentWindow = modification->content->sourceWindow;
    projection.contentRevision = modification->content->contentRevision;
    projection.contentDurationSeconds = modification->content->sourceWindow.durationSeconds();
    projection.contentKey = modification->contentKey();
    // playbackSourceReady 仅用于 renderer 严格 gate（isPlaybackRenderable）；
    // UI projection 不再以它阻断，WaitingForSource 也能产出有效 contentKey/content snapshot。
    projection.playbackSourceReady = modification->isRenderable();

    const auto* source = findAudioSource(modification->content->sourceWindow.sourcePersistentId);
    if (source != nullptr)
    {
        projection.sampleRate = source->getShape().sourceSampleRate;
        projection.numChannels = source->getShape().numChannels;
    }

    return projection;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::buildProjections() const
{
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve(playbackRegions_.size());

    for (const auto& region : playbackRegions_)
    {
        if (region.hasValidPlacement())
            projections.push_back(makeProjection(region));
    }

    return projections;
}

std::vector<OpenTunePlaybackRenderer*> OpenTuneDocumentController::publishModelChange()
{
    return playbackRenderers_;
}

void OpenTuneDocumentController::refreshRegisteredRenderers(const std::vector<OpenTunePlaybackRenderer*>& renderers)
{
    for (auto* renderer : renderers)
        if (renderer != nullptr)
            renderer->refreshRenderPlanFromDocument();
}

void OpenTuneDocumentController::reconcileEditorSelectionPlaybackRegions()
{
    editorSelectionPlaybackRegions_.erase(
        std::remove_if(editorSelectionPlaybackRegions_.begin(),
                       editorSelectionPlaybackRegions_.end(),
                       [this](juce::ARAPlaybackRegion* playbackRegion)
                       {
                           return findPlaybackRegion(playbackRegion) == nullptr;
                       }),
        editorSelectionPlaybackRegions_.end());
}

bool OpenTuneDocumentController::publishPlaybackReadSourceForModification(
    AudioModification& modification,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer)
{
    if (contentRenderService_ == nullptr)
        return false;

    if (audioBuffer == nullptr)
        return false;

    if (!modification.hasContentState())
        return false;

    const auto& content = *modification.content;
    const auto key = modification.contentKey();

    PlaybackReadSource readSource;
    readSource.contentKey = key;
    readSource.renderCache = contentRenderService_->getOrCreateRenderCache(key);
    readSource.audioBuffer = std::move(audioBuffer);
    readSource.audioSampleRate = TimeCoordinate::kRenderSampleRate;
    readSource.timeStretchCache = &contentRenderService_->getTimeStretchCache();
    readSource.pitchRevision = content.editable.pitchRevision;
    readSource.pitchShiftRevision = content.editable.pitchShiftRevision;
    readSource.timeGridRevision = content.editable.timeGridRevision;
    readSource.volumeEnvelope = std::make_shared<const AutomationLane>(content.editable.volumeEnvelope);
    readSource.timeGrid = content.editable.timeGrid->isIdentity()
        ? nullptr
        : content.editable.timeGrid;

    contentRenderService_->publishPlaybackSource(key, readSource);
    return true;
}

bool OpenTuneDocumentController::birthContentForModification(AudioModification& modification)
{
    // F0 #B.4: 在用户 read 开始前取消该 content 的旧 F0 request
    contentF0ExtractionService_->cancel(F0RequestKey{modification.contentKey()});

    // 前置条件：必须已拥有 content 和有效 sourceWindow。
    // restore 路径负责建立 content；这里只读取 AudioSource 并构建 CRS 派生 PCM。
    if (!modification.hasContentState())
        return false;

    auto& content = *modification.content;
    if (content.sourceWindow.sourcePersistentId.isEmpty() || !content.sourceWindow.isValid())
        return false;

    auto* source = findAudioSource(content.sourceWindow.sourcePersistentId);
    if (source == nullptr)
    {
        modification.birthState = AudioModificationBirthState::WaitingForSource;
        return false;
    }

    if (!source->hasReaderLease())
        source->createReaderLease();

    if (!source->canReadSamples())
    {
        modification.birthState = AudioModificationBirthState::WaitingForSource;
        return false;
    }

    modification.birthState = AudioModificationBirthState::Rendering;
    ++modification.birthRevision;

    // 1. Determine source window from AudioSource
    auto readerLease = source->shareReaderLease();
    if (readerLease == nullptr || source->getShape().numChannels <= 0
        || source->getShape().numSamples <= 0 || source->getShape().sourceSampleRate <= 0.0)
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    const auto sourceWindow = content.sourceWindow;

    const double sourceSampleRate = source->getShape().sourceSampleRate;
    const int64_t numSamples = source->getShape().numSamples;
    const int numChannels = source->getShape().numChannels;

    // 2. Read ARA source window
    const int64_t sourceStartSample = static_cast<int64_t>(
        std::round(sourceWindow.sourceStartSeconds * sourceSampleRate));
    const int64_t sourceEndSample = static_cast<int64_t>(
        std::round(sourceWindow.sourceEndSeconds * sourceSampleRate));
    const int64_t windowSamples = std::max<int64_t>(0,
        std::min<int64_t>(sourceEndSample, numSamples)
        - std::max<int64_t>(0, sourceStartSample));

    if (windowSamples <= 0)
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    juce::AudioBuffer<float> playableAccum(numChannels, static_cast<int>(windowSamples));
    playableAccum.clear();

    constexpr int64_t kChunkSamples = 32768;
    {
        int64_t readOffset = sourceStartSample;
        int64_t accumWriteOffset = 0;
        int64_t remaining = windowSamples;

        while (remaining > 0)
        {
            const int64_t chunkSamples = std::min(kChunkSamples, remaining);
            const int accumOffset = static_cast<int>(accumWriteOffset);

            std::vector<void*> channelPointers(static_cast<size_t>(numChannels));
            for (int ch = 0; ch < numChannels; ++ch)
                channelPointers[static_cast<size_t>(ch)] = playableAccum.getWritePointer(ch, accumOffset);

            if (!readerLease->readAudioSamples(readOffset,
                                                  static_cast<int>(chunkSamples),
                                                  channelPointers.data()))
            {
                modification.birthState = AudioModificationBirthState::Failed;
                return false;
            }

            readOffset += chunkSamples;
            accumWriteOffset += chunkSamples;
            remaining -= chunkSamples;
        }
    }

    // 3. Extract ch0 for async F0
    std::vector<float> channel0Data(static_cast<size_t>(playableAccum.getNumSamples()));
    {
        const float* ch0Read = playableAccum.getReadPointer(0);
        std::copy(ch0Read, ch0Read + playableAccum.getNumSamples(), channel0Data.begin());
    }

    // 4. Resample audio to 44.1kHz for derived playback buffer
    // Per ARA2 spec: AudioSource owns original PCM, AudioModification does not.
    // This buffer will be published to CRS as derived playback cache.
    juce::AudioBuffer<float> storedBuffer;
    const double targetSampleRate = TimeCoordinate::kRenderSampleRate;
    if (std::abs(sourceSampleRate - targetSampleRate) > 1.0)
    {
        const int storedLen = juce::jmax(1,
            static_cast<int>(TimeCoordinate::secondsToSamples(
                TimeCoordinate::samplesToSeconds(playableAccum.getNumSamples(), sourceSampleRate),
                targetSampleRate)));

        storedBuffer.setSize(numChannels, storedLen);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto resampledData = resamplingManager_->upsampleForHost(
                playableAccum.getReadPointer(ch),
                playableAccum.getNumSamples(),
                static_cast<int>(sourceSampleRate),
                static_cast<int>(targetSampleRate));
            const int toCopy = juce::jmin(storedLen, static_cast<int>(resampledData.size()));
            storedBuffer.copyFrom(ch, 0, resampledData.data(), toCopy);
        }
    }
    else
    {
        storedBuffer = std::move(playableAccum);
    }

    // 6. Detect silent gaps and write back via owner methods (not direct content.analysis access)
    auto silentGaps = SilentGapDetector::detectAllGapsAdaptive(storedBuffer);
    auto storedAudioBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(storedBuffer));

    modification.submitSilentGaps(std::move(silentGaps));
    modification.applyOriginalF0State(OriginalF0State::NotRequested);

    // 7. Publish to CRS (derived playback cache + resampled audio buffer)
    // Per ARA2 spec: CRS holds derived/cache for renderer fast read,
    // not source audio truth. Original PCM remains in AudioSource.
    if (!publishPlaybackReadSourceForModification(modification, storedAudioBuffer))
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    // 8. Set modification state and notify ARA host
    modification.birthState = AudioModificationBirthState::Ready;
    if (modification.audioModification != nullptr)
        modification.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

    // 9. Schedule async F0 extraction via CRS
    if (contentRenderService_ != nullptr)
    {
        auto* hostModification = modification.audioModification;
        scheduleAsyncF0Extraction(modification.contentKey(), std::move(channel0Data), sourceSampleRate, hostModification);
    }

    return true;
}

void OpenTuneDocumentController::removeCRSArtifactsForModification(const AudioModification& modification)
{
    contentF0ExtractionService_->cancel(F0RequestKey{modification.contentKey()});

    if (contentRenderService_ == nullptr)
        return;

    const auto key = modification.contentKey();
    if (!key.isValid())
        return;

    contentRenderService_->removePlaybackSource(key);
    contentRenderService_->removeRenderCache(key);
    contentRenderService_->removeStretcher(key);
    contentRenderService_->getTimeStretchCache().invalidate(key);
}

// Removed rebuildCRSForSource per architecture: sample access enable is permission,
// not user intent. Source-level scan that infers user intent is prohibited.
// Only requestReadAudioForPlaybackRegions() (user button press) may read host audio.

void OpenTuneDocumentController::scheduleAsyncF0Extraction(
    ContentKey key,
    std::vector<float> channel0Data,
    double sourceSampleRate,
    juce::ARAAudioModification* hostModification)
{
    if (!contentF0ExtractionService_)
        return;

    // 当前 wrapper 或 Host pointer 缺失时不提交任务
    if (hostModification == nullptr)
        return;

    auto f0Svc = ProcessF0Runtime::getInstance().getF0Service();
    if (!f0Svc)
        return;

    auto crs = contentRenderService_;

    // Capture current birthRevision before submission (#B.3)
    uint64_t birthRevision = 0;
    if (auto* mod = findAudioModificationByContentKey(key))
        birthRevision = mod->birthRevision;

    // Submit real ARA AudioModification ContentKey to F0 extraction service
    contentF0ExtractionService_->submit(
        F0RequestKey{key},
        [f0Svc, data = std::move(channel0Data), sourceSampleRate, birthRevision, leaseToken = asyncLeaseToken_](const std::shared_ptr<F0RunOwnerState>& runOwnerState) mutable
        {
            if (leaseToken && !leaseToken->load(std::memory_order_acquire))
                return F0ExtractionService::Result{};

            if (f0Svc == nullptr || data.empty())
                return F0ExtractionService::Result{};

            auto extraction = f0Svc->extractF0(data.data(), data.size(),
                                                static_cast<int>(sourceSampleRate), runOwnerState);

            if (!extraction.ok() || extraction.value().empty())
            {
                return F0ExtractionService::Result{};
            }

            const auto& f0Data = extraction.value();
            const int hopSize = f0Svc->getF0HopSize();
            const int f0SampleRate = f0Svc->getF0SampleRate();

            const auto energy = computeFrameEnergy(
                data.data(), static_cast<int>(data.size()),
                static_cast<int>(sourceSampleRate),
                f0Data, f0SampleRate, hopSize);

            F0ExtractionService::Result result;
            result.success = true;
            result.f0 = f0Data;
            result.hopSize = hopSize;
            result.f0SampleRate = f0SampleRate;
            result.energy = std::move(energy);
            return result;
        },
        [this, crs, key, birthRevision, hostModification, leaseToken = asyncLeaseToken_](F0ExtractionService::Result&& result) mutable
        {
            if (leaseToken && !leaseToken->load(std::memory_order_acquire))
                return;

            if (crs == nullptr)
                return;

            if (!result.success || result.f0.empty())
            {
                // Completion only if Host pointer matches and content still exists with same birthRevision (#B.3)
                if (auto* mod = findAudioModificationByContentKey(key))
                {
                    if (!mod->hasContentState())
                        return;
                    if (mod->birthRevision != birthRevision)
                        return; // Stale completion - modification restarted
                    if (mod->audioModification != hostModification)
                        return; // Stale completion - new Host AudioModification reused same ContentKey + birthRevision

                    mod->applyOriginalF0State(OriginalF0State::Failed);
                    if (mod->audioModification != nullptr)
                        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
                }
                return;
            }

            // Completion only if Host pointer matches and content still exists with same birthRevision (#B.3)
            if (auto* mod = findAudioModificationByContentKey(key))
            {
                if (!mod->hasContentState())
                    return;
                if (mod->birthRevision != birthRevision)
                    return; // Stale completion - modification restarted
                if (mod->audioModification != hostModification)
                    return; // Stale completion - new Host AudioModification reused same ContentKey + birthRevision

                // Rebuild pitchCurve from Result
                auto pitchCurve = std::make_shared<PitchCurve>();
                pitchCurve->setOriginalF0(result.f0);
                pitchCurve->setSampleRate(static_cast<double>(result.f0SampleRate));
                pitchCurve->setHopSize(result.hopSize);
                if (!result.energy.empty())
                    pitchCurve->setOriginalEnergy(result.energy);

                mod->applyOriginalF0(std::move(pitchCurve));
                if (mod->audioModification != nullptr)
                    mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
            }
        });
}

// Per architecture: DC owns CRS and installs render execution lease.
// The lease is detached in destructor to prevent dangling callback.

void OpenTuneDocumentController::installDocumentRenderExecution()
{
    ContentRenderService::ExecutionLease lease;
    lease.owner = this;
    lease.renderJobCallback = [this](RenderJob& job)
    {
        processDocumentRenderJob(job);
    };

    contentRenderService_->attachExecutionLease(std::move(lease));
}

void OpenTuneDocumentController::processDocumentRenderJob(RenderJob& job)
{
    if (job.renderCache == nullptr)
        return;

    auto* mod = findAudioModificationByContentKey(job.contentKey);
    if (mod == nullptr || !mod->isRenderable())
    {
        job.renderCache->completeChunkRenderFailure(job.startSeconds, job.targetRevision);
        return;
    }

    PlaybackReadSource readSource;
    if (contentRenderService_ == nullptr
        || !contentRenderService_->getPlaybackReadSource(job.contentKey, readSource)
        || readSource.audioBuffer == nullptr)
    {
        job.renderCache->completeChunkRenderFailure(job.startSeconds, job.targetRevision);
        return;
    }

    auto snap = snapshotAudioModification(job.contentKey);
    if (!snap)
    {
        job.renderCache->completeChunkRenderFailure(job.startSeconds, job.targetRevision);
        return;
    }

    ProcessRenderRuntime::getInstance().processChunkRenderJob(
        contentRenderService_, job, std::move(snap),
        false, {});
}

std::shared_ptr<const EditableContentSnapshot> OpenTuneDocumentController::snapshotAudioModification(ContentKey key) const
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState())
        return nullptr;

    const auto& content = *mod->content;

    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->audioBuffer = nullptr;
    snap->audioSampleRate = 0.0;
    snap->sourceWindow = content.sourceWindow;
    snap->notes = content.editable.notes;
    snap->pitchCurve = content.analysis.pitchCurve;
    snap->timeGrid = content.editable.timeGrid;
    snap->pitchShiftSettings = content.editable.pitchShiftSettings;
    snap->silentGaps = content.analysis.silentGaps;
    snap->detectedKey = content.analysis.detectedKey;
    snap->referenceFeatures = content.analysis.referenceFeatures;
    snap->originalF0State = content.analysis.originalF0State;
    snap->pitchRevision = content.editable.pitchRevision;
    snap->pitchShiftRevision = content.editable.pitchShiftRevision;
    snap->timeGridRevision = content.editable.timeGridRevision;
    snap->contentRevision = content.contentRevision;
    snap->notesRevision = content.editable.notesRevision;
    return snap;
}

void OpenTuneDocumentController::refreshModificationCRSMetadata(ContentKey key)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState())
        return;

    const auto& content = *mod->content;

    // 刷新 CRS 的 PlaybackReadSource metadata（不重新发布 audio buffer）
    // PlaybackReadSource 只承载播放所需的不可变元数据。
    // 真正的分析态（pitchCurve/timeGrid/silentGaps）由 AudioModification.content 持有，
    // 渲染时通过 snapshotAudioModification 注入到 EditableContentSnapshot，再交给 ProcessRenderRuntime。
    PlaybackReadSource readSource;
    if (contentRenderService_ && contentRenderService_->getPlaybackReadSource(key, readSource))
    {
        readSource.pitchRevision = content.editable.pitchRevision;
        readSource.pitchShiftRevision = content.editable.pitchShiftRevision;
        readSource.timeGridRevision = content.editable.timeGridRevision;
        readSource.timeGrid = content.editable.timeGrid->isIdentity()
            ? nullptr
            : content.editable.timeGrid;
        contentRenderService_->publishPlaybackSource(key, std::move(readSource));
    }
}

void OpenTuneDocumentController::requestModificationRender(ContentKey key, double startSeconds, double endSeconds)
{
    if (contentRenderService_ == nullptr)
        return;

    PlaybackReadSource readSource;
    if (!contentRenderService_->getPlaybackReadSource(key, readSource))
        return;

    if (readSource.audioBuffer == nullptr || readSource.audioSampleRate <= 0.0)
        return;

    const int startSample = static_cast<int>(startSeconds * readSource.audioSampleRate);
    const int endSample = static_cast<int>(endSeconds * readSource.audioSampleRate);

    // Enqueue render request: job carries content identity and sample range.
    // RenderWorker resolves start/end/targetRevision from PendingJob when executing.
    auto snap = snapshotAudioModification(key);

    RenderJob job;
    job.contentKey = key;
    job.audioBuffer = readSource.audioBuffer;
    job.audioSampleRate = readSource.audioSampleRate;
    job.startSample = startSample;
    job.endSampleExclusive = endSample;
    job.renderCache = contentRenderService_->getOrCreateRenderCache(key);

    // Silent gaps carry chunk-planning metadata into enqueueRender.
    if (snap)
    {
        job.silentGaps = snap->silentGaps;
    }

    contentRenderService_->enqueueRender(std::move(job));
}

void OpenTuneDocumentController::requestFullModificationRender(ContentKey key)
{
    if (contentRenderService_ == nullptr)
        return;

    PlaybackReadSource readSource;
    if (!contentRenderService_->getPlaybackReadSource(key, readSource))
        return;

    if (readSource.audioBuffer == nullptr || readSource.audioSampleRate <= 0.0)
        return;

    const int totalSamples = readSource.audioBuffer->getNumSamples();
    const double totalSeconds = static_cast<double>(totalSamples) / readSource.audioSampleRate;

    requestModificationRender(key, 0.0, totalSeconds);
}

bool OpenTuneDocumentController::removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    const auto oldSize = playbackRegions_.size();
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [playbackRegion](const PlaybackRegion& region)
                                          {
                                              return region.playbackRegion == playbackRegion;
                                          }),
                           playbackRegions_.end());
    return playbackRegions_.size() != oldSize;
}

// RestoredContentBinding removed — ARA archive now uses ContentKey + content payload.
// Legacy archive records are skipped during doRestoreObjectsFromStream.

bool OpenTuneDocumentController::requestSetPlaybackPosition(double timeInSeconds)
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestSetPlaybackPosition(timeInSeconds);
    return true;
}

bool OpenTuneDocumentController::requestStartPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestStartPlayback();
    return true;
}

bool OpenTuneDocumentController::requestStopPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestStopPlayback();
    return true;
}

bool OpenTuneDocumentController::requestEnableCycle(bool enabled)
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestEnableCycle(enabled);
    return true;
}

bool OpenTuneDocumentController::requestSetCycleRange(double startTime, double duration)
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestSetCycleRange(startTime, duration);
    return true;
}

// -----------------------------------------------------------------------
// 编辑器只读内容访问器实现
// -----------------------------------------------------------------------

std::shared_ptr<const juce::AudioBuffer<float>> OpenTuneDocumentController::readAudioBuffer(ContentKey key) const
{
    PlaybackReadSource crsSrc;
    if (contentRenderService_ != nullptr && contentRenderService_->getPlaybackReadSource(key, crsSrc) && crsSrc.audioBuffer != nullptr)
        return crsSrc.audioBuffer;
    return nullptr;
}

std::shared_ptr<PitchCurve> OpenTuneDocumentController::readPitchCurve(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return nullptr;
    return mod->content->analysis.pitchCurve;
}

OriginalF0State OpenTuneDocumentController::readOriginalF0State(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return OriginalF0State::NotRequested;
    return mod->content->analysis.originalF0State;
}

DetectedKey OpenTuneDocumentController::readDetectedKey(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return {};
    return mod->content->analysis.detectedKey;
}

std::vector<Note> OpenTuneDocumentController::readNotes(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return {};
    return mod->content->editable.notes;
}

uint64_t OpenTuneDocumentController::readNotesRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return 0;
    return mod->content->editable.notesRevision;
}

std::shared_ptr<const TimeGridSnapshot> OpenTuneDocumentController::readTimeGrid(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return nullptr;
    return mod->content->editable.timeGrid;
}

uint64_t OpenTuneDocumentController::readTimeGridRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return 0;
    return mod->content->editable.timeGridRevision;
}

PitchShiftSettings OpenTuneDocumentController::readPitchShift(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return {};
    return mod->content->editable.pitchShiftSettings;
}

uint64_t OpenTuneDocumentController::readContentRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return 0;
    return mod->content->contentRevision;
}

double OpenTuneDocumentController::readContentDuration(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return 0.0;
    return mod->content->sourceWindow.durationSeconds();
}

bool OpenTuneDocumentController::hasContent(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    return mod != nullptr && mod->hasContentState();
}

std::shared_ptr<const EditableContentSnapshot>
OpenTuneDocumentController::readContentSnapshot(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return nullptr;
    return mod->snapshotContent();
}

// ARA mutation API implementations — Processor delegates ARA writes here

bool OpenTuneDocumentController::applyNotesToModification(const ContentKey& key, std::vector<Note> notes)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyNotes(std::move(notes));
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyVolumeEnvelopeToModification(const ContentKey& key,
                                                                   AutomationLane envelope)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyVolumeEnvelope(envelope);

    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

    refreshRegisteredRenderers(publishModelChange());
    return true;
}

void OpenTuneDocumentController::republishPlaybackSourceForModification(ContentKey key)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState())
        return;
    if (contentRenderService_ == nullptr)
        return;

    PlaybackReadSource readSource;
    if (!contentRenderService_->getPlaybackReadSource(key, readSource))
        return;
    if (readSource.audioBuffer == nullptr || readSource.audioBuffer->getNumSamples() <= 0)
        return;

    const auto& content = *mod->content;
    readSource.renderCache = contentRenderService_->getOrCreateRenderCache(key);
    readSource.pitchRevision = content.editable.pitchRevision;
    readSource.pitchShiftRevision = content.editable.pitchShiftRevision;
    readSource.timeGridRevision = content.editable.timeGridRevision;
    readSource.volumeEnvelope = std::make_shared<const AutomationLane>(content.editable.volumeEnvelope);
    readSource.timeGrid = content.editable.timeGrid->isIdentity()
        ? nullptr
        : content.editable.timeGrid;

    contentRenderService_->publishPlaybackSource(key, std::move(readSource));
}

bool OpenTuneDocumentController::applyPitchCurveToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyPitchCurve(std::move(curve));
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyOriginalF0ToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyOriginalF0(std::move(curve));

    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

    // OriginalF0 只更新分析数据，不触发音频渲染，所以不调用 refreshRegisteredRenderers
    return true;
}

bool OpenTuneDocumentController::applyTimeGridToModification(const ContentKey& key, std::shared_ptr<const TimeGridSnapshot> grid)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState() || grid == nullptr) return false;
    if (!mod->applyTimeGrid(std::move(grid)))
        return false;
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyPitchShiftStateToModification(const ContentKey& key,
                                                                     const PitchShiftEditState& state)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    if (!mod->applyPitchShiftState(state)) return false;
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyDetectedKeyToModification(const ContentKey& key, const DetectedKey& detectedKey)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyDetectedKey(detectedKey);
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyReferenceFeaturesToModification(const ContentKey& key, const ReferenceFeatureSet& features)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyReferenceFeatures(features);
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyOriginalF0StateToModification(const ContentKey& key, const OriginalF0State& state)
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyOriginalF0State(state);
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

} // namespace OpenTune

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<OpenTune::OpenTuneDocumentController>();
}

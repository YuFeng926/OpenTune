#include "OpenTuneDocumentController.h"

#include "OpenTuneEditorView.h"
#include "OpenTunePlaybackRenderer.h"

#include "../Inference/F0InferenceService.h"
#include "../Runtime/ProcessF0Runtime.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Services/F0ExtractionService.h"
#include "../Services/ImportedClipF0Extraction.h"
#include "../DSP/F0KeyDetector.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"
#include "../Utils/SilentGapDetector.h"
#include "../Utils/PitchCurve.h"
#include "../Inference/RenderCache.h"
#include "../Render/Stage2TimeStretchRebuilder.h"
#include "../Utils/SourceWindow.h"
#include "../Utils/AppLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "../Content/ContentSnapshotProjection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace OpenTune {

namespace {

constexpr int kContentPayloadArchiveMagic = 0x4F544143;
constexpr int kContentPayloadArchiveVersion = 7; // v7: EqFilter.paletteSlot (v6 dynamic EQ filters migrated with deterministic slot assignment)
constexpr int kContentPayloadArchiveVersionMin = 3;
constexpr int kMaxContentPayloadRecords = 4096;

// clone hook 深拷贝项目 content：经 snapshot 投影复制值字段和可编辑真相，
// 可变 PitchCurve 由 contentStateFromSnapshot 重建独立实例（不共享），
// TimeGridSnapshot/分析标量按现有 const 快照语义共享。
// 产物是新 ContentState：contentRevision 从默认 1 开始，不继承源运行时 revision。
ContentState cloneModificationContent(const ContentState& source)
{
    ContentState copy = contentStateFromSnapshot(makeContentSnapshot(source));
    const bool hasCompletedOriginalF0 =
        source.analysis.originalF0State == OriginalF0State::Ready
        && source.analysis.pitchCurve != nullptr
        && source.analysis.pitchCurve->hasOriginalF0Data();

    if (!hasCompletedOriginalF0)
    {
        // 不复制没有对应异步任务的中间分析状态；可编辑内容仍完整保留。
        copy.analysis = {};
    }

    return copy;
}

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
    completionGate_ = std::make_shared<ProcessRenderRuntime::CompletionGate>();
    installDocumentRenderExecution();
    AppLogger::logNoThrow("ARA-DIAG: DocumentController created dc="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)));

}

OpenTuneDocumentController::~OpenTuneDocumentController()
{
    // 撤销服务租约，防止异步 F0 completion 写回已析构的 DC
    if (asyncLeaseToken_)
        asyncLeaseToken_->store(false, std::memory_order_release);

    // 关闭 Stage1→Stage2 completion gate：持锁置 closed，与 worker 的
    // notifyChunkSettled（持同一把锁调用回调）互斥，杜绝回调访问已析构的 this。
    {
        std::lock_guard<std::mutex> lk(completionGate_->mutex);
        completionGate_->closed = true;
    }

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
}

namespace {
void serializeAudioModificationContent(const AudioModification& mod, juce::XmlElement& el)
{
    el.setAttribute("birthRevision", juce::String(static_cast<juce::int64>(mod.birthRevision)));

    auto* sw = new juce::XmlElement("SourceWindow");
    sw->setAttribute("sourcePersistentId", mod.content->sourceWindow.sourcePersistentId);
    // Note: numeric sourceId is NOT serialized in ARA domain (it's for Capture/Standalone only)
    // ARA domain uses AudioSource persistentID only
    sw->setAttribute("startSeconds", mod.content->sourceWindow.sourceStartSeconds);
    sw->setAttribute("durationSeconds", mod.content->sourceWindow.durationSeconds());
    el.addChildElement(sw);

    auto* editable = new juce::XmlElement("EditableContent");
    editable->setAttribute("notesRevision", juce::String(static_cast<juce::int64>(mod.content->notesRevision)));
    editable->setAttribute("pitchRevision", juce::String(static_cast<juce::int64>(mod.content->pitchRevision)));
    editable->setAttribute("timeGridRevision", juce::String(static_cast<juce::int64>(mod.content->timeGridRevision)));
    editable->setAttribute("noteTopologyInitialized", mod.content->noteTopologyInitialized ? 1 : 0);

    for (const auto& note : mod.content->notes)
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
        
        // v6: Per-note EQ settings — active + Filter 子节点列表
        if (note.eq.has_value()) {
            auto* eqEl = new juce::XmlElement("EqSettings");
            eqEl->setAttribute("active", note.eq->active ? 1 : 0);
            for (const auto& f : note.eq->filters) {
                auto* fEl = new juce::XmlElement("Filter");
                fEl->setAttribute("type", static_cast<int>(f.type));
                fEl->setAttribute("frequencyHz", f.frequencyHz);
                fEl->setAttribute("gainDb", f.gainDb);
                fEl->setAttribute("q", f.q);
                fEl->setAttribute("slot", f.paletteSlot);
                if (f.bypassed)
                    fEl->setAttribute("bypassed", 1);
                eqEl->addChildElement(fEl);
            }
            n->addChildElement(eqEl);
        }
        
        editable->addChildElement(n);
    }

    // Volume Envelope（AutomationLane，revision 不落盘）
    auto* env = new juce::XmlElement("VolumeEnvelope");
    env->setAttribute("pointCount", static_cast<int>(mod.content->volumeEnvelope.points().size()));
    for (const auto& point : mod.content->volumeEnvelope.points())
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
            s->setAttribute("retuneSpeed", seg.parameterSnapshot.retuneSpeed);
            s->setAttribute("pitchDriftScale", seg.parameterSnapshot.pitchDriftScale);
            s->setAttribute("vibratoDepth", seg.parameterSnapshot.vibratoDepth);
            s->setAttribute("vibratoRate", seg.parameterSnapshot.vibratoRate);
            const juce::MemoryBlock f0Data(seg.f0Data.data(), seg.f0Data.size() * sizeof(float));
            s->setAttribute("f0Base64", f0Data.toBase64Encoding());
            editable->addChildElement(s);
        }
    }

    auto* ps = new juce::XmlElement("PitchShiftSettings");
    ps->setAttribute("semitone", mod.content->pitchShiftSettings.semitone);
    ps->setAttribute("cents", mod.content->pitchShiftSettings.cents);
    editable->addChildElement(ps);

    // TimeGrid: owner invariant 保证完整 content 的 timeGrid 非空，无条件写出
    {
        auto* tg = new juce::XmlElement("TimeGrid");
        for (const auto& h : mod.content->timeGrid->handles())
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
    analysis->setAttribute("analysisRevision", juce::String(static_cast<juce::int64>(mod.content->analysis.analysisRevision)));
    analysis->setAttribute("originalF0State", static_cast<int>(mod.content->analysis.originalF0State));
    
    // DetectedKey
    auto* dk = new juce::XmlElement("DetectedKey");
    dk->setAttribute("root", static_cast<int>(mod.content->analysis.detectedKey.root));
    dk->setAttribute("scale", static_cast<int>(mod.content->analysis.detectedKey.scale));
    dk->setAttribute("confidence", mod.content->analysis.detectedKey.confidence);
    dk->setAttribute("origin", static_cast<int>(mod.content->analysis.detectedKey.origin));
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

// Parse XML to new ContentState with complete replacement semantics.
// Missing optional children naturally result in default/empty state.
// Per ARA2 spec: Maps archived source persistentID to current source persistentID via filter.
std::optional<ContentState> restoreAudioModificationContent(const juce::XmlElement& el,
                                                                               const juce::ARARestoreObjectsFilter* filter,
                                                                               int archiveVersion)
{
    ContentState content;
    std::vector<PitchCorrectionSegment> restoredCorrectionSegments;

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
        content.notesRevision = static_cast<uint64_t>(editable->getStringAttribute("notesRevision").getLargeIntValue());
        content.pitchRevision = static_cast<uint64_t>(editable->getStringAttribute("pitchRevision").getLargeIntValue());
        content.timeGridRevision = static_cast<uint64_t>(editable->getStringAttribute("timeGridRevision").getLargeIntValue());

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
            note.vibratoDepth = static_cast<float>(n->getDoubleAttribute(
                "vibratoDepth", PitchControlConfig::kDefaultVibratoDepth));
            note.vibratoRate = static_cast<float>(n->getDoubleAttribute(
                "vibratoRate", PitchControlConfig::kDefaultVibratoRateHz));

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
            
            // v5 起 Note.eq 契约：EqSettings 子元素
            // v5: scalar 9 字段 → 迁移为 5 个固定过滤器
            // v6: Filter 子节点列表
            if (archiveVersion >= 5)
            {
                if (auto* eqEl = n->getChildByName("EqSettings"))
                {
                    if (archiveVersion >= 6 && !eqEl->hasAttribute("active"))
                        return std::nullopt;
                    if (eqEl->hasAttribute("active"))
                    {
                        EqSettings eq;
                        eq.active = eqEl->getIntAttribute("active") != 0;
                        bool parsedEq = false;

                        if (archiveVersion >= 6) {
                            // v6: 当前格式必须完整合法，不能静默截断或跳过损坏的 Filter。
                            eq.filters.clear();
                            int filterCount = 0;
                            for (auto* ignored : eqEl->getChildWithTagNameIterator("Filter")) {
                                juce::ignoreUnused(ignored);
                                ++filterCount;
                            }
                            parsedEq = filterCount > 0
                                    && filterCount <= EqSettings::kMaxFilters
                                    && filterCount == eqEl->getNumChildElements();
                            for (auto* fEl : eqEl->getChildWithTagNameIterator("Filter"))
                            {
                                if (!parsedEq)
                                    break;
                                if (!fEl->hasAttribute("type")
                                    || !fEl->hasAttribute("frequencyHz")
                                    || !fEl->hasAttribute("gainDb")
                                    || !fEl->hasAttribute("q")) {
                                    parsedEq = false;
                                    break;
                                }
                                const int typeInt = fEl->getIntAttribute("type", 0);
                                if (typeInt < 0 || typeInt > static_cast<int>(EqFilterType::HighCut)) {
                                    parsedEq = false;
                                    break;
                                }
                                const int filterIndex = static_cast<int>(eq.filters.size());
                                EqFilter f;
                                f.type = static_cast<EqFilterType>(typeInt);
                                f.frequencyHz = static_cast<float>(
                                    fEl->getDoubleAttribute("frequencyHz", 1000.0));
                                f.gainDb = static_cast<float>(
                                    fEl->getDoubleAttribute("gainDb", 0.0));
                                f.q = static_cast<float>(
                                    fEl->getDoubleAttribute("q", 2.0));
                                // v7: paletteSlot 属性；v6 旧数据按索引确定性补 slot
                                f.paletteSlot = fEl->hasAttribute("slot")
                                    ? fEl->getIntAttribute("slot", filterIndex)
                                    : filterIndex;
                                f.bypassed = fEl->getIntAttribute("bypassed", 0) != 0;
                                eq.filters.push_back(f);
                            }
                        } else if (eqEl->hasAttribute("lowCutFrequencyHz")
                                && eqEl->hasAttribute("lowShelfFrequencyHz")
                                && eqEl->hasAttribute("lowShelfGainDb")
                                && eqEl->hasAttribute("peakFrequencyHz")
                                && eqEl->hasAttribute("peakGainDb")
                                && eqEl->hasAttribute("highShelfFrequencyHz")
                                && eqEl->hasAttribute("highShelfGainDb")
                                && eqEl->hasAttribute("highCutFrequencyHz")) {
                            // v5 legacy: scalar 9 字段 → 5 个固定过滤器
                            const float lowCutFreq = static_cast<float>(
                                eqEl->getDoubleAttribute("lowCutFrequencyHz", 80.0));
                            const float lowShelfFreq = static_cast<float>(
                                eqEl->getDoubleAttribute("lowShelfFrequencyHz", 500.0));
                            const float lowShelfGain = static_cast<float>(
                                eqEl->getDoubleAttribute("lowShelfGainDb", 0.0));
                            const float peakFreq = static_cast<float>(
                                eqEl->getDoubleAttribute("peakFrequencyHz", 3000.0));
                            const float peakGain = static_cast<float>(
                                eqEl->getDoubleAttribute("peakGainDb", 0.0));
                            const float highShelfFreq = static_cast<float>(
                                eqEl->getDoubleAttribute("highShelfFrequencyHz", 8000.0));
                            const float highShelfGain = static_cast<float>(
                                eqEl->getDoubleAttribute("highShelfGainDb", 0.0));
                            const float highCutFreq = static_cast<float>(
                                eqEl->getDoubleAttribute("highCutFrequencyHz", 12000.0));

                            eq.filters = {
                                { EqFilterType::LowCut,   lowCutFreq,   0.0f,    0.707f, 0 },
                                { EqFilterType::LowShelf, lowShelfFreq, lowShelfGain, 2.0f, 1 },
                                { EqFilterType::Peak,     peakFreq,     peakGain, 2.0f, 2 },
                                { EqFilterType::HighShelf,highShelfFreq,highShelfGain, 2.0f, 3 },
                                { EqFilterType::HighCut,  highCutFreq,  0.0f,    0.707f, 4 }
                            };
                            parsedEq = true;
                        }

                        if (archiveVersion >= 6 && (!parsedEq || !eq.isValid()))
                            return std::nullopt;
                        if (parsedEq && eq.isValid())
                            note.eq = eq;
                    }
                }
            }
            
            content.notes.push_back(note);
        }

        // 旧归档无该字段时，以是否有音符为准，避免覆盖已有音符
        content.noteTopologyInitialized =
            editable->getIntAttribute("noteTopologyInitialized", content.notes.empty() ? 0 : 1) != 0;

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
            content.volumeEnvelope = AutomationLane::fromSnapshot(points);
        } else {
            const auto noteGainLane = AutomationLane::fromLegacyNoteGains(content.notes);
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
                content.volumeEnvelope = AutomationLane::sum(
                    noteGainLane, AutomationLane::fromLegacyStepPoints(points));
            } else {
                content.volumeEnvelope = noteGainLane;
            }
        }
        for (auto& note : content.notes)
            note.outputGainDb = content.volumeEnvelope.evalAt(note.startTime);

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

            seg.parameterSnapshot.retuneSpeed = static_cast<float>(s->getDoubleAttribute("retuneSpeed"));
            seg.parameterSnapshot.pitchDriftScale = static_cast<float>(s->getDoubleAttribute("pitchDriftScale", 1.0));
            seg.parameterSnapshot.vibratoDepth = static_cast<float>(s->getDoubleAttribute("vibratoDepth"));
            seg.parameterSnapshot.vibratoRate = static_cast<float>(s->getDoubleAttribute("vibratoRate"));

            // 再次验证浮点值的有效性
            if (!std::isfinite(seg.parameterSnapshot.retuneSpeed) || !std::isfinite(seg.parameterSnapshot.vibratoDepth) || !std::isfinite(seg.parameterSnapshot.vibratoRate) || !std::isfinite(seg.parameterSnapshot.pitchDriftScale))
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

            content.pitchShiftSettings.semitone = semitone;
            content.pitchShiftSettings.cents = cents;
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

        auto timeGrid = TimeGridSnapshot::makeFromHandles(std::move(handles));
        if (timeGrid == nullptr)
            return std::nullopt;

        content.timeGrid = std::move(timeGrid);

        // 完整性校验：TimeGrid 总时长与 sourceWindow 时长误差 <= 1e-6
        const double timeGridTotal = content.timeGrid->totalDurationSeconds();
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
        const int originalF0State = analysis->getIntAttribute("originalF0State");

        // 有效性范围检查
        if (originalF0State < static_cast<int>(OriginalF0State::NotRequested) || originalF0State > static_cast<int>(OriginalF0State::Failed))
            return std::nullopt;

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
            const int origin = dk->getIntAttribute("origin", -1);
            // 旧数据无 origin 字段：按 confidence 迁移
            content.analysis.detectedKey.origin = origin < 0
                ? DetectedKey::originFromLegacyConfidence(static_cast<float>(confidence))
                : static_cast<Origin>(origin);
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
                note.vibratoDepth = static_cast<float>(n->getDoubleAttribute(
                    "vibratoDepth", PitchControlConfig::kDefaultVibratoDepth));
                note.vibratoRate = static_cast<float>(n->getDoubleAttribute(
                    "vibratoRate", PitchControlConfig::kDefaultVibratoRateHz));
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
    return contentKey.isValid()
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

int OpenTuneDocumentController::requestReadAudioForPlaybackRegion(
    juce::ARAPlaybackRegion* playbackRegion)
{
    if (playbackRegion == nullptr)
        return -1;

    if (findPlaybackRegion(playbackRegion) == nullptr)
        return -1;

    auto* modification = findAudioModification(playbackRegion->getAudioModification());
    if (modification == nullptr)
        return -1;

    // 构造当前输入 stamp，基于 FCPE 实际输入内容版本判定
    auto* source = findAudioSource(modification->audioModification != nullptr
                                       ? modification->audioModification->getAudioSource()
                                       : nullptr);
    const auto currentStamp = makeF0InputStamp(
        source != nullptr ? *source : AudioSource{},
        *modification);

    const bool hasValidF0 = modification->hasContentState()
        && modification->content->analysis.originalF0State == OriginalF0State::Ready
        && modification->content->analysis.pitchCurve != nullptr
        && modification->content->analysis.pitchCurve->hasOriginalF0Data();

    const bool sameStamp = modification->originalF0InputStamp.has_value()
        && modification->originalF0InputStamp.value() == currentStamp;

    // 同输入已 materialized：no-op
    if (sameStamp && hasValidF0
        && modification->birthState == AudioModificationBirthState::Ready)
    {
        AppLogger::log("ReadAudio: same-input no-op, key="
            + juce::String(static_cast<juce::int64>(modification->contentKey().objectId)));
        return 0;
    }

    // 同输入正在提取 F0：no-op
    if (sameStamp
        && modification->hasContentState()
        && modification->content->analysis.originalF0State == OriginalF0State::Extracting)
    {
        AppLogger::log("ReadAudio: same-input extracting no-op, key="
            + juce::String(static_cast<juce::int64>(modification->contentKey().objectId)));
        return 0;
    }

    // 同输入正在构建 CRS：no-op
    if (sameStamp && modification->birthState == AudioModificationBirthState::Rendering)
    {
        AppLogger::log("ReadAudio: same-input rendering no-op, key="
            + juce::String(static_cast<juce::int64>(modification->contentKey().objectId)));
        return 0;
    }

    // 同输入 + 有效旧 F0 + 未 materialize：调用 birthContentForModification
    // 保留 archive F0、只重建 CRS，不重新提交 FCPE
    if (sameStamp && hasValidF0
        && (modification->birthState == AudioModificationBirthState::WaitingForSource
            || modification->birthState == AudioModificationBirthState::Failed))
    {
        AppLogger::log("ReadAudio: same-input materialize, key="
            + juce::String(static_cast<juce::int64>(modification->contentKey().objectId)));
        if (!birthContentForModification(*modification))
            return -1;
        refreshRegisteredRenderers(publishModelChange());
        return 1;
    }

    // stamp 不同或没有有效 F0：force-refresh
    if (modification->originalF0InputStamp.has_value()
        && modification->originalF0InputStamp.value() != currentStamp)
    {
        AppLogger::log("ReadAudio: force-refresh, key="
            + juce::String(static_cast<juce::int64>(modification->contentKey().objectId))
            + " oldGen=" + juce::String(static_cast<juce::int64>(modification->originalF0InputStamp->sourceSampleGeneration))
            + " newGen=" + juce::String(static_cast<juce::int64>(currentStamp.sourceSampleGeneration)));
    }
    else if (!hasValidF0)
    {
        AppLogger::log("ReadAudio: no valid F0, birth, key="
            + juce::String(static_cast<juce::int64>(modification->contentKey().objectId)));
    }
    else
    {
        AppLogger::log("ReadAudio: untracked force-refresh, key="
            + juce::String(static_cast<juce::int64>(modification->contentKey().objectId)));
    }

    // force-refresh 前确认 modification 有 content state 且 source 有效；
    // 不在无效 source 情况下先清掉旧内容
    if (!modification->hasContentState())
        return -1;
    if (source == nullptr || !source->getShape().isValid())
        return -1;

    // 取消旧 F0 任务、清除 CRS 派生物、invalidateDerivedContent 清除旧 pitchCurve
    removeCRSArtifactsForModification(*modification);
    modification->invalidateDerivedContent();
    // invalidateDerivedContent 已清除 analysis（含 pitchCurve）和 originalF0InputStamp

    if (modification->birthState != AudioModificationBirthState::WaitingForSource
        && modification->birthState != AudioModificationBirthState::Failed)
        return -1;

    // 设置当前 stamp
    modification->originalF0InputStamp = currentStamp;

    if (!birthContentForModification(*modification))
        return -1;

    refreshRegisteredRenderers(publishModelChange());
    return 1;
}

void OpenTuneDocumentController::requestReadAudioForPlaybackRegionAsync(
    juce::ARAPlaybackRegion* playbackRegion,
    std::function<void(int)> completionCallback)
{
    // ARA SDK requires DocumentController operations on main thread.
    // This method now executes synchronously to comply with ARA thread constraints.
    // Callers should display a loading overlay before calling if UI responsiveness is needed.
    const int count = requestReadAudioForPlaybackRegion(playbackRegion);
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

void OpenTuneDocumentController::didUpdateRegionSequenceProperties(juce::ARARegionSequence* regionSequence)
{
    // RegionSequence 属性（颜色）变化：projection 每次从 SDK 对象实时读取
    // getEffectiveColor，无需 wrapper 缓存同步；这里只刷新 renderer plan。
    juce::ignoreUnused(regionSequence);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willBeginEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
}

void OpenTuneDocumentController::didEndEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
    readRestoredAudio(nullptr);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioModificationProperties(juce::ARAAudioModification* audioModification)
{
    // 唯一 identity binding 点：doCreateAudioModification hook 只按 host pointer
    // 注册 DTO，persistentID 在其返回后由 SDK updateProperties 写入。
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

    // 删除关联的 PlaybackRegions。Per ARA2 spec, host 必须先销毁 region 再销毁
    // modification，此处可直接通过 host pointer 判断归属。
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [audioModification](const PlaybackRegion& region)
                                          {
                                              return region.playbackRegion != nullptr
                                                  && region.playbackRegion->getAudioModification() == audioModification;
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
    ensurePlaybackRegion(playbackRegion);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    removePlaybackRegion(playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification* /*audioModification*/,
    juce::ARAPlaybackRegion* playbackRegion)
{
    // 归属 relationship 由 SDK object 持有，不再写 wrapper 缓存字段；
    // 只保留 host pointer 生命周期索引。
    ensurePlaybackRegion(playbackRegion);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didAddAudioSourceToDocument(juce::ARADocument* /*document*/, juce::ARAAudioSource* audioSource)
{
    ensureAudioSource(audioSource);
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
    if (auto* source = findAudioSource(audioSource))
    {
        // Per ARA2 spec (ARAInterface.h:1145-1151):
        // kARAContentUpdateSignalScopeRemainsUnchanged means actual signal is unaffected.
        // When signal is unchanged (e.g. window reopen after archive restore):
        //   - Preserve analysis (pitchCurve, originalF0) — these remain valid
        //   - Skip removeCRSArtifactsForModification — it cancels F0 tasks, contradicting preservation
        //   - CRS playback cache 重建：新内容由用户 Read 触发；archive 恢复且
        //     已有有效 F0 的内容在 endEditing/access 后自动重建 PCM
        // When signal changed:
        //   - Invalidate ALL derived artifacts (CRS + analysis)
        const bool signalChanged = scopeFlags.affectSamples();

        if (signalChanged)
            source->incrementSampleContentGeneration();

        for (auto& modification : audioModifications_)
        {
            if (modification.hasContentState()
                && modification.content->sourceWindow.sourcePersistentId == source->getIdentity().persistentId)
            {
                if (signalChanged)
                {
                    removeCRSArtifactsForModification(modification);
                    modification.invalidateDerivedContent();
                }
                // else: signal unchanged — preserve everything; PCM 由 readRestoredAudio / 用户 Read 重建
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
    // ARA Audio Source Management callback 内建立 HostAudioReader lease；
    // 实际 sample read 在用户 Read 或 restore 自动 materialization 的非实时路径进行。
    if (enable)
        source.createReaderLease();

    // 编辑会话结束后到达的 access：archive 恢复且已有有效 F0 的内容自动重建 PCM。
    // 编辑会话内（isHostEditingDocument）不提前读，由 didEndEditing 统一处理。
    if (enable && !getDocumentController()->isHostEditingDocument())
        readRestoredAudio(&source);

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

    // pending 元素直接存 {AudioModification* target, ContentState state}
    std::vector<std::pair<AudioModification*, ContentState>> pending;
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
        auto newContent = restoreAudioModificationContent(*xml, filter, version);
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

        // 原子替换 content（新恢复的 ContentState 携带默认运行时 revision 1）
        targetMod->content = std::move(state);
        targetMod->birthState = AudioModificationBirthState::WaitingForSource;
        ++targetMod->birthRevision;
    }

    // 提交完成后再通知 Host，避免 Host 观察到半提交状态。
    // Per ARA2 spec (ARAInterface.h:2223-2228): archive restore must not notify Host.
    // Host is already aware of the restore that triggered this call.
    // refreshRegisteredRenderers() is sufficient to update plugin-internal views.

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

juce::ARAAudioModification* OpenTuneDocumentController::doCreateAudioModification(
    juce::ARAAudioSource* audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification* optionalModificationToClone)
{
    auto* modification = new juce::ARAAudioModification(audioSource, hostRef, optionalModificationToClone);

    // SDK 在本 hook 返回后才 updateProperties（写入新 persistentID）并调用
    // didUpdateAudioModificationProperties。此处只按 host pointer 注册 DTO：
    // identity binding 由 didUpdate 唯一负责，clone 内容不得依赖 persistent ID。
    AudioModification dto;
    dto.audioModification = modification;

    if (optionalModificationToClone != nullptr)
    {
        auto* sourceDto = findAudioModification(
            const_cast<juce::ARAAudioModification*>(optionalModificationToClone));
        if (sourceDto != nullptr && sourceDto->hasContentState())
        {
            // 深拷贝项目 content，不读 PCM、不启动 F0、不通知 host。
            dto.content = cloneModificationContent(*sourceDto->content);
            dto.birthState = AudioModificationBirthState::WaitingForSource;
            dto.originalF0InputStamp = sourceDto->originalF0InputStamp;
        }
    }

    audioModifications_.push_back(std::move(dto));
    return modification;
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
    region.playbackRegion = playbackRegion;
    playbackRegions_.push_back(std::move(region));
    return playbackRegions_.back();
}

OpenTuneDocumentController::PlaybackRegionProjection
OpenTuneDocumentController::makeProjection(const PlaybackRegion& placement) const
{
    PlaybackRegionProjection projection;

    auto* playbackRegion = placement.playbackRegion;
    if (playbackRegion == nullptr)
        return projection;

    projection.playbackRegion = playbackRegion;

    // 唯一读取点：DC 消息线程 projection/render plan 构建路径。
    // 音频线程/RenderWorker 只消费 projection 快照，不访问 SDK object。
    const auto* hostModification = playbackRegion->getAudioModification();
    const auto modificationPersistentId = hostModification != nullptr
        ? juce::String(hostModification->getPersistentID())
        : juce::String();
    projection.startInPlaybackTime = playbackRegion->getStartInPlaybackTime();
    projection.startInModificationTime = playbackRegion->getStartInAudioModificationTime();
    projection.durationInPlaybackTime = playbackRegion->getDurationInPlaybackTime();
    projection.durationInModificationTime = playbackRegion->getDurationInAudioModificationTime();

    // ARA 标准有效颜色：getEffectiveColor 回退链仅为 region 自身 color →
    // RegionSequence color（ARA_Library/PlugIn/ARAPlug.cpp:588-594），无 musical
    // context 一级。空值表示无颜色。ARAColor 为 0.0f~1.0f 的 RGB float，不透明
    // 投影为 alpha=1.0f 的 juce::Colour。
    if (const ARA::ARAColor* color = playbackRegion->getEffectiveColor())
        projection.displayColour = juce::Colour::fromFloatRGBA(color->r, color->g, color->b, 1.0f);
    else
        projection.displayColour = std::nullopt;

    const auto* modification = modificationPersistentId.isNotEmpty()
        ? findAudioModification(modificationPersistentId)
        : nullptr;
    if (modification == nullptr)
        return projection;

    projection.contentKey = modification->contentKey();
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

    const auto key = modification.contentKey();

    auto readSource = makePlaybackReadSource(
        key,
        modification.snapshotContent(),
        std::move(audioBuffer),
        TimeCoordinate::kRenderSampleRate,
        contentRenderService_->getOrCreateRenderCache(key),
        contentRenderService_->getTimeStretchCache());

    contentRenderService_->publishPlaybackSource(key, readSource);
    return true;
}

void OpenTuneDocumentController::readRestoredAudio(const AudioSource* enabledSource)
{
    // archive 恢复且已有有效 OriginalF0 的内容：自动重建 PCM，用户无需 Read。
    // WaitingForSource 由现有生命周期保证 content 存在，直接读 *mod.content。
    // 新插入插件无有效 F0，自动扫描自然跳过。
    for (auto& mod : audioModifications_)
    {
        if (mod.birthState != AudioModificationBirthState::WaitingForSource)
            continue;

        auto& content = *mod.content;
        if (content.analysis.originalF0State != OriginalF0State::Ready
            || content.analysis.pitchCurve == nullptr
            || !content.analysis.pitchCurve->hasOriginalF0Data())
            continue;

        if (enabledSource != nullptr
            && content.sourceWindow.sourcePersistentId != enabledSource->getIdentity().persistentId)
            continue;

        // 仅对有有效 PlaybackRegion 的 modification 触发读取（归属实时读 SDK object）
        bool hasPlaybackRegion = false;
        for (const auto& region : playbackRegions_)
        {
            if (!region.hasValidPlacement())
                continue;

            const auto* hostModification = region.playbackRegion->getAudioModification();
            if (hostModification != nullptr
                && juce::String(hostModification->getPersistentID()) == mod.persistentId)
            {
                hasPlaybackRegion = true;
                break;
            }
        }
        if (!hasPlaybackRegion)
            continue;

        // 复用 archive F0 前，建立当前运行时 stamp，使后续 Read 能命中 same-input no-op
        if (auto* source = findAudioSource(mod.content->sourceWindow.sourcePersistentId))
            mod.originalF0InputStamp = makeF0InputStamp(*source, mod);

        birthContentForModification(mod);
    }
}

OriginalF0InputStamp OpenTuneDocumentController::makeF0InputStamp(const AudioSource& source, const AudioModification& modification)
{
    OriginalF0InputStamp stamp;
    stamp.sourcePersistentId = source.getIdentity().persistentId;
    stamp.sourceSampleGeneration = source.getSampleContentGeneration();
    stamp.sourceSampleRate = source.getShape().sourceSampleRate;
    stamp.numSamples = source.getShape().numSamples;
    stamp.numChannels = source.getShape().numChannels;
    if (modification.hasContentState())
    {
        stamp.sourceStartSeconds = modification.content->sourceWindow.sourceStartSeconds;
        stamp.sourceEndSeconds = modification.content->sourceWindow.sourceEndSeconds;
    }
    return stamp;
}

bool OpenTuneDocumentController::birthContentForModification(AudioModification& modification)
{
    // F0 #B.4: 每次 birth 开始前取消该 content 的旧 F0 request
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

    // reader lease 由 didEnableAudioSourceSamplesAccess（ARA Audio Source Management
    // callback）建立，这里只消费，不在非实时路径创建。
    if (!source->canReadSamples())
    {
        modification.birthState = AudioModificationBirthState::WaitingForSource;
        return false;
    }

    modification.birthState = AudioModificationBirthState::Rendering;
    ++modification.birthRevision;

    // 若已有 valid F0（恢复路径），跳过重复提取，避免新旧重叠。
    // 验证 pitchCurve 实际包含 F0 数据，防止空 pitchCurve 误判为已提取。
    // 同时要求 stamp 与当前 source stamp 相等，force refresh 后不得走 archive reuse。
    const auto currentStamp = makeF0InputStamp(*source, modification);
    const bool stampMatches = modification.originalF0InputStamp.has_value()
        && modification.originalF0InputStamp.value() == currentStamp;
    const bool alreadyHasF0 = stampMatches
        && content.analysis.originalF0State == OriginalF0State::Ready
        && content.analysis.pitchCurve != nullptr
        && content.analysis.pitchCurve->hasOriginalF0Data();

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

    // 3. Extract ch0 for async F0 (skip if F0 already available)
    std::vector<float> channel0Data;
    if (!alreadyHasF0)
    {
        channel0Data.resize(static_cast<size_t>(playableAccum.getNumSamples()));
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

    // 6. Silent gaps 仅在新内容用户 Read 路径（无有效 F0）检测；
    // archive 恢复路径（alreadyHasF0）保留 restored silentGaps，不递增 analysis/contentRevision。
    if (!alreadyHasF0)
    {
        modification.submitSilentGaps(SilentGapDetector::detectAllGapsAdaptive(storedBuffer));
        modification.applyOriginalF0State(OriginalF0State::Extracting);
    }
    auto storedAudioBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(storedBuffer));

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
    // 有效 F0 表示 archive/runtime 恢复，不产生新模型变更；无有效 F0 表示用户 Read，保持通知
    if (modification.audioModification != nullptr)
        modification.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), !alreadyHasF0);

    // 有效 F0（archive 恢复）自动触发完整 Stage1 渲染，无需用户手动编辑即可出声；
    // 用户手动 Read（无有效 F0）不提前 Stage1，F0 提取完成后由既有链继续。
    if (alreadyHasF0)
        requestFullModificationRender(modification.contentKey());

    // 9. Schedule async F0 extraction via CRS (skip if F0 already available)
    if (!alreadyHasF0)
    {
        auto* hostModification = modification.audioModification;
        scheduleAsyncF0Extraction(modification.contentKey(), std::move(channel0Data), sourceSampleRate, hostModification, currentStamp);
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
// not user intent. 新内容仅由用户 Read（requestReadAudioForPlaybackRegion）读取；
// archive 恢复且已有有效 F0 的内容在 endEditing/access 后自动重建 PCM。

bool OpenTuneDocumentController::scheduleAsyncF0Extraction(
    ContentKey key,
    std::vector<float> channel0Data,
    double sourceSampleRate,
    juce::ARAAudioModification* hostModification,
    OriginalF0InputStamp stamp)
{
    // Pre-check failures: mark matching content Failed so Extracting is not permanent
    auto markFailedIfCurrentBirth = [&](uint64_t birth) {
        if (auto* m = findAudioModificationByContentKey(key))
        {
            if (m->birthRevision == birth
                && (hostModification == nullptr || m->audioModification == hostModification))
            {
                m->applyOriginalF0State(OriginalF0State::Failed);
                if (m->audioModification != nullptr)
                    m->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
            }
        }
    };

    // Capture current birthRevision before any early return
    uint64_t birthRevision = 0;
    if (auto* mod = findAudioModificationByContentKey(key))
        birthRevision = mod->birthRevision;

    if (!contentF0ExtractionService_)
    {
        markFailedIfCurrentBirth(birthRevision);
        return false;
    }

    if (hostModification == nullptr)
    {
        markFailedIfCurrentBirth(birthRevision);
        return false;
    }

    auto crs = contentRenderService_;
    if (!crs)
    {
        markFailedIfCurrentBirth(birthRevision);
        return false;
    }

    auto submitResult = contentF0ExtractionService_->submit(
        F0RequestKey{key},
        [data = std::move(channel0Data), sourceSampleRate, birthRevision, leaseToken = asyncLeaseToken_](const std::shared_ptr<F0RunOwnerState>& runOwnerState) mutable
        {
            if (leaseToken && !leaseToken->load(std::memory_order_acquire))
                return F0ExtractionService::Result{};

            if (data.empty())
                return F0ExtractionService::Result{};

            // Lazy-initialize ProcessF0Runtime and resolve F0 service inside the worker
            // thread (same pattern as PluginProcessor capture/requestContentRefresh).
            auto& f0Runtime = ProcessF0Runtime::getInstance();
            if (!f0Runtime.isReady())
                f0Runtime.initialize(ModelPathResolver::getModelsDirectory());
            auto f0Svc = f0Runtime.getF0Service();
            if (!f0Svc)
            {
                AppLogger::error("ARA-F0: worker lazy-init model Failed — F0 service unavailable (birthRev="
                    + juce::String(static_cast<juce::int64>(birthRevision)) + ")");
                F0ExtractionService::Result failed;
                failed.errorMessage = "ara_no_f0_service";
                return failed;
            }

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
        [this, crs, key, birthRevision, hostModification, stamp, leaseToken = asyncLeaseToken_](F0ExtractionService::Result&& result) mutable
        {
            if (leaseToken && !leaseToken->load(std::memory_order_acquire))
                return;

            // Non-destruction CRS unavailability: set Failed to avoid latch stall
            if (crs == nullptr)
            {
                if (auto* mod = findAudioModificationByContentKey(key))
                {
                    if (mod->birthRevision == birthRevision
                        && (hostModification == nullptr || mod->audioModification == hostModification)
                        && mod->audioModification != nullptr)
                    {
                        mod->applyOriginalF0State(OriginalF0State::Failed);
                        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
                    }
                }
                return;
            }

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
                    // Stamp 必须与提交时捕获的 stamp 一致；从当前 source 重新构造 stamp 比对，
                    // 防止 source shape/generation 变化未走 affectSamples invalidation
                    if (!mod->originalF0InputStamp.has_value() || mod->originalF0InputStamp.value() != stamp)
                        return; // Stale completion - source changed during extraction
                    auto* currentSource = this->findAudioSource(mod->content->sourceWindow.sourcePersistentId);
                    if (currentSource == nullptr)
                        return; // Stale completion - source no longer available
                    const auto nowStamp = makeF0InputStamp(*currentSource, *mod);
                    if (nowStamp != stamp)
                        return; // Stale completion - source shape/generation changed during extraction

                    mod->applyOriginalF0State(OriginalF0State::Failed);
                    if (mod->audioModification != nullptr)
                        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

                    // Log the failure reason (result.errorMessage carries the worker-side failure cause)
                    AppLogger::error("ARA-F0: extraction failed key="
                        + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
                        + " mod=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(mod))
                        + " reason=" + (result.errorMessage.empty() ? juce::String("no_data_or_unvoiced") : juce::String(result.errorMessage)));
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
                // Stamp 必须与提交时捕获的 stamp 一致；从当前 source 重新构造 stamp 比对，
                // 防止 source shape/generation 变化未走 affectSamples invalidation
                if (!mod->originalF0InputStamp.has_value() || mod->originalF0InputStamp.value() != stamp)
                    return; // Stale completion - source changed during extraction
                auto* currentSource = this->findAudioSource(mod->content->sourceWindow.sourcePersistentId);
                if (currentSource == nullptr)
                    return; // Stale completion - source no longer available
                const auto nowStamp = makeF0InputStamp(*currentSource, *mod);
                if (nowStamp != stamp)
                    return; // Stale completion - source shape/generation changed during extraction

                // Rebuild pitchCurve from Result
                auto pitchCurve = std::make_shared<PitchCurve>();
                pitchCurve->setOriginalF0(result.f0);
                pitchCurve->setSampleRate(static_cast<double>(result.f0SampleRate));
                pitchCurve->setHopSize(result.hopSize);
                if (!result.energy.empty())
                    pitchCurve->setOriginalEnergy(result.energy);

                // 调式检测（F0 提交成功链）：origin==Manual 的内容永不覆盖；
                // 检测在 std::move 前读 result 数据，pitchCurve 移动后由 applyOriginalF0 存入 content
                if (mod->content->analysis.detectedKey.origin != Origin::Manual) {
                    F0KeyDetector detector;
                    const auto detectedKey = detector.detect(result.f0, result.energy);
                    if (detectedKey.origin != Origin::Unset)
                        mod->applyDetectedKey(detectedKey);
                }

                mod->applyOriginalF0(std::move(pitchCurve));
                // 保存 stamp，确保后续 Read 可命中 same-input no-op
                mod->originalF0InputStamp = stamp;

                if (mod->audioModification != nullptr)
                    mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

                // F0 commit → form "committed data → request current version render" transaction
                requestFullModificationRender(key);
            }
        });

    // Non-Accepted: set Failed if current modification still matches this submission
    if (submitResult != F0ExtractionService::SubmitResult::Accepted)
    {
        if (auto* mod = findAudioModificationByContentKey(key))
        {
            if (mod->birthRevision == birthRevision
                && (hostModification == nullptr || mod->audioModification == hostModification))
            {
                mod->applyOriginalF0State(OriginalF0State::Failed);
                if (mod->audioModification != nullptr)
                    mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
            }
        }
        return false;
    }

    return true;
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
    // Stage2: TimeGrid 时间拉伸重建（只消费 job 携带的 snapshot/audio）
    if (job.kind == RenderJob::Kind::Stage2Rebuild)
    {
        Stage2TimeStretchRebuilder::Request request;
        request.contentKey = job.contentKey;
        request.contentSnapshot = job.contentSnapshot;
        request.audioBuffer = job.audioBuffer;
        request.audioSampleRate = job.audioSampleRate;
        Stage2TimeStretchRebuilder::rebuild(*contentRenderService_, request);
        return;
    }

    if (job.renderCache == nullptr)
        return;

    if (!job.contentSnapshot)
    {
        job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision);
        return;
    }

    ProcessRenderRuntime::CompletionContext completion;
    completion.gate = completionGate_;
    completion.chunkSettled = [this](ContentKey key,
                                     std::shared_ptr<const EditableContentSnapshot> snapshot,
                                     std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                                     double audioSampleRate) {
        handleStage1ChunkSettled(key, std::move(snapshot),
                                 std::move(audioBuffer), audioSampleRate);
    };
    ProcessRenderRuntime::getInstance().processChunkRenderJob(
        contentRenderService_, job,
        false, std::move(completion));
}

void OpenTuneDocumentController::handleStage1ChunkSettled(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> snapshot,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
    double audioSampleRate)
{
    if (contentRenderService_ == nullptr)
        return;

    if (snapshot == nullptr
        || audioBuffer == nullptr
        || snapshot->timeGrid->isIdentity())
        return;

    contentRenderService_->enqueueStage2RebuildWhenCanonicalSettled(
        key,
        std::move(snapshot),
        std::move(audioBuffer),
        audioSampleRate);
}

std::shared_ptr<const EditableContentSnapshot> OpenTuneDocumentController::snapshotAudioModification(ContentKey key) const
{
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState())
        return nullptr;

    // AudioModification adds the cached AudioSource shape to the common projection.
    return mod->snapshotContent();
}

void OpenTuneDocumentController::requestModificationRender(
    ContentKey key,
    double startSeconds,
    double endSeconds,
    std::shared_ptr<const EditableContentSnapshot> snapshot)
{
    PlaybackReadSource readSource;
    if (contentRenderService_ == nullptr
        || !contentRenderService_->getPlaybackReadSource(key, readSource)
        || readSource.audioBuffer == nullptr
        || readSource.audioSampleRate <= 0.0)
        return;

    const int64_t totalSamples = readSource.audioBuffer->getNumSamples();
    const int64_t startSample = juce::jlimit<int64_t>(
        0, totalSamples, TimeCoordinate::secondsToSamplesFloor(startSeconds, readSource.audioSampleRate));
    const int64_t endSample = juce::jlimit<int64_t>(
        0, totalSamples, TimeCoordinate::secondsToSamplesCeil(endSeconds, readSource.audioSampleRate));
    requestModificationRenderSamples(key, startSample, endSample, std::move(snapshot));
}

void OpenTuneDocumentController::requestModificationRenderSamples(
    ContentKey key,
    int64_t startSample,
    int64_t endSampleExclusive,
    std::shared_ptr<const EditableContentSnapshot> snapshot)
{
    if (contentRenderService_ == nullptr || endSampleExclusive <= startSample)
        return;

    PlaybackReadSource readSource;
    if (!contentRenderService_->getPlaybackReadSource(key, readSource)
        || readSource.audioBuffer == nullptr
        || readSource.audioSampleRate <= 0.0)
        return;

    const int64_t totalSamples = readSource.audioBuffer->getNumSamples();
    startSample = juce::jlimit<int64_t>(0, totalSamples, startSample);
    endSampleExclusive = juce::jlimit<int64_t>(0, totalSamples, endSampleExclusive);
    if (endSampleExclusive <= startSample)
        return;

    if (!snapshot || !contentRenderService_->republishPlaybackSource(key, snapshot)
        || !snapshot->hasUsableOriginalF0())
        return;

    RenderJob job;
    job.contentKey = key;
    job.audioBuffer = readSource.audioBuffer;
    job.audioSampleRate = readSource.audioSampleRate;
    job.startSample = startSample;
    job.endSampleExclusive = endSampleExclusive;
    job.renderCache = contentRenderService_->getOrCreateRenderCache(key);
    job.contentSnapshot = std::move(snapshot);

    contentRenderService_->enqueueRender(std::move(job));
}

void OpenTuneDocumentController::requestFullModificationRender(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> snapshot)
{
    if (contentRenderService_ == nullptr)
        return;

    if (!snapshot)
        snapshot = snapshotAudioModification(key);
    if (!snapshot)
        return;

    PlaybackReadSource readSource;
    if (!contentRenderService_->getPlaybackReadSource(key, readSource))
        return;

    if (readSource.audioBuffer == nullptr || readSource.audioSampleRate <= 0.0)
        return;

    requestModificationRenderSamples(
        key, 0, readSource.audioBuffer->getNumSamples(), std::move(snapshot));
}

void OpenTuneDocumentController::invalidateAllModificationCaches()
{
    if (contentRenderService_ == nullptr)
        return;

    for (const auto& mod : audioModifications_)
    {
        if (!mod.isRenderable())
            continue;
        const auto key = mod.contentKey();
        if (!key.isValid())
            continue;
        if (auto cache = contentRenderService_->getRenderCache(key))
            cache->clear();
    }
    contentRenderService_->getTimeStretchCache().clear();

    for (const auto& mod : audioModifications_)
    {
        if (!mod.isRenderable())
            continue;
        requestFullModificationRender(mod.contentKey());
    }
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

void OpenTuneDocumentController::observeHostPlaybackState(bool isPlaying) noexcept
{
    auto state = playbackCommandState_.load(std::memory_order_acquire);
    auto next = state + kStateVersionIncrement;

    // Publish this observation and, when it matches the request that was
    // present in the same snapshot, confirm that request. A failed CAS means a
    // newer request or observation won the race; do not spin on the audio thread
    // or reinterpret this old PositionInfo against the newer state.
    next |= kObservedValid;
    if (isPlaying)
        next |= kObservedPlaying;
    else
        next &= ~kObservedPlaying;

    const bool pending = (state & kRequestPending) != 0;
    const bool targetPlaying = (state & kTargetPlaying) != 0;
    if (pending && targetPlaying == isPlaying)
        next &= ~kRequestPending;

    if ((next & kRequestPending) == 0)
    {
        if (isPlaying)
            next |= kTargetPlaying;
        else
            next &= ~kTargetPlaying;
    }

    playbackCommandState_.compare_exchange_strong(state,
                                                  next,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed);
}

void OpenTuneDocumentController::observeHostPlaybackPosition(
    const juce::Optional<juce::AudioPlayHead::PositionInfo>& positionInfo,
    double blockDurationSeconds) noexcept
{
    // Update the CAS-based play/stop observation first (preserves existing logic).
    if (positionInfo.hasValue())
        observeHostPlaybackState(positionInfo->getIsPlaying());

    // Update document-shared canonical transport truth from the playback renderer.
    const uint64_t epoch = sharedPlayHeadState_.update(positionInfo);

    // Publish projection anchor with CAS: multi-writer safe (multiple ARA roles
    // may call this from their processBlock). On contention, skip — canonical
    // atomics already hold the latest truth, so UI falls back to timeInSeconds.
    // Only publish when blockDurationSeconds > 0 (real audio data); zero-sample
    // blocks still update canonical state but must not publish a zero-horizon projection.
    if (blockDurationSeconds > 0.0 && positionInfo.hasValue())
    {
        if (const auto timeSec = positionInfo->getTimeInSeconds())
        {
            const double nowClock = juce::Time::getMillisecondCounterHiRes() * 0.001;
            const double horizon = *timeSec + blockDurationSeconds;
            if (sharedPlayHeadState_.timeInSeconds.load(std::memory_order_relaxed) == *timeSec)
                sharedPlayHeadState_.presentationProjection.tryPublish(
                    *timeSec, nowClock, horizon, epoch);
        }
    }
}

void OpenTuneDocumentController::markPlaybackRequest(bool shouldPlay) noexcept
{
    auto state = playbackCommandState_.load(std::memory_order_acquire);
    for (;;)
    {
        auto next = state + kStateVersionIncrement;
        next |= kRequestPending;
        if (shouldPlay)
            next |= kTargetPlaying;
        else
            next &= ~kTargetPlaying;

        if (playbackCommandState_.compare_exchange_weak(state,
                                                        next,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire))
            return;
    }
}

bool OpenTuneDocumentController::requestTogglePlayback(bool fallbackObservedPlaying, double pendingSeekTime)
{
    // Message-thread only. Check host availability first.
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    auto state = playbackCommandState_.load(std::memory_order_acquire);
    bool newTarget = false;
    for (;;)
    {
        const bool pending = (state & kRequestPending) != 0;
        const bool targetPlaying = (state & kTargetPlaying) != 0;
        const bool observedValid = (state & kObservedValid) != 0;
        const bool observedPlaying = (state & kObservedPlaying) != 0;

        // Decision priority: pending target (flip) > valid shared observed (flip)
        // > caller's local processor snapshot as first-contact fallback.
        if (pending)
            newTarget = !targetPlaying;
        else if (observedValid)
            newTarget = !observedPlaying;
        else
            newTarget = !fallbackObservedPlaying;

        auto next = state + kStateVersionIncrement;
        next |= kRequestPending;
        if (newTarget)
            next |= kTargetPlaying;
        else
            next &= ~kTargetPlaying;

        if (playbackCommandState_.compare_exchange_weak(state,
                                                        next,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire))
            break;
    }

    if (newTarget)
    {
        if (pendingSeekTime >= 0.0)
            playbackController->requestSetPlaybackPosition(pendingSeekTime);
        playbackController->requestStartPlayback();
        AppLogger::log("ARA-TRANSPORT: toggle target=start observed="
            + juce::String((state & kObservedValid)
                ? ((state & kObservedPlaying) ? "true" : "false")
                : "unknown")
            + " fallback=" + juce::String(fallbackObservedPlaying ? "true" : "false"));
    }
    else
    {
        playbackController->requestStopPlayback();
        AppLogger::log("ARA-TRANSPORT: toggle target=stop observed="
            + juce::String((state & kObservedValid)
                ? ((state & kObservedPlaying) ? "true" : "false")
                : "unknown")
            + " fallback=" + juce::String(fallbackObservedPlaying ? "true" : "false"));
    }
    return true;
}

bool OpenTuneDocumentController::requestStartPlayback(double pendingSeekTime)
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    if (pendingSeekTime >= 0.0)
        playbackController->requestSetPlaybackPosition(pendingSeekTime);

    markPlaybackRequest(true);
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

    markPlaybackRequest(false);
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

std::shared_ptr<const PitchCurveSnapshot> OpenTuneDocumentController::readPitchCurve(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return nullptr;
    return mod->content->analysis.pitchCurve != nullptr
        ? mod->content->analysis.pitchCurve->getSnapshot()
        : nullptr;
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

PitchShiftSettings OpenTuneDocumentController::readPitchShift(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return {};
    return mod->content->pitchShiftSettings;
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

    contentRenderService_->republishPlaybackSource(key, mod->snapshotContent());
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
    const bool isIdentity = grid->isIdentity();
    if (!mod->applyTimeGrid(std::move(grid)))
        return false;

    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

    auto snapshot = mod->snapshotContent();

    // TimeGrid 变更 → 失效 Stage2 并在 Stage1 settled 后重建；
    // identity TimeGrid 无需 Stage2（无时间拉伸）。
    if (isIdentity)
    {
        contentRenderService_->republishPlaybackSource(key, snapshot);
    }
    else
    {
        contentRenderService_->getTimeStretchCache().invalidate(key);
        requestFullModificationRender(key, std::move(snapshot));
    }

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

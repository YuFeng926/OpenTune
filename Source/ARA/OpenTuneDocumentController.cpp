#include "OpenTuneDocumentController.h"

#include "OpenTunePlaybackRenderer.h"

#include "../Audio/NotePitchAudition.h"
#include "../Utils/NoteAudition.h"

#include "../Runtime/ProcessF0Runtime.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../DSP/F0KeyDetector.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"
#include "../Utils/PlaybackSampleGeometry.h"
#include "../Utils/SilentGapDetector.h"
#include "../Utils/PitchCurve.h"
#include "../Inference/RenderCache.h"
#include "../Utils/SourceWindow.h"
#include "../Utils/AppLogger.h"
#include "../Content/ContentEditCommands.h"
#include "../Content/ContentSnapshotProjection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace OpenTune {

namespace {

constexpr int kContentPayloadArchiveMagic = 0x4F544143;
constexpr int kContentPayloadArchiveVersionMin = 3;
constexpr int kMaxContentPayloadRecords = 4096;

juce::String araDiagnostic(const OpenTuneDocumentController* controller)
{
    return "ARA-DIAG: dc=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(controller)) + " ";
}

juce::String f0Diagnostic(const OpenTuneDocumentController* controller,
                         const juce::String& modificationId, uint64_t birthRevision)
{
    return araDiagnostic(controller) + "modification=" + modificationId.quoted()
        + " birth=" + juce::String(static_cast<juce::int64>(birthRevision)) + " ";
}

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
        copy.analysis.clearOriginalAnalysis();

    return copy;
}

} // namespace

struct OpenTuneDocumentController::HostTransportRelayState
{
    explicit HostTransportRelayState(OpenTuneDocumentController& document) : owner(document) {}

    OpenTuneDocumentController& owner;
    std::atomic<bool> alive { true };
    std::mutex mutex;
    HostTransportCoalescer coalescer;
    bool flushScheduled = false;
#if JUCE_MAC
    CFRunLoopTimerRef pendingFlush = nullptr;
#endif
};

namespace {

#if JUCE_MAC
void transportFlushTimerFired(CFRunLoopTimerRef, void* info)
{
    auto flush = std::move(*static_cast<std::function<void()>*>(info));
    flush();
}

void deferTransportFlush(CFRunLoopTimerRef& pendingFlush, std::function<void()> fn)
{
    // Mouse tracking runs in kCFRunLoopCommonModes. Logic drops playback-controller
    // calls made from that mode, so the flush waits for the default mode.
    auto* holder = new std::function<void()>(std::move(fn));
    CFRunLoopTimerContext context {};
    context.info = holder;
    context.release = [](const void* info) { delete static_cast<const std::function<void()>*>(info); };
    pendingFlush = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        0,
        0,
        0,
        transportFlushTimerFired,
        &context);
    if (pendingFlush == nullptr)
    {
        std::unique_ptr<std::function<void()>> owned(holder);
        (*owned)();
        return;
    }

    CFRunLoopAddTimer(CFRunLoopGetMain(), pendingFlush, kCFRunLoopDefaultMode);
}
#endif

template <typename Member>
bool playbackMemberCallable(const ARA::ARAPlaybackControllerInterface* iface,
                            bool fieldPresent,
                            Member member) noexcept
{
    if (iface == nullptr || !fieldPresent)
        return false;
    return iface->*member != nullptr;
}

bool editorRendererOwnsRegion(juce::ARAEditorRenderer* renderer,
                              juce::ARAPlaybackRegion* region) noexcept
{
    if (renderer == nullptr || region == nullptr)
        return false;

    const auto& direct = renderer->getPlaybackRegions<juce::ARAPlaybackRegion>();
    if (std::find(direct.begin(), direct.end(), region) != direct.end())
        return true;

    for (auto* sequence : renderer->getRegionSequences<juce::ARARegionSequence>())
    {
        if (sequence == nullptr)
            continue;
        const auto& inSequence = sequence->getPlaybackRegions<juce::ARAPlaybackRegion>();
        if (std::find(inSequence.begin(), inSequence.end(), region) != inSequence.end())
            return true;
    }
    return false;
}

} // namespace

OpenTuneDocumentController::OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                                        const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation(entry, instance)
    , appPreferences_({}, [this](auto scheme) { audioEditingSchemeChanged(scheme); })
    , contentRenderService_(std::make_shared<ContentRenderService>())
    , resamplingManager_(std::make_shared<ResamplingManager>())
    , transportRelay_(std::make_shared<HostTransportRelayState>(*this))
    , notePitchAudition_(std::make_unique<NotePitchAudition>())
    , renderTerminalRelay_(std::make_shared<RenderTerminalRelay>())
{
    auditionRoute_.notePitchAudition.store(notePitchAudition_.get(), std::memory_order_release);
    ProcessRenderRuntime::getInstance().retainOwner();
    installDocumentRenderExecution();
    f0Flights_.sink = std::make_shared<AraF0DocumentFlights::CompletionSink>(
        [this](const std::vector<F0TaskId>& ids, const AraF0Result& result) {
            AraF0DocumentFlights::Receivers receivers;
            for (auto id : ids)
                receivers.merge(f0Flights_.take(id));
            for (const auto& receiver : receivers)
                commitF0Completion(receiver.second, result);
        });
    AppLogger::logNoThrow(araDiagnostic(this) + "DocumentController created");
}

OpenTuneDocumentController::~OpenTuneDocumentController()
{
    // DC/ARA model ownership、F0 completion 和 DC destruction 均在消息线程串行执行。
    auto* messageManager = juce::MessageManager::getInstance();
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
    {
        AppLogger::error("ARA-DIAG: DocumentController destroyed off message thread; ownership contract violated");
        std::terminate();
    }
    AppLogger::logNoThrow(araDiagnostic(this) + "DocumentController destructor begin");
    revokeDocumentF0();

    transportRelay_->alive.store(false, std::memory_order_release);
#if JUCE_MAC
    if (auto timer = std::exchange(transportRelay_->pendingFlush, nullptr))
    {
        CFRunLoopTimerInvalidate(timer);
        CFRelease(timer);
    }
#endif

    if (renderTerminalRelay_ != nullptr)
        renderTerminalRelay_->clear();
    auditionRoute_.keyboardAudition.store(nullptr, std::memory_order_release);
    auditionRoute_.keyboardMixer.store(nullptr, std::memory_order_release);
    auditionRoute_.notePitchMixer.store(nullptr, std::memory_order_release);

    renderCompletionGate_->closeAndWait();

    // 停止渲染服务：detach execution lease（清 lease、失败收敛排队 job，
    // 等待同步回调和异步 completion 全部归还）。
    if (contentRenderService_)
        contentRenderService_->detachExecutionLease(contentRenderService_.get());
    ProcessRenderRuntime::getInstance().detachDeferredJobs(
        contentRenderService_.get(), renderCompletionGate_);

    // Owner-driven detach: before clearing playbackRenderers_, walk the list
    // and call detachDocumentController(*this) on each renderer.
    for (auto* renderer : playbackRenderers_)
    {
        if (renderer != nullptr)
            renderer->detachDocumentController(*this);
    }
    collectPlaybackSnapshots();
    playbackRenderers_.clear();

    if (contentRenderService_)
    {
        contentRenderService_->clearAll();
        collectPlaybackSnapshots();
    }

    ProcessRenderRuntime::getInstance().releaseOwner();
    AppLogger::logNoThrow(araDiagnostic(this) + "DocumentController destructor end");
}

void OpenTuneDocumentController::willDestroyDocument(juce::ARADocument*)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    revokeDocumentF0();
    AppLogger::logNoThrow(araDiagnostic(this) + "willDestroyDocument");
}

namespace {
juce::String archiveSeconds(double seconds)
{
    return juce::String(seconds, std::numeric_limits<double>::max_digits10 - 1, true);
}

juce::String encodeArchiveFloats(const std::vector<float>& values)
{
    return juce::MemoryBlock(values.data(), values.size() * sizeof(float)).toBase64Encoding();
}

bool decodeArchiveFloats(const juce::String& encoded, std::vector<float>& values)
{
    const int dot = encoded.indexOfChar('.');
    if (dot <= 0)
        return false;

    const auto length = encoded.substring(0, dot);
    const auto bytes = length.getLargeIntValue();
    const auto data = encoded.substring(dot + 1);
    // JUCE's decoder trusts the byte count and zero-fills a short payload.
    if (bytes < 0 || bytes > std::numeric_limits<int>::max() / 8
        || bytes % sizeof(float) != 0 || length != juce::String(bytes)
        || data.length() != (bytes * 8 + 5) / 6
        || !data.containsOnly(".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"))
        return false;

    juce::MemoryBlock decoded;
    if (!decoded.fromBase64Encoding(encoded))
        return false;
    if (bytes != 0)
    {
        values.resize(static_cast<size_t>(bytes) / sizeof(float));
        std::memcpy(values.data(), decoded.getData(), decoded.getSize());
    }
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

void serializeAudioModificationContent(const AudioModification& mod, juce::XmlElement& el)
{
    OpenTuneDocumentController::writeSourceWindowToArchive(mod.content->sourceWindow, el);

    auto* editable = new juce::XmlElement("EditableContent");
    editable->setAttribute("notesRevision", juce::String(static_cast<juce::int64>(mod.content->notesRevision)));
    editable->setAttribute("pitchRevision", juce::String(static_cast<juce::int64>(mod.content->pitchRevision)));
    editable->setAttribute("timeGridRevision", juce::String(static_cast<juce::int64>(mod.content->timeGridRevision)));
    editable->setAttribute("noteTopologyInitialized", mod.content->noteTopologyInitialized ? 1 : 0);

    for (const auto& note : mod.content->notes)
    {
        auto* n = new juce::XmlElement("Note");
        n->setAttribute("start", archiveSeconds(note.startTime));
        n->setAttribute("end", archiveSeconds(note.endTime));
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
        p->setAttribute("time", archiveSeconds(point.timeSeconds));
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
            s->setAttribute("f0Base64", encodeArchiveFloats(seg.f0Data));
            s->setAttribute("baseF0Base64", encodeArchiveFloats(seg.baseF0Data));
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
            he->setAttribute("sourceSeconds", archiveSeconds(h.source_seconds));
            he->setAttribute("outputSeconds", archiveSeconds(h.output_seconds));
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
    const auto originalF0State = mod.content->analysis.originalF0State;
    const bool pending = mod.analysisContinuationPending || originalF0State == OriginalF0State::Extracting;
    analysis->setAttribute("originalF0State", static_cast<int>(pending
        ? OriginalF0State::NotRequested : originalF0State));
    analysis->setAttribute("analysisContinuationPending", pending ? 1 : 0);
    if (const auto& stamp = mod.originalF0InputStamp;
        originalF0State == OriginalF0State::Ready && !pending && stamp)
    {
        analysis->setAttribute("sourcePersistentId", stamp->sourcePersistentId);
        analysis->setAttribute("sourceSampleGeneration", juce::String(static_cast<juce::int64>(stamp->sourceSampleGeneration)));
        analysis->setAttribute("sourceSampleRate", archiveSeconds(stamp->sourceSampleRate));
        analysis->setAttribute("numSamples", juce::String(static_cast<juce::int64>(stamp->numSamples)));
        analysis->setAttribute("numChannels", stamp->numChannels);
    }
    
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
        rf->setAttribute("sourceDurationSeconds", archiveSeconds(mod.content->analysis.referenceFeatures.sourceDurationSeconds));
        if (mod.content->analysis.referenceFeatures.errorMessage.isNotEmpty())
            rf->setAttribute("errorMessage", mod.content->analysis.referenceFeatures.errorMessage);
        
        // Pitch notes
        for (const auto& note : mod.content->analysis.referenceFeatures.pitch.notes)
        {
            auto* n = new juce::XmlElement("PitchNote");
            n->setAttribute("start", archiveSeconds(note.startTime));
            n->setAttribute("end", archiveSeconds(note.endTime));
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
            a->setAttribute("sourceSeconds", archiveSeconds(anchor.sourceSeconds));
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
            pc->setAttribute("f0Base64", encodeArchiveFloats(snap->getOriginalF0()));
            const auto& energy = snap->getOriginalEnergy();
            if (!energy.empty())
                pc->setAttribute("energyBase64", encodeArchiveFloats(energy));
            for (const auto& range : snap->getErasedRanges())
            {
                auto* erased = new juce::XmlElement("ErasedRange");
                erased->setAttribute("startFrame", range.startFrame);
                erased->setAttribute("endFrameExclusive", range.endFrameExclusive);
                pc->addChildElement(erased);
            }
            analysis->addChildElement(pc);
        }
    }
    el.addChildElement(analysis);
}

struct RestoredModificationContent
{
    ContentState content;
    bool analysisContinuationPending{false};
    std::optional<OriginalF0InputStamp> originalF0InputStamp;
};

std::optional<RestoredModificationContent> restoreAudioModificationContent(
    const juce::XmlElement& el, SourceWindow sourceWindow, int archiveVersion)
{
    RestoredModificationContent restored;
    auto& analysisContinuationPending = restored.analysisContinuationPending;
    auto& content = restored.content;
    content.sourceWindow = std::move(sourceWindow);
    std::vector<PitchCorrectionSegment> restoredCorrectionSegments;

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

            const auto frameCount = static_cast<size_t>(seg.endFrame - seg.startFrame);
            if (!decodeArchiveFloats(s->getStringAttribute("f0Base64"), seg.f0Data)
                || seg.f0Data.size() != frameCount)
                return std::nullopt;
            if (archiveVersion >= OpenTuneDocumentController::kContentArchiveVersionF0Continuation
                && !decodeArchiveFloats(s->getStringAttribute("baseF0Base64"), seg.baseF0Data))
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

    // TimeGrid: 必须存在且通过 TimeGridSnapshot 工厂验证
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

        // Non-identity archive TimeGrid is rejected rather than silently replaced.
        if (!timeGrid->isIdentity())
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
        if (archiveVersion < OpenTuneDocumentController::kContentArchiveVersionF0Continuation)
        {
            analysisContinuationPending = content.analysis.originalF0State == OriginalF0State::Extracting;
            if (analysisContinuationPending)
                content.analysis.originalF0State = OriginalF0State::NotRequested;
        }
        else
        {
            const auto continuation = analysis->getStringAttribute("analysisContinuationPending");
            if ((continuation != "0" && continuation != "1")
                || content.analysis.originalF0State == OriginalF0State::Extracting)
                return std::nullopt;
            analysisContinuationPending = continuation == "1";
            if (analysisContinuationPending && content.analysis.originalF0State != OriginalF0State::NotRequested)
                return std::nullopt;
        }

        // The stamp identifies the source state the archived analysis belongs to.
        // Archives without the stamp attributes (older format) restore without
        // one and count as mismatch in the restore apply loop.
        if (archiveVersion >= OpenTuneDocumentController::kContentArchiveVersionF0Continuation
            && content.analysis.originalF0State == OriginalF0State::Ready
            && !analysisContinuationPending
            && analysis->hasAttribute("sourcePersistentId"))
        {
            OriginalF0InputStamp stamp;
            stamp.sourcePersistentId = analysis->getStringAttribute("sourcePersistentId");
            stamp.sourceSampleGeneration = static_cast<uint64_t>(
                analysis->getStringAttribute("sourceSampleGeneration").getLargeIntValue());
            stamp.sourceSampleRate = analysis->getDoubleAttribute("sourceSampleRate");
            stamp.numSamples = analysis->getStringAttribute("numSamples").getLargeIntValue();
            stamp.numChannels = analysis->getIntAttribute("numChannels");
            // The archived window endpoints are stored separately and belong to
            // the same content; the live stamp projects them identically.
            stamp.sourceStartSeconds = content.sourceWindow.sourceStartSeconds;
            stamp.sourceEndSeconds = content.sourceWindow.sourceEndSeconds;
            restored.originalF0InputStamp = std::move(stamp);
        }

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
                rfProducer != static_cast<int>(ReferenceFeatureProducer::StandardAuto) &&
                rfProducer != 2 /* legacy GAME producer: tolerated on read */)
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
            std::vector<float> energy;
            const auto encodedF0 = pc->getStringAttribute("f0Base64");
            const auto encodedEnergy = pc->getStringAttribute("energyBase64");
            if ((encodedF0.isNotEmpty() && !decodeArchiveFloats(encodedF0, f0))
                || (encodedEnergy.isNotEmpty() && !decodeArchiveFloats(encodedEnergy, energy)))
                return std::nullopt;

            // In v10 a correction layer can outlive its raw analysis. Frame
            // addresses remain in the stored curve coordinates while it waits.
            const int frameCount = static_cast<int>(f0.size());
            const bool legacy = archiveVersion < OpenTuneDocumentController::kContentArchiveVersionF0Continuation;
            int erasedEnd = 0;
            std::vector<F0FrameRange> erasedRanges;
            for (auto* erased : pc->getChildWithTagNameIterator("ErasedRange"))
            {
                const int startFrame = erased->getIntAttribute("startFrame");
                const int endFrameExclusive = erased->getIntAttribute("endFrameExclusive");
                if (startFrame < 0 || endFrameExclusive <= startFrame
                    || (legacy && endFrameExclusive > frameCount))
                    return std::nullopt;
                erasedRanges.push_back({startFrame, endFrameExclusive});
                erasedEnd = std::max(erasedEnd, endFrameExclusive);
            }

            auto fresh = std::make_shared<PitchCurve>();
            fresh->replaceOriginalAnalysis(
                std::move(f0), std::move(energy), hopSize, sampleRate);
            if (!erasedRanges.empty())
                fresh->replaceErasedRangesInRange(0, erasedEnd, erasedRanges);
            content.analysis.pitchCurve = std::move(fresh);
        }
    }

    if (!restoredCorrectionSegments.empty())
    {
        if (content.analysis.pitchCurve == nullptr)
            return std::nullopt;

        if (archiveVersion < OpenTuneDocumentController::kContentArchiveVersionF0Continuation)
        {
            const int frameCount = static_cast<int>(content.analysis.pitchCurve->size());
            for (const auto& segment : restoredCorrectionSegments)
                if (segment.endFrame > frameCount)
                    return std::nullopt;
        }

        // ARA 严格校验已覆盖 startFrame >= 0、区间有序与 f0Data 长度（违规整
        // 文档拒绝），baseF0Data 对齐未校验：与其他加载边界一致做一次归一，
        // 错位 base 按前缀截断或置空。
        normalizeLoadedCorrectionSegmentLengths(restoredCorrectionSegments);
        auto next = content.analysis.pitchCurve->deriveCopy();
        next->replaceCorrectionSegments(restoredCorrectionSegments);
        content.analysis.pitchCurve = std::move(next);
    }

    return restored;
}
} // namespace

void OpenTuneDocumentController::writeSourceWindowToArchive(const SourceWindow& window,
                                                            juce::XmlElement& element)
{
    auto* sourceWindow = new juce::XmlElement("SourceWindow");
    sourceWindow->setAttribute("sourcePersistentId", window.sourcePersistentId);
    // Note: numeric sourceId is NOT serialized in ARA domain (it's for Capture/Standalone only)
    // ARA domain uses AudioSource persistentID only
    // Both authoritative endpoints retain all binary64 significant digits.
    sourceWindow->setAttribute("startSeconds", archiveSeconds(window.sourceStartSeconds));
    sourceWindow->setAttribute("endSeconds", archiveSeconds(window.sourceEndSeconds));
    element.addChildElement(sourceWindow);
}

std::optional<SourceWindow> OpenTuneDocumentController::readSourceWindowFromArchive(
    const juce::XmlElement& element,
    const juce::ARAAudioSource& parentSource,
    const ARA::ARARestoreObjectsFilter* hostFilter,
    int archiveVersion)
{
    const auto* sourceWindow = element.getChildByName("SourceWindow");
    if (sourceWindow == nullptr)
        return std::nullopt;

    const double startSeconds = sourceWindow->getDoubleAttribute("startSeconds");
    double endSeconds = 0.0;
    if (archiveVersion >= kContentArchiveVersionExplicitSourceEnd)
    {
        if (!sourceWindow->hasAttribute("endSeconds"))
            return std::nullopt;
        endSeconds = sourceWindow->getDoubleAttribute("endSeconds");
    }
    else
    {
        // Earlier archives stored duration; end reconstruction stays exclusive
        // to those versions.
        const double durationSeconds = sourceWindow->getDoubleAttribute("durationSeconds");
        if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
            return std::nullopt;
        endSeconds = startSeconds + durationSeconds;
    }

    if (!std::isfinite(startSeconds)
        || !std::isfinite(endSeconds)
        || endSeconds <= startSeconds)
        return std::nullopt;

    const auto archivedSourceId = sourceWindow->getStringAttribute("sourcePersistentId");
    if (archivedSourceId.isEmpty())
        return std::nullopt;

    auto parentId = juce::String(parentSource.getPersistentID());
    if (hostFilter != nullptr)
    {
        for (ARA::ARASize i = 0; i < hostFilter->audioSourceIDsCount; ++i)
        {
            if (archivedSourceId != hostFilter->audioSourceArchiveIDs[i])
                continue;

            const auto currentId = juce::String::fromUTF8(hostFilter->audioSourceCurrentIDs != nullptr
                ? hostFilter->audioSourceCurrentIDs[i] : hostFilter->audioSourceArchiveIDs[i]);
            if (currentId != parentId)
                return std::nullopt;
            parentId = currentId;
            break;
        }
    }

    return SourceWindow{0, std::move(parentId), startSeconds, endSeconds};
}

const ContentRenderService* OpenTuneDocumentController::getContentRenderService() const noexcept
{
    return contentRenderService_.get();
}

std::shared_ptr<ContentRenderService> OpenTuneDocumentController::getContentRenderServiceShared() const noexcept
{
    return contentRenderService_;
}

NotePitchAudition& OpenTuneDocumentController::getNotePitchAudition() noexcept
{
    return *notePitchAudition_;
}

bool OpenTuneDocumentController::selectNotePitchAuditionRendererForRegion(
    juce::ARAPlaybackRegion* region) noexcept
{
    JUCE_ASSERT_MESSAGE_THREAD
    juce::ARAEditorRenderer* selected = nullptr;
    if (region != nullptr)
    {
        if (auto* controller = getDocumentController())
        {
            for (auto* renderer : controller->getEditorRenderers<juce::ARAEditorRenderer>())
            {
                if (editorRendererOwnsRegion(renderer, region))
                {
                    selected = renderer;
                    break;
                }
            }
        }
    }

    auditionRoute_.notePitchMixer.store(selected, std::memory_order_release);
    notePitchAudition_->setTargetRegion(selected != nullptr ? region : nullptr);

    return selected != nullptr;
}

void OpenTuneDocumentController::setKeyboardAuditionSource(std::shared_ptr<NoteAudition> source)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (source != nullptr)
        keyboardAuditionSources_.push_back(std::move(source));

    // Retained by keyboardAuditionSources_ for the document lifetime: an audio
    // callback that already loaded the pointer can finish without a lock.
    // Ownership of keyboardMixer is never reset here: the first editor renderer
    // to observe a published source keeps mixing every later source, so two
    // renderers can never mix the same NoteAudition concurrently. Ownership is
    // released only by the owning renderer's destructor.
    auditionRoute_.keyboardAudition.store(
        keyboardAuditionSources_.empty() ? nullptr : keyboardAuditionSources_.back().get(),
        std::memory_order_release);
}

OpenTuneDocumentController::RenderTerminalSubscription
OpenTuneDocumentController::subscribeRenderTerminalCallback(RenderTerminalCallback callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    return renderTerminalRelay_ != nullptr
        ? renderTerminalRelay_->subscribe(std::move(callback))
        : 0;
}

void OpenTuneDocumentController::unsubscribeRenderTerminalCallback(RenderTerminalSubscription subscription)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (renderTerminalRelay_ != nullptr)
        renderTerminalRelay_->unsubscribe(subscription);
}

OpenTuneDocumentController::RenderTerminalRelay::Subscription
OpenTuneDocumentController::RenderTerminalRelay::subscribe(RenderTerminalCallback next)
{
    const std::lock_guard<std::mutex> lock(mutex);
    const auto subscription = nextSubscription++;
    callbacks.emplace(subscription, std::move(next));
    return subscription;
}

void OpenTuneDocumentController::RenderTerminalRelay::unsubscribe(Subscription subscription)
{
    const std::lock_guard<std::mutex> lock(mutex);
    callbacks.erase(subscription);
}

void OpenTuneDocumentController::RenderTerminalRelay::clear()
{
    const std::lock_guard<std::mutex> lock(mutex);
    callbacks.clear();
}

void OpenTuneDocumentController::RenderTerminalRelay::invoke(ContentKey key, bool completed)
{
    std::vector<RenderTerminalCallback> current;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        current.reserve(callbacks.size());
        for (const auto& subscription : callbacks)
            current.push_back(subscription.second);
    }
    for (const auto& callback : current)
        if (callback)
            callback(key, completed);
}

void OpenTuneDocumentController::RenderTerminalRelay::invokeCompleted(
    ContentKey key, uint64_t renderGeneration)
{
    std::vector<RenderTerminalCallback> current;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (callbacks.empty())
            return;
        // A multi-chunk render queues one settled callback per chunk; every
        // queued callback observes a fully settled cache by the time it runs.
        // Claim the success once per key+render generation so only one terminal
        // notification reaches the editor for the same render generation.
        const auto it = lastCompletedRenderGeneration.find(key);
        if (it != lastCompletedRenderGeneration.end()
            && it->second == renderGeneration)
            return;
        lastCompletedRenderGeneration[key] = renderGeneration;
        current.reserve(callbacks.size());
        for (const auto& subscription : callbacks)
            current.push_back(subscription.second);
    }
    for (const auto& callback : current)
        if (callback)
            callback(key, true);
}

bool OpenTuneDocumentController::PlaybackRegionProjection::isPlaybackRenderable() const noexcept
{
    return contentKey.isValid()
        && endInPlaybackTime > startInPlaybackTime
        && endInModificationTime > startInModificationTime
        && durationInPlaybackTime == durationInModificationTime;
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

int OpenTuneDocumentController::requestReadAudioForPlaybackRegion(
    juce::ARAPlaybackRegion* playbackRegion)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (playbackRegion == nullptr)
        return -1;

    if (findPlaybackRegion(playbackRegion) == nullptr)
        return -1;

    auto* modification = findAudioModification(playbackRegion->getAudioModification());
    if (modification == nullptr || !modification->hasContentState())
        return -1;

    auto* source = findAudioSource(modification->audioModification->getAudioSource());
    if (source == nullptr || !source->getShape().isValid())
    {
        const auto reason = source == nullptr
            ? std::string("ARA Read could not resolve its audio source")
            : "ARA Read audio source shape is invalid (channels="
                + std::to_string(source->getShape().numChannels) + ", samples="
                + std::to_string(source->getShape().numSamples) + ", sampleRate="
                + std::to_string(source->getShape().sourceSampleRate) + ")";
        modification->birthState = AudioModificationBirthState::Failed;
        modification->applyOriginalF0State(OriginalF0State::Failed);
        applyOriginalF0FailureReasonToModification(modification->contentKey(), reason);
        return -1;
    }
    if (source->getAraAudioSource()->isDeactivatedForUndoHistory()
        || modification->audioModification->isDeactivatedForUndoHistory())
        return -1;

    const auto currentStamp = makeF0InputStamp(*source, *modification);

    const bool hasValidF0 = modification->hasReadyOriginalF0();
    const bool sameStamp = modification->originalF0InputStamp == currentStamp;
    const auto task = f0Flights_.taskForReceiver(modification->wrapperGeneration);
    const bool hasLiveReceiver = task.value != 0
        && ProcessF0Runtime::getInstance().araAnalysisQueue()->isActive(task);
    if (sameStamp && (hasLiveReceiver || (hasValidF0 && modification->birthState == AudioModificationBirthState::Ready)))
    {
        AppLogger::log(f0Diagnostic(this, modification->persistentId, modification->birthRevision)
            + "read_audio_same_input");
        return 0;
    }

    const bool materialized = birthContentForModification(*modification, *source, currentStamp);
    refreshRegisteredRenderers(publishModelChange());
    return materialized ? 1 : -1;
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
    collectPlaybackSnapshots();
}

void OpenTuneDocumentController::collectPlaybackSnapshots()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    for (auto* renderer : playbackRenderers_)
        if (renderer != nullptr)
            renderer->collectPlaybackSnapshots();
    if (contentRenderService_ != nullptr)
        contentRenderService_->collectPlaybackSnapshots();
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
    initializePendingNoteTopologies();
    readRestoredAudio(nullptr);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioModificationProperties(juce::ARAAudioModification* audioModification)
{
    // 唯一 identity binding 点：doCreateAudioModification hook 只按 host pointer
    // 注册 DTO，persistentID 在其返回后由 SDK updateProperties 写入。
    auto& modification = *findAudioModification(audioModification);
    const bool created = modification.persistentId.isEmpty();
    modification.updateIdentity(audioModification);
    bindAudioModificationIdentity(modification);
    auto* parent = audioModification->getAudioSource();
    if (created)
        AppLogger::log(araDiagnostic(this) + "createAudioModification persistentID="
            + modification.persistentId.quoted() + " source=" + juce::String(parent->getPersistentID()).quoted());
    if (auto* source = findAudioSource(parent))
        modification.attachSource(*source);
    if (created)
        modification.registerNoteTopologyInitialization(
            AudioEditingScheme::usesNotesPrimaryScheme(appPreferences_.getAudioEditingScheme()));
    // Note: birthContentForModification() called lazily on read request, not here

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDeactivateAudioModificationForUndoHistory(
    juce::ARAAudioModification* audioModification, bool deactivate)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto& modification = *findAudioModification(audioModification);
    AppLogger::log(f0Diagnostic(this, modification.persistentId, modification.birthRevision)
        + "deactivateAudioModificationForUndoHistory edge="
        + (deactivate ? "active->inactive reason=ModificationUndo" : "inactive->active"));
    if (deactivate)
        revokeF0Reception(modification);
}

void OpenTuneDocumentController::willDestroyAudioModification(juce::ARAAudioModification* audioModification)
{
    auto& modification = *findAudioModification(audioModification);
    AppLogger::log(araDiagnostic(this) + "destroyAudioModification persistentID="
        + modification.persistentId.quoted() + " reason=Orphaned");
    revokeF0Reception(modification);
    removeCRSArtifactsForModification(modification);

    // The host has already destroyed the associated playback regions.
    // Keep the persistent ID to ContentKey mapping for a future wrapper.
    audioModifications_.remove_if([audioModification](const AudioModification& m)
                                  {
                                      return m.audioModification == audioModification;
                                  });

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
    const auto& source = ensureAudioSource(audioSource);
    AppLogger::log(araDiagnostic(this) + "createAudioSource persistentID="
        + source.getIdentity().persistentId.quoted());
}

void OpenTuneDocumentController::didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
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
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (scopeFlags.affectSamples())
    {
        auto& source = *findAudioSource(audioSource);
        AppLogger::log(araDiagnostic(this) + "audio_source_samples_changed source="
            + source.getIdentity().persistentId.quoted() + " reason=SamplesChanged");
        abortSourceF0(source, AraF0Abort::SamplesChanged);
        source.incrementSampleContentGeneration();
        for (auto& modification : audioModifications_)
        {
            if (modification.hasContentState()
                && modification.audioModification->getAudioSource() == audioSource)
            {
                removeCRSArtifactsForModification(modification);
                modification.clearOriginalAnalysis();
            }
        }
    }

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                    bool enable)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto& source = *findAudioSource(audioSource);
    // ARA dispatches this hook only when the requested value differs from the current one.
    AppLogger::log(araDiagnostic(this) + "enableAudioSourceSamplesAccess source="
        + source.getIdentity().persistentId.quoted()
        + (enable ? " edge=false->true" : " edge=true->false reason=SamplesDisabled"));
    if (enable)
        return;

    abortSourceF0(source, AraF0Abort::SamplesDisabled);
    source.incrementSampleContentGeneration();
    source.clearReaderLease();
}

void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                     bool enable)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (enable && !audioSource->isDeactivatedForUndoHistory())
    {
        auto& source = *findAudioSource(audioSource);
        source.createReaderLease();
        // Inside an edit, didEndEditing reads the completed graph.
        if (!getDocumentController()->isHostEditingDocument())
            readRestoredAudio(&source);
    }

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDeactivateAudioSourceForUndoHistory(juce::ARAAudioSource* audioSource,
                                                                        bool deactivate)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto& source = *findAudioSource(audioSource);
    AppLogger::log(araDiagnostic(this) + "deactivateAudioSourceForUndoHistory source="
        + source.getIdentity().persistentId.quoted()
        + (deactivate ? " edge=active->inactive reason=SourceUndo" : " edge=inactive->active"));
    if (!deactivate)
        return;

    abortSourceF0(source, AraF0Abort::SourceUndo);
    source.incrementSampleContentGeneration();
    source.clearReaderLease();
}

void OpenTuneDocumentController::didDeactivateAudioSourceForUndoHistory(juce::ARAAudioSource* audioSource,
                                                                       bool deactivate)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    // The host can retain its sample grant across undo/redo. Restore only the reader here.
    if (!deactivate && audioSource->isSampleAccessEnabled())
        findAudioSource(audioSource)->createReaderLease();
}

void OpenTuneDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    juce::ignoreUnused(audioModification);
    removePlaybackRegion(playbackRegion);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyAudioSource(juce::ARAAudioSource* audioSource)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto& destroyedSource = *findAudioSource(audioSource);
    AppLogger::log(araDiagnostic(this) + "destroyAudioSource persistentID="
        + destroyedSource.getIdentity().persistentId.quoted() + " reason=Orphaned");
    abortSourceF0(destroyedSource, AraF0Abort::Orphaned);
    // All modifications have already been destroyed. Erasing the source destroys its reader.
    audioSources_.erase(std::remove_if(audioSources_.begin(), audioSources_.end(),
                                       [audioSource](const AudioSource& source)
                                       {
                                           return source.matches(audioSource);
                                       }),
                        audioSources_.end());

    refreshRegisteredRenderers(publishModelChange());
}

bool OpenTuneDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                                             const juce::ARARestoreObjectsFilter* filter)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto logPrefix = araDiagnostic(this) + "doRestoreObjectsFromStream";
    const auto* hostFilter = filter->getHostFilter();
    AppLogger::log(logPrefix + " begin documentData="
        + (filter->shouldRestoreDocumentData() ? "true" : "false")
        + " filter=" + (hostFilter != nullptr ? "explicit" : "full"));
    const auto logMapping = [&](const char* object, const juce::String& archiveId, const juce::String& currentId) {
        AppLogger::log(logPrefix + " filter_object=" + object
            + " archive=" + archiveId.quoted() + " current=" + currentId.quoted());
    };
    if (hostFilter != nullptr)
    {
        const auto logMappings = [&](const char* object, ARA::ARASize count,
                                     const ARA::ARAPersistentID* archiveIds,
                                     const ARA::ARAPersistentID* currentIds) {
            AppLogger::log(logPrefix + " filter_object=" + object
                + " count=" + juce::String(static_cast<juce::uint64>(count)));
            for (ARA::ARASize i = 0; i < count; ++i)
                logMapping(object, juce::String::fromUTF8(archiveIds[i]),
                    juce::String::fromUTF8(currentIds != nullptr ? currentIds[i] : archiveIds[i]));
        };
        logMappings("source", hostFilter->audioSourceIDsCount,
                    hostFilter->audioSourceArchiveIDs, hostFilter->audioSourceCurrentIDs);
        logMappings("modification", hostFilter->audioModificationIDsCount,
                    hostFilter->audioModificationArchiveIDs, hostFilter->audioModificationCurrentIDs);
    }
    else
    {
        for (const auto* source : getDocument()->getAudioSources())
        {
            const auto sourceId = juce::String(source->getPersistentID());
            logMapping("source", sourceId, sourceId);
            for (const auto* modification : source->getAudioModifications())
            {
                const auto id = juce::String(modification->getPersistentID());
                logMapping("modification", id, id);
            }
        }
    }

    const auto finish = [&](bool ok, size_t applied, const juce::String& detail) {
        AppLogger::log(logPrefix + " end result=" + (ok ? "true" : "false")
            + " applied=" + juce::String(static_cast<juce::uint64>(applied)) + detail);
        if (ok && applied == 0)
            AppLogger::log(logPrefix + " restore_true_zero_applied");
        return ok;
    };

    const int magic = input.readInt();
    const int version = input.readInt();
    const int bindingCount = input.readInt();
    if (input.failed() || input.getPosition() != 3 * sizeof(int32_t))
        return finish(false, 0, " reason=archive_header_read");
    if (magic != kContentPayloadArchiveMagic)
        return finish(false, 0, " reason=archive_magic");
    if (version < kContentPayloadArchiveVersionMin || version > kContentArchiveVersion)
        return finish(false, 0, " reason=archive_version version=" + juce::String(version));
    if (bindingCount < 0 || bindingCount > kMaxContentPayloadRecords)
        return finish(false, 0, " reason=archive_record_count records=" + juce::String(bindingCount));
    AppLogger::log(logPrefix + " version=" + juce::String(version) + " records=" + juce::String(bindingCount));

    struct PendingRestore
    {
        AudioModification& target;
        const AudioSource& source;
        RestoredModificationContent restored;
    };
    std::vector<PendingRestore> pending;
    pending.reserve(bindingCount);
    for (int i = 0; i < bindingCount; ++i)
    {
        const auto start = input.getPosition();
        const auto archivedId = input.readString();
        const auto xmlString = input.readString();
        auto record = " record=" + juce::String(i) + " archive=" + archivedId.quoted();
        const auto reject = [&](const char* reason) {
            return finish(false, 0, record + " decode=failed reason=" + reason);
        };
        if (input.failed()
            || input.getPosition() - start != static_cast<juce::int64>(archivedId.getNumBytesAsUTF8() + xmlString.getNumBytesAsUTF8() + 2))
            return reject("archive_record_read");

        auto* araTarget = filter->getAudioModificationToRestoreStateWithID<juce::ARAAudioModification>(archivedId.toRawUTF8());
        auto* target = araTarget != nullptr ? findAudioModification(araTarget) : nullptr;
        if (target == nullptr)
        {
            AppLogger::log(logPrefix + record + " decode=skipped_no_target");
            continue;
        }

        auto* parent = target->audioModification->getAudioSource();
        record += " current=" + target->persistentId.quoted()
            + " parent=" + juce::String(parent->getPersistentID()).quoted();
        const auto* source = findAudioSource(parent);
        if (source == nullptr)
            return reject("missing_live_parent");
        const auto xml = juce::XmlDocument::parse(xmlString);
        if (xml == nullptr || !xml->hasTagName("AudioModificationContent"))
            return reject("modification_xml");
        auto sourceWindow = readSourceWindowFromArchive(*xml, *parent, hostFilter, version);
        if (!sourceWindow.has_value())
            return reject("source_window_or_parent_mapping");

        const auto& shape = source->getShape();
        const auto sourceDuration = shape.durationSeconds();
        if (!std::isfinite(sourceDuration) || sourceWindow->sourceStartSeconds < 0.0
            || sourceWindow->sourceEndSeconds > sourceDuration + 1.0e-6)
            return reject("source_window_outside_parent");

        // The archived seconds stay authoritative. These addresses only validate the readable span.
        const auto windowStartSample = TimeCoordinate::secondsToSamplesNearest(
            sourceWindow->sourceStartSeconds, shape.sourceSampleRate);
        const auto windowEndSample = TimeCoordinate::secondsToSamplesNearest(
            sourceWindow->sourceEndSeconds, shape.sourceSampleRate);
        if (windowStartSample < 0 || windowStartSample >= windowEndSample || windowEndSample > shape.numSamples)
            return reject("source_window_unreadable");

        auto restored = restoreAudioModificationContent(*xml, std::move(*sourceWindow), version);
        if (!restored.has_value())
            return reject("modification_content");
        AppLogger::log(logPrefix + record + " decode=ok analysis_state="
            + juce::String(static_cast<int>(restored->content.analysis.originalF0State))
            + " continuation=" + (restored->analysisContinuationPending ? "true" : "false"));
        pending.push_back({*target, *source, std::move(*restored)});
    }

    const bool notesPrimary = AudioEditingScheme::usesNotesPrimaryScheme(appPreferences_.getAudioEditingScheme());
    for (auto& entry : pending)
    {
        auto& target = entry.target;
        AppLogger::log(f0Diagnostic(this, target.persistentId, target.birthRevision)
            + "revoke_reception reason=RestoreReplacement");
        revokeF0Reception(target);
        removeCRSArtifactsForModification(target);
        target.content = std::move(entry.restored.content);
        target.birthState = AudioModificationBirthState::WaitingForSource;
        target.analysisContinuationPending = entry.restored.analysisContinuationPending;
        target.registerNoteTopologyInitialization(notesPrimary);
        target.originalF0InputStamp.reset();
        target.attachSource(entry.source);
        // Archived F0 is adopted only when its recorded input stamp still
        // matches the live parent source. A mismatch (or a stamp-less older
        // archive) keeps the user's edits, clears the stale analysis and
        // re-enters analysis once through the pending continuation.
        const auto liveStamp = makeF0InputStamp(entry.source, target);
        if (target.hasReadyOriginalF0())
        {
            if (entry.restored.originalF0InputStamp
                && *entry.restored.originalF0InputStamp == liveStamp)
            {
                AppLogger::log(f0Diagnostic(this, target.persistentId, target.birthRevision)
                    + "restore_adopted_archive_f0 stamp=" + liveStamp.sourcePersistentId.quoted());
                target.originalF0InputStamp = liveStamp;
            }
            else
            {
                AppLogger::log(f0Diagnostic(this, target.persistentId, target.birthRevision)
                    + "restore_stamp_mismatch clear_original_analysis reason=RestoreStampMismatch");
                target.clearOriginalAnalysis();
                target.analysisContinuationPending = true;
            }
        }
    }

    // The host initiated the restore. Publish the completed batch to internal views only.
    if (!pending.empty())
        refreshRegisteredRenderers(publishModelChange());
    return finish(true, pending.size(), {});
}

bool OpenTuneDocumentController::doStoreObjectsToStream(juce::ARAOutputStream& output,
                                                          const juce::ARAStoreObjectsFilter* filter)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto logPrefix = araDiagnostic(this) + "doStoreObjectsToStream";
    const auto& sources = filter->getAudioSourcesToStore();
    const auto& modifications = filter->getAudioModificationsToStore();
    juce::StringArray sourceIds, modificationIds;
    for (const auto* source : sources)
        sourceIds.add(juce::String(source->getPersistentID()).quoted());

    std::vector<const AudioModification*> bindings;
    bindings.reserve(modifications.size());
    for (auto* araMod : modifications)
    {
        const auto id = juce::String(araMod->getPersistentID());
        modificationIds.add(id.quoted());
        const auto* modification = findAudioModification(id);
        if (modification != nullptr && modification->hasContentState())
            bindings.push_back(modification);
    }
    AppLogger::log(logPrefix + " documentData=" + (filter->shouldStoreDocumentData() ? "true" : "false")
        + " source_count=" + juce::String(sourceIds.size()) + " sources=[" + sourceIds.joinIntoString(", ") + "]"
        + " modification_count=" + juce::String(modificationIds.size()) + " modifications=["
        + modificationIds.joinIntoString(", ") + "] records="
        + juce::String(static_cast<juce::uint64>(bindings.size())));

    if (bindings.size() > static_cast<size_t>(kMaxContentPayloadRecords))
        return false;

    bool ok = output.writeInt(kContentPayloadArchiveMagic);
    ok = output.writeInt(kContentArchiveVersion) && ok;
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
    dto.wrapperGeneration = nextWrapperGeneration_++;

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
    return new juce::ARAEditorView(getDocumentController());
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
    projection.endInPlaybackTime = playbackRegion->getEndInPlaybackTime();
    projection.startInModificationTime = playbackRegion->getStartInAudioModificationTime();
    projection.endInModificationTime = playbackRegion->getEndInAudioModificationTime();
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

std::vector<OpenTunePlaybackRenderer*> OpenTuneDocumentController::publishModelChange()
{
    presentationChanges_.sendChangeMessage();
    return playbackRenderers_;
}

void OpenTuneDocumentController::refreshRegisteredRenderers(const std::vector<OpenTunePlaybackRenderer*>& renderers)
{
    for (auto* renderer : renderers)
        if (renderer != nullptr)
            renderer->refreshRenderPlanFromDocument();
    collectPlaybackSnapshots();
}

bool OpenTuneDocumentController::publishPlaybackReadSourceForModification(
    AudioModification& modification,
    NativePcmSource nativeSource,
    std::shared_ptr<const juce::AudioBuffer<float>> canonicalBuffer,
    int64_t firstCanonicalSample)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (contentRenderService_ == nullptr)
        return false;

    if (nativeSource.buffer == nullptr || canonicalBuffer == nullptr)
        return false;

    if (!modification.hasContentState())
        return false;

    const auto key = modification.contentKey();

    auto readSource = makePlaybackReadSource(
        key,
        modification.snapshotContent(),
        std::move(nativeSource),
        std::move(canonicalBuffer),
        TimeCoordinate::kRenderSampleRate,
        firstCanonicalSample,
        contentRenderService_->getOrCreateRenderCache(key),
        contentRenderService_->getTimeStretchCache());

    contentRenderService_->publishPlaybackSource(key, std::move(readSource));

    // The plan freezes the published source view; a new publication is the
    // event that refreshes it.
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

void OpenTuneDocumentController::audioEditingSchemeChanged(AudioEditingScheme::Scheme scheme)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const bool notesPrimary = AudioEditingScheme::usesNotesPrimaryScheme(scheme);
    for (auto& modification : audioModifications_)
        modification.registerNoteTopologyInitialization(notesPrimary);

    if (notesPrimary && !getDocumentController()->isHostEditingDocument())
        initializePendingNoteTopologies();
    presentationChanges_.sendChangeMessage();
}

void OpenTuneDocumentController::initializePendingNoteTopologies()
{
    for (auto& modification : audioModifications_)
        initializePendingNoteTopology(modification);
}

bool OpenTuneDocumentController::initializePendingNoteTopology(AudioModification& modification)
{
    if (!modification.noteTopologyInitPending
        || !modification.hasReadyOriginalF0()
        || modification.content->noteTopologyInitialized
        || modification.audioModification->isDeactivatedForUndoHistory()
        || modification.audioModification->getAudioSource()->isDeactivatedForUndoHistory())
        return false;

    return generateNotesOnly(modification, {});
}

bool OpenTuneDocumentController::generateNotesOnly(ContentKey key, const NoteGeneratorParams& params)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    auto* modification = findAudioModificationByContentKey(key);
    return modification != nullptr && modification->hasContentState() && generateNotesOnly(*modification, params);
}

bool OpenTuneDocumentController::generateNotesOnly(AudioModification& modification, const NoteGeneratorParams& params)
{
    const auto snapshot = modification.snapshotContent();
    const auto patch = ContentEditCommands::planGenerateNotesOnly(*snapshot, params);
    if (!patch)
        return false;

    applyNotesToModification(modification.contentKey(),
                             mergeNotesRange(snapshot->notes, patch->affectedRange, patch->afterNotesInRange));
    requestFullModificationRender(modification.contentKey());
    return true;
}

void OpenTuneDocumentController::readRestoredAudio(const AudioSource* enabledSource)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    // Permission never creates intent. Only archived F0 or an explicit analysis
    // request can enter this path after endEditing or a late sample-access grant.
    for (auto& mod : audioModifications_)
    {
        if (mod.birthState != AudioModificationBirthState::WaitingForSource
            || (!mod.analysisContinuationPending && !mod.hasReadyOriginalF0()))
            continue;

        auto* parent = mod.audioModification->getAudioSource();
        const auto* source = findAudioSource(parent);
        if ((enabledSource != nullptr && source != enabledSource)
            || !source->canReadSamples()
            || parent->isDeactivatedForUndoHistory()
            || mod.audioModification->isDeactivatedForUndoHistory())
            continue;

        const auto currentStamp = makeF0InputStamp(*source, mod);
        // Without analysis intent, only matching Ready content can materialize its PCM.
        if (!mod.analysisContinuationPending
            && (mod.originalF0InputStamp != currentStamp
                || std::none_of(playbackRegions_.begin(), playbackRegions_.end(), [&](const PlaybackRegion& region) {
                    return region.hasValidPlacement()
                        && region.playbackRegion->getAudioModification() == mod.audioModification;
                })))
            continue;

        birthContentForModification(mod, *source, currentStamp);
    }
}

OriginalF0InputStamp OpenTuneDocumentController::makeF0InputStamp(const AudioSource& source, const AudioModification& modification)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
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

bool OpenTuneDocumentController::birthContentForModification(AudioModification& modification,
                                                            const AudioSource& source,
                                                             const OriginalF0InputStamp& currentStamp)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto& content = *modification.content;
    const bool alreadyHasF0 = modification.originalF0InputStamp == currentStamp
        && modification.hasReadyOriginalF0();
    const bool wasPending = modification.analysisContinuationPending;
    modification.registerNoteTopologyInitialization(
        AudioEditingScheme::usesNotesPrimaryScheme(appPreferences_.getAudioEditingScheme()));
    AppLogger::log(f0Diagnostic(this, modification.persistentId, modification.birthRevision)
        + "revoke_reception reason=Birth");
    revokeF0Reception(modification);
    if (!alreadyHasF0)
    {
        removeCRSArtifactsForModification(modification);
        if (modification.originalF0InputStamp.has_value() && modification.originalF0InputStamp != currentStamp)
            AppLogger::log(f0Diagnostic(this, modification.persistentId, modification.birthRevision)
                + "clear_original_analysis reason=stamp_mismatch");
        modification.clearOriginalAnalysis();
    }

    const auto failBirth = [this, &modification](std::string reason) {
        modification.birthState = AudioModificationBirthState::Failed;
        modification.applyOriginalF0State(OriginalF0State::Failed);
        applyOriginalF0FailureReasonToModification(
            modification.contentKey(), std::move(reason));
        return false;
    };

    if (content.sourceWindow.sourcePersistentId.isEmpty() || !content.sourceWindow.isValid())
        return failBirth("ARA source window is invalid (persistentId="
            + content.sourceWindow.sourcePersistentId.toStdString()
            + ", startSeconds=" + std::to_string(content.sourceWindow.sourceStartSeconds)
            + ", endSeconds=" + std::to_string(content.sourceWindow.sourceEndSeconds) + ")");

    const auto& sourceShape = source.getShape();
    if (!sourceShape.isValid())
        return failBirth("ARA audio source shape is invalid (channels="
            + std::to_string(sourceShape.numChannels) + ", samples="
            + std::to_string(sourceShape.numSamples) + ", sampleRate="
            + std::to_string(sourceShape.sourceSampleRate) + ")");

    if (!source.canReadSamples())
    {
        modification.birthState = AudioModificationBirthState::WaitingForSource;
        modification.analysisContinuationPending = !alreadyHasF0;
        if (modification.analysisContinuationPending && !wasPending)
            modification.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes::tuningIsAffected(), true);
        return false;
    }

    modification.originalF0InputStamp = currentStamp;

    // 1. Authoritative source window. Seconds stay untouched; sample addresses
    //    below are derived operation coordinates.
    const auto sourceWindow = content.sourceWindow;

    const double sourceSampleRate = sourceShape.sourceSampleRate;
    const int numChannels = sourceShape.numChannels;
    const uint64_t audioRevision = source.getSampleContentGeneration();

    // 2. Retain the actual native PCM. A full-source read is reused by every
    //    modification of the same source identity and sample content generation
    //    through the already-published CRS views.
    NativePcmSource nativeSource;
    nativeSource.buffer = findReusableNativePcm(modification,
                                                sourceWindow.sourcePersistentId,
                                                sourceSampleRate,
                                                numChannels,
                                                audioRevision,
                                                sourceShape.numSamples);
    if (nativeSource.buffer == nullptr)
    {
        if (sourceShape.numSamples > static_cast<int64_t>(std::numeric_limits<int>::max()))
            return failBirth("ARA source is too large for a native PCM buffer (samples="
                + std::to_string(sourceShape.numSamples) + ")");

        juce::AudioBuffer<float> nativePcm(numChannels, static_cast<int>(sourceShape.numSamples));

        constexpr int64_t kChunkSamples = 32768;
        int64_t readOffset = 0;
        while (readOffset < sourceShape.numSamples)
        {
            const int64_t chunkSamples = std::min(kChunkSamples, sourceShape.numSamples - readOffset);
            const int chunkOffset = static_cast<int>(readOffset);

            std::vector<void*> channelPointers(static_cast<size_t>(numChannels));
            for (int ch = 0; ch < numChannels; ++ch)
                channelPointers[static_cast<size_t>(ch)] = nativePcm.getWritePointer(ch, chunkOffset);

            if (!source.readAudioSamples(readOffset, static_cast<int>(chunkSamples), channelPointers.data()))
            {
                return failBirth("ARA host audio reader failed (startSample="
                    + std::to_string(readOffset) + ", samples="
                    + std::to_string(chunkSamples) + ", channels="
                    + std::to_string(numChannels) + ")");
            }

            readOffset += chunkSamples;
        }

        nativeSource.buffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(nativePcm));
    }

    nativeSource.sampleRate = sourceSampleRate;
    nativeSource.firstSourceSample = 0;
    nativeSource.audioRevision = audioRevision;
    content.nativeSource = nativeSource;

    // 3. Canonical editing input derived directly from the native PCM. Its first
    //    element is the Q projection of the authoritative window start.
    const double canonicalRate = TimeCoordinate::kRenderSampleRate;
    const int64_t firstCanonicalSample = PlaybackSampleGeometry::projectToSample(
        sourceWindow.sourceStartSeconds * canonicalRate);
    const int64_t canonicalEndSample = std::max(firstCanonicalSample + 1,
        PlaybackSampleGeometry::projectToSample(sourceWindow.sourceEndSeconds * canonicalRate));
    const int64_t canonicalLength = canonicalEndSample - firstCanonicalSample;

    if (canonicalLength > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return failBirth("ARA source window exceeds canonical buffer capacity (firstCanonical="
            + std::to_string(firstCanonicalSample) + ", endCanonical="
            + std::to_string(canonicalEndSample) + ")");
    }

    std::shared_ptr<const juce::AudioBuffer<float>> canonicalBuffer;
    if (sourceSampleRate == canonicalRate)
    {
        const int64_t localStart = firstCanonicalSample - nativeSource.firstSourceSample;
        const int64_t available = nativeSource.buffer->getNumSamples() - localStart;
        const int64_t copyLength = std::min(canonicalLength, available);
        if (copyLength <= 0)
        {
            return failBirth("ARA source window lies outside the native PCM (localStart="
                + std::to_string(localStart) + ", nativeSamples="
                + std::to_string(nativeSource.buffer->getNumSamples()) + ")");
        }

        if (localStart == 0 && copyLength == nativeSource.buffer->getNumSamples())
        {
            canonicalBuffer = nativeSource.buffer;
        }
        else
        {
            juce::AudioBuffer<float> sliced(numChannels, static_cast<int>(copyLength));
            for (int ch = 0; ch < numChannels; ++ch)
                sliced.copyFrom(ch, 0, *nativeSource.buffer,
                                ch % nativeSource.buffer->getNumChannels(),
                                static_cast<int>(localStart),
                                static_cast<int>(copyLength));
            canonicalBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(sliced));
        }
    }
    else
    {
        juce::AudioBuffer<float> converted(numChannels, static_cast<int>(canonicalLength));
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto resampledData = resamplingManager_->resampleRange(
                nativeSource.buffer->getReadPointer(ch),
                static_cast<size_t>(nativeSource.buffer->getNumSamples()),
                sourceSampleRate,
                canonicalRate,
                nativeSource.firstSourceSample,
                firstCanonicalSample,
                static_cast<int>(canonicalLength));
            const int toCopy = juce::jmin(static_cast<int>(canonicalLength),
                                          static_cast<int>(resampledData.size()));
            converted.copyFrom(ch, 0, resampledData.data(), toCopy);
        }
        canonicalBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(converted));
    }

    content.audioBuffer = canonicalBuffer;
    content.sampleRate = canonicalRate;
    content.firstCanonicalSample = firstCanonicalSample;
    content.audioRevision = audioRevision;

    // 4. Channel 0 for async F0 uses the window-local original native span.
    AraF0Pcm pcm{{}, sourceSampleRate, numChannels};
    if (!alreadyHasF0)
    {
        const int64_t f0Start = TimeCoordinate::secondsToSamplesNearest(
            sourceWindow.sourceStartSeconds, sourceSampleRate) - nativeSource.firstSourceSample;
        const int64_t f0End = TimeCoordinate::secondsToSamplesNearest(
            sourceWindow.sourceEndSeconds, sourceSampleRate) - nativeSource.firstSourceSample;
        const int64_t available = nativeSource.buffer->getNumSamples() - f0Start;
        const int64_t f0Samples = std::min(f0End - f0Start, available);
        if (f0Start < 0 || f0Samples <= 0)
        {
            return failBirth("ARA native window has no readable samples (startSample="
                + std::to_string(f0Start) + ", samples="
                + std::to_string(f0Samples) + ")");
        }

        const float* ch0Read = nativeSource.buffer->getReadPointer(0) + static_cast<int>(f0Start);
        pcm.channel0.assign(ch0Read, ch0Read + f0Samples);
    }

    // 5. Silent gaps are detected on the canonical editing input.
    if (!alreadyHasF0)
    {
        modification.submitSilentGaps(SilentGapDetector::detectAllGapsAdaptive(*canonicalBuffer));
        modification.applyOriginalF0State(OriginalF0State::Extracting);
    }

    // 6. Publish the complete source view: native PCM, canonical input and the
    //    immutable editable snapshot in one CRS publication.
    if (!publishPlaybackReadSourceForModification(modification,
                                                  nativeSource,
                                                  canonicalBuffer,
                                                  firstCanonicalSample))
    {
        return failBirth("ARA playback read source publication failed for contentKey="
            + std::to_string(modification.contentKey().objectId));
    }

    // 8. Set modification state and notify ARA host
    modification.birthState = AudioModificationBirthState::Ready;
    // 有效 F0 表示 archive/runtime 恢复，不产生新模型变更；无有效 F0 表示用户 Read，保持通知
    if (modification.audioModification != nullptr)
        modification.audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected()
                + juce::ARAContentUpdateScopes::tuningIsAffected(),
            !alreadyHasF0);

    // 有效 F0（archive 恢复）自动触发完整 Stage1 渲染，无需用户手动编辑即可出声；
    // 用户手动 Read（无有效 F0）不提前 Stage1，F0 提取完成后由既有链继续。
    if (alreadyHasF0)
        requestFullModificationRender(modification.contentKey());

    if (!alreadyHasF0)
        scheduleAsyncF0Extraction(modification, std::move(pcm), currentStamp);

    return true;
}

std::shared_ptr<const juce::AudioBuffer<float>> OpenTuneDocumentController::findReusableNativePcm(
    const AudioModification& modification,
    const juce::String& sourcePersistentId,
    double sourceSampleRate,
    int sourceChannelCount,
    uint64_t sampleGeneration,
    int64_t sourceSampleCount) const
{
    if (contentRenderService_ == nullptr || sourcePersistentId.isEmpty())
        return nullptr;

    for (const auto& candidate : audioModifications_)
    {
        if (&candidate == &modification
            || !candidate.hasContentState()
            || candidate.content->sourceWindow.sourcePersistentId != sourcePersistentId)
            continue;

        PlaybackReadSource published;
        if (!contentRenderService_->getPlaybackReadSource(candidate.contentKey(), published))
            continue;

        const auto& native = published.nativeSource;
        if (native.buffer == nullptr
            || native.buffer->getNumChannels() != sourceChannelCount
            || native.sampleRate != sourceSampleRate
            || native.audioRevision != sampleGeneration
            || native.firstSourceSample != 0
            || native.buffer->getNumSamples() != sourceSampleCount)
            continue;

        return native.buffer;
    }

    return nullptr;
}

void OpenTuneDocumentController::abortSourceF0(const AudioSource& source, AraF0Abort reason)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    for (auto& modification : audioModifications_)
        if (modification.audioModification->getAudioSource() == source.getAraAudioSource())
            modification.revokeAnalysisReception();

    // Look up tasks by source object identity; the host may have rebound the
    // source's persistentId while its flights were in flight.
    const auto sourceTasks = f0Flights_.bySource.find(source.getAraAudioSource());
    if (sourceTasks == f0Flights_.bySource.end())
        return;

    const std::set<F0TaskId> tasks = sourceTasks->second;
    for (auto id : tasks)
        f0Flights_.take(id);
    ProcessF0Runtime::getInstance().araAnalysisQueue()->abortTasks(tasks, reason);
}

void OpenTuneDocumentController::revokeDocumentF0()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    f0Flights_.sink.reset();
    if (f0Flights_.byTaskId.empty())
        return;

    std::set<F0TaskId> tasks;
    for (const auto& entry : f0Flights_.byTaskId)
        tasks.insert(entry.first);
    f0Flights_.byTaskId.clear();
    f0Flights_.byLocalKey.clear();
    f0Flights_.bySource.clear();
    ProcessF0Runtime::getInstance().araAnalysisQueue()->abortTasks(tasks, AraF0Abort::Orphaned);
}

void OpenTuneDocumentController::revokeF0Reception(AudioModification& modification)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    modification.revokeAnalysisReception();
    if (!f0Flights_.byTaskId.empty())
        ProcessF0Runtime::getInstance().araAnalysisQueue()->revokeReceiver(f0Flights_, modification.wrapperGeneration);
}

void OpenTuneDocumentController::removeCRSArtifactsForModification(const AudioModification& modification)
{
    if (contentRenderService_ == nullptr)
        return;

    const auto key = modification.contentKey();
    if (!key.isValid())
        return;

    contentRenderService_->removePlaybackSource(key);
    contentRenderService_->removeRenderCache(key);
}

// Removed rebuildCRSForSource per architecture: sample access enable is permission,
// not user intent. 新内容仅由用户 Read（requestReadAudioForPlaybackRegion）读取；
// archive 恢复且已有有效 F0 的内容在 endEditing/access 后自动重建 PCM。

void OpenTuneDocumentController::scheduleAsyncF0Extraction(
    AudioModification& modification, AraF0Pcm pcm, const OriginalF0InputStamp& stamp)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (!f0Flights_.sink)
        return;

    const auto logPrefix = f0Diagnostic(this, modification.persistentId, modification.birthRevision);
    AppLogger::log(logPrefix + "submit source=" + stamp.sourcePersistentId.quoted());
    AraF0Submission submission{AraF0Admission::Closing, {}};
    if (auto* queue = ProcessF0Runtime::getInstance().araAnalysisQueue())
    {
        const AraF0LocalKey key{stamp, F0ExtractionService::snapshotAraConfig(pcm.sourceSampleRate)};
        submission = queue->submit(f0Flights_, key, std::move(pcm),
            {modification.contentKey(), modification.wrapperGeneration,
             modification.birthRevision, modification.persistentId, stamp});
        switch (submission.admission)
        {
            case AraF0Admission::Accepted:
            case AraF0Admission::Attached:
                f0Flights_.bySource[modification.audioModification->getAudioSource()]
                    .insert(submission.taskId);
                break;
            case AraF0Admission::QueueFull:
            case AraF0Admission::InvalidInput:
            case AraF0Admission::Closing:
                break;
        }
    }
    AppLogger::log(logPrefix + "submit_result=" + juce::String(static_cast<int>(submission.admission))
        + " task=" + juce::String(static_cast<juce::uint64>(submission.taskId.value)));

    const char* reason;
    switch (submission.admission)
    {
        case AraF0Admission::Accepted:
        case AraF0Admission::Attached:
            return;
        case AraF0Admission::QueueFull:
            reason = "F0 extraction queue is full";
            break;
        case AraF0Admission::InvalidInput:
            reason = "F0 extraction submission was rejected: invalid input";
            break;
        case AraF0Admission::Closing:
            reason = "F0 extraction queue is closing";
            break;
    }
    modification.applyOriginalF0State(OriginalF0State::Failed);
    applyOriginalF0FailureReasonToModification(modification.contentKey(), reason);
}

void OpenTuneDocumentController::commitF0Completion(const AraF0Target& target, const AraF0Result& result)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (!f0Flights_.sink)
        return;

    const auto logPrefix = f0Diagnostic(this, target.modificationPersistentId, target.birthRevision);
    auto* mod = findAudioModificationByContentKey(target.key);
    if (mod == nullptr
        || mod->wrapperGeneration != target.wrapperGeneration
        || mod->birthRevision != target.birthRevision
        || !mod->hasContentState()
        || mod->persistentId != target.modificationPersistentId
        || mod->originalF0InputStamp != target.stamp)
    {
        AppLogger::log(logPrefix + "commit_discarded reason=reception_revoked");
        return;
    }

    auto* hostModification = mod->audioModification;
    auto* parent = hostModification->getAudioSource();
    const auto* source = findAudioSource(parent);
    if (source == nullptr || !source->canReadSamples()
        || parent->isDeactivatedForUndoHistory() || hostModification->isDeactivatedForUndoHistory()
        || makeF0InputStamp(*source, *mod) != target.stamp)
    {
        AppLogger::log(logPrefix + "commit_discarded reason=input_unavailable_or_changed");
        return;
    }

    AppLogger::log(logPrefix + "commit success=" + (result.errorMessage.empty() ? "true" : "false")
        + " frames=" + juce::String(static_cast<juce::uint64>(result.f0.size())));
    collectPlaybackSnapshots();
    if (!result.errorMessage.empty())
    {
        mod->applyOriginalF0State(OriginalF0State::Failed);
        applyOriginalF0FailureReasonToModification(target.key, result.errorMessage);
        AppLogger::error("ARA-F0: extraction failed reason=" + juce::String(result.errorMessage));
        return;
    }

    if (mod->content->analysis.detectedKey.origin != Origin::Manual)
    {
        F0KeyDetector detector;
        const auto detectedKey = detector.detect(result.f0, result.energy);
        if (detectedKey.origin != Origin::Unset)
            mod->applyDetectedKey(detectedKey);
    }

    std::shared_ptr<PitchCurve> pitchCurve;
    if (mod->content->analysis.pitchCurve != nullptr)
        pitchCurve = mod->content->analysis.pitchCurve->deriveCopy();
    else
        pitchCurve = std::make_shared<PitchCurve>();
    pitchCurve->replaceOriginalAnalysis(result.f0, result.energy, result.hopSize, result.f0SampleRate);
    applyOriginalF0ToModification(target.key, std::move(pitchCurve));
    AppLogger::log(logPrefix + "commit_applied");
}

// Per architecture: DC owns CRS and installs render execution lease.
// The lease is detached in destructor to prevent dangling callback.

void OpenTuneDocumentController::installDocumentRenderExecution()
{
    ContentRenderService::ExecutionLease lease;
    lease.owner = contentRenderService_.get();
    const std::weak_ptr<ContentRenderService> weakContentRenderService = contentRenderService_;
    const auto renderGate = renderCompletionGate_;
    const auto renderTerminalRelay = renderTerminalRelay_;
    lease.renderJobCallback = [weakContentRenderService, renderGate, renderTerminalRelay](RenderJob& job)
    {
        if (auto contentRenderService = weakContentRenderService.lock())
            OpenTuneDocumentController::processDocumentRenderJob(
                std::move(contentRenderService), job, renderGate, renderTerminalRelay);
    };
    lease.renderFailureCallback = [renderGate, renderTerminalRelay](const RenderJob& job, const juce::String& reason) {
        ProcessRenderRuntime::notifyApplicationRenderFailure(
            job.contentKey, job.contentRevision, job.targetRevision,
            reason, job.renderCache, renderGate);
        if (renderTerminalRelay != nullptr)
        {
            const auto key = job.contentKey;
            ProcessRenderRuntime::postAsyncCallback([renderGate, renderTerminalRelay, key] {
                const ProcessRenderRuntime::CompletionCallbackLease callbackLease(*renderGate);
                if (callbackLease)
                    renderTerminalRelay->invoke(key, false);
            });
        }
    };
    lease.backendChangedCallback = [this, renderGate]() {
        ProcessRenderRuntime::CompletionCallbackLease callbackLease(*renderGate);
        if (callbackLease)
            invalidateAllModificationCaches();
    };

    contentRenderService_->attachExecutionLease(std::move(lease));
}

void OpenTuneDocumentController::processDocumentRenderJob(
    std::shared_ptr<ContentRenderService> contentRenderService,
    RenderJob& job,
    std::shared_ptr<ProcessRenderRuntime::CompletionGate> completionGate,
    std::shared_ptr<RenderTerminalRelay> renderTerminalRelay)
{
    if (job.renderCache == nullptr)
    {
        AppLogger::error("ARA-DIAG: render job dropped without cache");
        ProcessRenderRuntime::notifyApplicationRenderFailure(
            job.contentKey, job.contentRevision, job.targetRevision,
            "ARA render job has no RenderCache", {}, completionGate);
        if (renderTerminalRelay != nullptr)
        {
            const auto key = job.contentKey;
            ProcessRenderRuntime::postAsyncCallback([completionGate, renderTerminalRelay, key] {
                const ProcessRenderRuntime::CompletionCallbackLease callbackLease(*completionGate);
                if (callbackLease)
                    renderTerminalRelay->invoke(key, false);
            });
        }
        return;
    }

    if (!job.contentSnapshot)
    {
        AppLogger::error("ARA-DIAG: render job failed without content snapshot");
        if (job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision))
            ProcessRenderRuntime::notifyApplicationRenderFailure(
                job.contentKey, job.contentRevision, job.targetRevision,
                "Render job has no content snapshot",
                job.renderCache, completionGate);
        if (renderTerminalRelay != nullptr)
        {
            const auto key = job.contentKey;
            ProcessRenderRuntime::postAsyncCallback([completionGate, renderTerminalRelay, key] {
                const ProcessRenderRuntime::CompletionCallbackLease callbackLease(*completionGate);
                if (callbackLease)
                    renderTerminalRelay->invoke(key, false);
            });
        }
        return;
    }

    ProcessRenderRuntime::CompletionContext completion;
    completion.gate = std::move(completionGate);
    completion.applicationFailure = [gate = completion.gate](
        ContentKey key, uint64_t contentRevision, uint64_t targetRevision,
        const juce::String& reason, std::shared_ptr<RenderCache> renderCache) {
        ProcessRenderRuntime::notifyApplicationRenderFailure(
            key, contentRevision, targetRevision, reason, std::move(renderCache), gate);
    };
    const auto renderCache = job.renderCache;
    const auto contentKey = job.contentKey;
    if (renderTerminalRelay != nullptr)
    {
        completion.chunkSettled = [renderTerminalRelay, renderCache, contentKey](
            ContentKey, std::shared_ptr<const EditableContentSnapshot>,
            std::shared_ptr<const juce::AudioBuffer<float>>, double) {
            if (renderCache == nullptr)
                return;
            const auto state = renderCache->getStateSnapshot();
            if (state.canonicalSettled)
                renderTerminalRelay->invokeCompleted(contentKey, state.renderGeneration);
        };
        completion.chunkFailed = [renderTerminalRelay](ContentKey key, const juce::String&) {
            renderTerminalRelay->invoke(key, false);
        };
    }
    ProcessRenderRuntime::getInstance().processChunkRenderJob(
        std::move(contentRenderService), job,
        false, std::move(completion));
}

std::shared_ptr<const EditableContentSnapshot> OpenTuneDocumentController::snapshotAudioModification(ContentKey key) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState())
        return nullptr;

    // AudioModification adds the cached AudioSource shape to the common projection.
    return mod->snapshotContent();
}

void OpenTuneDocumentController::requestModificationRenderRanges(
    ContentKey key,
    const std::vector<NoteRangeSeconds>& affectedRanges,
    std::shared_ptr<const EditableContentSnapshot> snapshot)
{
    PlaybackReadSource readSource;
    if (contentRenderService_ == nullptr
        || !contentRenderService_->getPlaybackReadSource(key, readSource)
        || readSource.canonicalBuffer == nullptr
        || readSource.canonicalSampleRate <= 0.0)
        return;

    const int64_t totalSamples = readSource.canonicalBuffer->getNumSamples();
    std::vector<PlaybackSampleGeometry::SampleSpan> requestRanges;
    requestRanges.reserve(affectedRanges.size());
    for (const auto& range : affectedRanges)
    {
        const int64_t startSample = juce::jlimit<int64_t>(
            0, totalSamples,
            TimeCoordinate::secondsToSamplesFloor(range.startSeconds, readSource.canonicalSampleRate));
        const int64_t endSample = juce::jlimit<int64_t>(
            0, totalSamples,
            TimeCoordinate::secondsToSamplesCeil(range.endSeconds, readSource.canonicalSampleRate));
        if (endSample > startSample)
            requestRanges.push_back({startSample, endSample});
    }
    requestModificationRenderSampleRanges(key, requestRanges, std::move(snapshot));
}

void OpenTuneDocumentController::requestModificationRenderSampleRanges(
    ContentKey key,
    const std::vector<PlaybackSampleGeometry::SampleSpan>& requestRanges,
    std::shared_ptr<const EditableContentSnapshot> snapshot)
{
    if (contentRenderService_ == nullptr || requestRanges.empty())
        return;

    PlaybackReadSource readSource;
    if (!contentRenderService_->getPlaybackReadSource(key, readSource)
        || readSource.canonicalBuffer == nullptr
        || readSource.canonicalSampleRate <= 0.0)
        return;

    const int64_t totalSamples = readSource.canonicalBuffer->getNumSamples();
    std::vector<PlaybackSampleGeometry::SampleSpan> clampedRanges;
    clampedRanges.reserve(requestRanges.size());
    for (const auto& range : requestRanges)
    {
        const int64_t startSample = juce::jlimit<int64_t>(0, totalSamples, range.begin);
        const int64_t endSample = juce::jlimit<int64_t>(0, totalSamples, range.end);
        if (endSample > startSample)
            clampedRanges.push_back({startSample, endSample});
    }
    if (clampedRanges.empty())
        return;

    if (!snapshot || !contentRenderService_->republishPlaybackSource(key, snapshot))
        return;

    // The frozen plan carries the published snapshot; refresh it on publication.
    refreshRegisteredRenderers(publishModelChange());

    if (!snapshot->hasUsableOriginalF0())
        return;

    RenderJob job;
    job.contentKey = key;
    job.audioBuffer = readSource.canonicalBuffer;
    job.audioSampleRate = readSource.canonicalSampleRate;
    job.renderCache = contentRenderService_->getOrCreateRenderCache(key);
    job.contentSnapshot = std::move(snapshot);
    job.contentRevision = job.contentSnapshot->contentRevision;

    contentRenderService_->enqueueRender(std::move(job), clampedRanges);
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

    if (readSource.canonicalBuffer == nullptr || readSource.canonicalSampleRate <= 0.0)
        return;

    requestModificationRenderSampleRanges(
        key,
        std::vector<PlaybackSampleGeometry::SampleSpan>{{0, readSource.canonicalBuffer->getNumSamples()}},
        std::move(snapshot));
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

bool OpenTuneDocumentController::hostSeekAvailable()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    const ARA::ARAPlaybackControllerInterface* iface = playbackController->getInterface();
    if (iface == nullptr)
        return false;

    return playbackMemberCallable(
        iface,
        ARA_IMPLEMENTS_FIELD(iface, ARAPlaybackControllerInterface, requestSetPlaybackPosition),
        &ARA::ARAPlaybackControllerInterface::requestSetPlaybackPosition);
}

bool OpenTuneDocumentController::logicHostIsPlaying() const noexcept
{
    return logicCommitRestartsPlayback(
        juce::PluginHostType{}.isLogic(),
        sharedPlayHeadState_.isPlaying.load(std::memory_order_acquire));
}

void OpenTuneDocumentController::enqueueHostTransport(const std::function<void(HostTransportCoalescer&)>& mutate)
{
    auto relay = transportRelay_;
    if (relay == nullptr || !relay->alive.load(std::memory_order_acquire) || !mutate)
        return;

    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(relay->mutex);
        mutate(relay->coalescer);
        if (!relay->flushScheduled)
        {
            relay->flushScheduled = true;
            schedule = true;
        }
    }

    if (schedule)
        scheduleHostTransportFlush();
}

void OpenTuneDocumentController::scheduleHostTransportFlush()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto flush = [weak = std::weak_ptr<HostTransportRelayState>(transportRelay_)]()
    {
        const auto relay = weak.lock();
        if (!relay)
            return;
       #if JUCE_MAC
        if (auto timer = std::exchange(relay->pendingFlush, nullptr))
            CFRelease(timer);
       #endif
        if (!relay->alive.load(std::memory_order_acquire))
            return;

        HostTransportBatch batch;
        {
            std::lock_guard<std::mutex> lock(relay->mutex);
            batch = relay->coalescer.take();
            relay->flushScheduled = false;
        }

        if (!relay->alive.load(std::memory_order_acquire))
            return;

        relay->owner.performHostTransportBatch(batch);
    };
   #if JUCE_MAC
    deferTransportFlush(transportRelay_->pendingFlush, std::move(flush));
   #else
    ProcessRenderRuntime::postAsyncCallback(std::move(flush));
   #endif
}

void OpenTuneDocumentController::performHostTransportBatch(const HostTransportBatch& batch)
{
    if (batch.empty())
        return;

    auto* dc = getDocumentController();
    if (dc == nullptr)
        return;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return;

    const ARA::ARAPlaybackControllerInterface* iface = playbackController->getInterface();
    if (iface == nullptr)
        return;

    if (batch.seekIsCommit)
    {
        AppLogger::log("ARA-TRANSPORT: flush seek="
            + juce::String(batch.hasSeek ? "true" : "false")
            + " commit=" + juce::String(batch.seekIsCommit ? "true" : "false")
            + " restart=" + juce::String(batch.restartIfPlaying ? "true" : "false"));
    }

    for (const auto& call : planHostTransportCalls(batch))
    {
        switch (call.kind)
        {
            case HostTransportCall::Kind::Stop:
                if (playbackMemberCallable(iface,
                        ARA_IMPLEMENTS_FIELD(iface, ARAPlaybackControllerInterface, requestStopPlayback),
                        &ARA::ARAPlaybackControllerInterface::requestStopPlayback))
                    playbackController->requestStopPlayback();
                break;
            case HostTransportCall::Kind::Seek:
                if (playbackMemberCallable(iface,
                        ARA_IMPLEMENTS_FIELD(iface, ARAPlaybackControllerInterface, requestSetPlaybackPosition),
                        &ARA::ARAPlaybackControllerInterface::requestSetPlaybackPosition))
                    playbackController->requestSetPlaybackPosition(call.seconds);
                break;
            case HostTransportCall::Kind::Start:
                if (playbackMemberCallable(iface,
                        ARA_IMPLEMENTS_FIELD(iface, ARAPlaybackControllerInterface, requestStartPlayback),
                        &ARA::ARAPlaybackControllerInterface::requestStartPlayback))
                    playbackController->requestStartPlayback();
                break;
        }
    }
}

bool OpenTuneDocumentController::requestSetPlaybackPosition(double timeInSeconds)
{
    if (!hostSeekAvailable())
        return false;

    enqueueHostTransport([timeInSeconds](HostTransportCoalescer& coalescer)
    {
        coalescer.previewSeek(timeInSeconds);
    });
    return true;
}

bool OpenTuneDocumentController::requestCommitPlaybackPosition(double timeInSeconds)
{
    if (!hostSeekAvailable())
        return false;

    const bool restart = logicHostIsPlaying();

    enqueueHostTransport([timeInSeconds, restart](HostTransportCoalescer& coalescer)
    {
        coalescer.commitSeek(timeInSeconds, restart);
    });
    return true;
}

// -----------------------------------------------------------------------
// 编辑器只读内容访问器实现
// -----------------------------------------------------------------------

std::shared_ptr<const juce::AudioBuffer<float>> OpenTuneDocumentController::readAudioBuffer(ContentKey key) const
{
    PlaybackReadSource crsSrc;
    if (contentRenderService_ != nullptr && contentRenderService_->getPlaybackReadSource(key, crsSrc) && crsSrc.canonicalBuffer != nullptr)
        return crsSrc.canonicalBuffer;
    return nullptr;
}

std::shared_ptr<const PitchCurveSnapshot> OpenTuneDocumentController::readPitchCurve(ContentKey key) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return nullptr;
    return mod->content->analysis.pitchCurve != nullptr
        ? mod->content->analysis.pitchCurve->getSnapshot()
        : nullptr;
}

OriginalF0State OpenTuneDocumentController::readOriginalF0State(ContentKey key) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return OriginalF0State::NotRequested;
    return mod->content->analysis.originalF0State;
}


DetectedKey OpenTuneDocumentController::readDetectedKey(ContentKey key) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return {};
    return mod->content->analysis.detectedKey;
}

PitchShiftSettings OpenTuneDocumentController::readPitchShift(ContentKey key) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return {};
    return mod->content->pitchShiftSettings;
}

std::shared_ptr<const EditableContentSnapshot>
OpenTuneDocumentController::readContentSnapshot(ContentKey key) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return nullptr;
    return mod->snapshotContent();
}

// ARA mutation API implementations — Processor delegates ARA writes here

bool OpenTuneDocumentController::applyNotesToModification(const ContentKey& key, std::vector<Note> notes)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyNotes(std::move(notes));
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected()
                + juce::ARAContentUpdateScopes::notesAreAffected(),
            true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyVolumeEnvelopeToModification(const ContentKey& key,
                                                                   AutomationLane envelope)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyVolumeEnvelope(envelope);

    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected(), true);

    refreshRegisteredRenderers(publishModelChange());
    return true;
}

void OpenTuneDocumentController::republishPlaybackSourceForModification(ContentKey key)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState())
        return;
    if (contentRenderService_ == nullptr)
        return;

    contentRenderService_->republishPlaybackSource(key, mod->snapshotContent());
    refreshRegisteredRenderers(publishModelChange());
}

std::string OpenTuneDocumentController::readOriginalF0FailureReason(ContentKey key) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState())
        return {};
    return mod->content->analysis.originalF0FailureReason;
}

std::deque<OriginalF0FailureEvent>
OpenTuneDocumentController::takeOriginalF0FailureEvents()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    std::deque<OriginalF0FailureEvent> events;
    events.swap(pendingF0FailureEvents_);
    return events;
}

bool OpenTuneDocumentController::applyPitchCurveToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyPitchCurve(std::move(curve));
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected()
                + juce::ARAContentUpdateScopes::tuningIsAffected(),
            true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyNotesAndPitchCurveToModification(
    const ContentKey& key,
    std::vector<Note> notes,
    std::shared_ptr<PitchCurve> curve)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState() || curve == nullptr)
        return false;

    mod->applyNotes(std::move(notes));
    mod->applyPitchCurve(std::move(curve));

    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected()
                + juce::ARAContentUpdateScopes::notesAreAffected()
                + juce::ARAContentUpdateScopes::tuningIsAffected(),
            true);

    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyOriginalF0ToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyOriginalF0(std::move(curve));

    if (getDocumentController()->isHostEditingDocument() || !initializePendingNoteTopology(*mod))
        requestFullModificationRender(key);
    mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes::tuningIsAffected(), true);
    presentationChanges_.sendChangeMessage();
    return true;
}

bool OpenTuneDocumentController::applyTimeGridToModification(const ContentKey& key, std::shared_ptr<const TimeGridSnapshot> grid)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState() || grid == nullptr || !grid->isIdentity()) return false;
    if (!mod->applyTimeGrid(std::move(grid)))
        return false;

    auto snapshot = mod->snapshotContent();
    contentRenderService_->republishPlaybackSource(key, snapshot);
    refreshRegisteredRenderers(publishModelChange());

    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::timelineIsAffected(), true);

    return true;
}

bool OpenTuneDocumentController::applyPitchShiftStateToModification(const ContentKey& key,
                                                                     const PitchShiftEditState& state)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    if (!mod->applyPitchShiftState(state)) return false;
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected()
                + juce::ARAContentUpdateScopes::notesAreAffected()
                + juce::ARAContentUpdateScopes::tuningIsAffected(),
            true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyDetectedKeyToModification(const ContentKey& key, const DetectedKey& detectedKey)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyDetectedKey(detectedKey);
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::harmoniesAreAffected(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyReferenceFeaturesToModification(const ContentKey& key, const ReferenceFeatureSet& features)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyReferenceFeatures(features);
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::notesAreAffected()
                + juce::ARAContentUpdateScopes::timelineIsAffected(),
            true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyOriginalF0StateToModification(const ContentKey& key, const OriginalF0State& state)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyOriginalF0State(state);
    
    // Notify ARA host of content change for cache/save state invalidation
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::tuningIsAffected(), true);
    
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::applyOriginalF0FailureReasonToModification(
    const ContentKey& key, std::string reason)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr || !mod->hasContentState()) return false;
    mod->applyOriginalF0FailureReason(std::move(reason));
    pendingF0FailureEvents_.push_back({key,
        juce::String(mod->content->analysis.originalF0FailureReason)});
    if (mod->audioModification != nullptr)
        mod->audioModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::tuningIsAffected(), true);
    refreshRegisteredRenderers(publishModelChange());
    return true;
}

} // namespace OpenTune

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<OpenTune::OpenTuneDocumentController>();
}

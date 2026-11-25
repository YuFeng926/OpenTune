#include "StandaloneClipContent.h"
#include "ContentSnapshotProjection.h"
#include <algorithm>
#include <utility>

namespace OpenTune {

StandaloneClipContent::StandaloneClipContent(StandaloneClipId clipId)
    : clipId_(clipId)
{
}

ContentKey StandaloneClipContent::contentKey() const
{
    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = clipId_;
    return key;
}

std::shared_ptr<const EditableContentSnapshot> StandaloneClipContent::snapshotContent() const
{
    // 冷快照发布的互斥边界：仓库读锁允许多读者共存，惰性缓存的 token/指针
    // 写入由本锁串行化。提交路径（apply*）与快照读不取本锁、无读写并发保证
    // （消息线程单线程所有权）；本锁唯一保证的是双冷读者的发布串行化。
    std::lock_guard<std::mutex> guard(snapshotMutex_);
    if (cachedSnapshot_ != nullptr && cachedSnapshotToken_ == snapshotToken_)
        return cachedSnapshot_;

    auto snapshot = std::make_shared<const EditableContentSnapshot>(makeContentSnapshot(content_));
    cachedSnapshotToken_ = snapshotToken_;
    cachedSnapshot_ = std::move(snapshot);
    return cachedSnapshot_;
}

// ── Lifecycle ───────────────────────────────────────────────

void StandaloneClipContent::retireContent(ContentKey key)
{
    if (key != contentKey() || lifecycle_ == ContentLifecycle::Retired)
        return;

    StandaloneRetiredContentRecord record;
    record.key = key;
    record.content = std::move(content_);
    lifecycle_ = ContentLifecycle::Retired;

    // 重置当前 content 为空状态
    content_ = ContentState{};

    retired_.push_back(std::move(record));
    bumpSnapshotToken();
}

void StandaloneClipContent::reviveContent(ContentKey key)
{
    if (key != contentKey())
        return;

    for (auto it = retired_.begin(); it != retired_.end(); ++it) {
        if (it->key == key) {
            content_ = std::move(it->content);
            lifecycle_ = ContentLifecycle::Ready;
            bumpContentRevision();
            bumpSnapshotToken();
            retired_.erase(it);
            return;
        }
    }
}

void StandaloneClipContent::releaseRetiredContent(ContentKey key)
{
    if (key != contentKey())
        return;

    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
            [&key](const StandaloneRetiredContentRecord& r) { return r.key == key; }),
        retired_.end());
}

const StandaloneRetiredContentRecord* StandaloneClipContent::getRetiredContent(ContentKey key) const
{
    const auto it = std::find_if(retired_.begin(), retired_.end(),
        [&key](const StandaloneRetiredContentRecord& record) { return record.key == key; });
    return it != retired_.end() ? &*it : nullptr;
}

bool StandaloneClipContent::isRetired() const
{
    return lifecycle_ == ContentLifecycle::Retired;
}

bool StandaloneClipContent::hasActiveContent() const
{
    return lifecycle_ != ContentLifecycle::Retired;
}

// ── Apply commands ──────────────────────────────────────────

void StandaloneClipContent::initializeContent(ContentState payload)
{
    // TimeGrid 由各 owner 入口（import/split/merge/copy/restore）按权威
    // source window duration 显式初始化；这里只提交完整 payload，不再补造或改写。
    content_ = std::move(payload);
    bumpSnapshotToken();
}

void StandaloneClipContent::applyNotes(std::vector<Note> notes, bool topologyInitialized)
{
    content_.notes = std::move(notes);
    for (auto& note : content_.notes)
        note.outputGainDb = content_.volumeEnvelope.evalAt(note.startTime);
    content_.noteTopologyInitialized = topologyInitialized;
    ++content_.notesRevision;
    bumpContentRevision();
    bumpSnapshotToken();
}

void StandaloneClipContent::applyImportedSource(SourceWindow window, NativePcmSource nativeSource,
                                                int64_t firstCanonicalSample)
{
    content_.sourceWindow = std::move(window);
    content_.nativeSource = std::move(nativeSource);
    content_.firstCanonicalSample = firstCanonicalSample;
    bumpSnapshotToken();
}

void StandaloneClipContent::applyVolumeEnvelope(AutomationLane envelope)
{
    content_.volumeEnvelope = std::move(envelope);
    for (auto& note : content_.notes)
        note.outputGainDb = content_.volumeEnvelope.evalAt(note.startTime);
    ++content_.notesRevision;
    bumpSnapshotToken();
}

void StandaloneClipContent::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    // 发布边界转交独立对象：owner 内容里的曲线与调用方保留的句柄分属两个
    // PitchCurve 对象（共享同一不可变快照），提交后经旧句柄改写不影响 owner。
    content_.analysis.pitchCurve = curve ? curve->deriveCopy() : nullptr;
    ++content_.pitchRevision;
    bumpContentRevision();
    bumpSnapshotToken();
}

void StandaloneClipContent::applyOriginalF0(std::shared_ptr<PitchCurve> curve)
{
    // 一次性提交：curve + Ready 状态 + 单次 analysis/content revision。
    // 不调用 applyOriginalF0State，避免同一次提交双 bump。
    // 发布边界转交独立对象（同 applyPitchCurve）。
    content_.analysis.pitchCurve = curve ? curve->deriveCopy() : nullptr;
    content_.analysis.setOriginalF0State(OriginalF0State::Ready);
    ++content_.analysis.analysisRevision;
    ++content_.pitchRevision;
    bumpContentRevision();
    bumpSnapshotToken();
}

void StandaloneClipContent::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot)
{
    if (snapshot == nullptr)
        return;
    content_.timeGrid = std::move(snapshot);
    ++content_.timeGridRevision;
    bumpSnapshotToken();
}

bool StandaloneClipContent::applyPitchShiftState(const PitchShiftEditState& state)
{
    if (content_.analysis.pitchCurve == nullptr)
        return false;

    // 曲线不可变别名：修正段写入以新曲线对象整体提交（写时复制），
    // 公开内容里不存在可就地改写的可变曲线。
    auto next = content_.analysis.pitchCurve->deriveCopy();
    next->replaceCorrectionSegments(state.segments);
    content_.notes = state.notes;
    content_.analysis.pitchCurve = std::move(next);
    content_.pitchShiftSettings = state.settings;
    ++content_.notesRevision;
    ++content_.pitchRevision;
    bumpContentRevision();
    bumpSnapshotToken();
    return true;
}

void StandaloneClipContent::applyDetectedKey(const DetectedKey& key)
{
    content_.analysis.detectedKey = key;
    bumpSnapshotToken();
}

void StandaloneClipContent::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    content_.analysis.referenceFeatures = features;
    bumpSnapshotToken();
}

void StandaloneClipContent::applyOriginalF0State(OriginalF0State state)
{
    // 幂等 setter：状态未变且失败原因无需清理时不推进快照令牌，
    // 与 Capture/ARA owner 的 revision 语义一致。
    const bool reasonCleared = state != OriginalF0State::Failed
        && !content_.analysis.originalF0FailureReason.empty();
    if (!content_.analysis.setOriginalF0State(state) && !reasonCleared)
        return;
    bumpSnapshotToken();
}

void StandaloneClipContent::applyOriginalF0FailureReason(std::string reason)
{
    content_.analysis.originalF0FailureReason = std::move(reason);
    bumpSnapshotToken();
}

void StandaloneClipContent::applyPitchShiftSettings(PitchShiftSettings settings)
{
    content_.pitchShiftSettings = std::move(settings);
    bumpSnapshotToken();
}

void StandaloneClipContent::applySilentGaps(std::vector<SilentGap> gaps)
{
    content_.analysis.silentGaps = std::move(gaps);
    bumpSnapshotToken();
}

void StandaloneClipContent::applyAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, double sampleRate)
{
    // TimeGrid 由 owner 按权威 source window duration 初始化；派生 PCM 长度只作存储边界。
    const double durationSeconds = content_.sourceWindow.durationSeconds();

    content_.audioBuffer = std::move(buffer);
    content_.sampleRate = sampleRate;
    ++content_.audioRevision;
    // 非正/非有限 duration 生成 identity 失败时保留已有 bootstrap identity，不写 nullptr。
    if (auto realGrid = TimeGridSnapshot::makeIdentity(durationSeconds))
    {
        content_.timeGrid = std::move(realGrid);
        ++content_.timeGridRevision;
    }
    bumpContentRevision();
    bumpSnapshotToken();
}

// ── Private helpers ─────────────────────────────────────────

void StandaloneClipContent::bumpContentRevision()
{
    ++content_.contentRevision;
}

} // namespace OpenTune

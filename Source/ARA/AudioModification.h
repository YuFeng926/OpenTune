#pragma once
#include "AudioSource.h"
#include "../Utils/SourceWindow.h"
#include "../Content/ContentKey.h"
#include "../Content/AudioModificationContentState.h"
#include "../Content/EditableContentSnapshot.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

enum class AudioModificationBirthState
{
    Empty,
    WaitingForSource,
    PendingBirth,
    Rendering,
    Ready,
    Failed
};

struct AudioModification
{
    juce::ARAAudioModification* audioModification{nullptr};
    juce::String persistentId;
    // Per ARA2 spec: AudioSource identity binding is part of content.sourceWindow,
    // not wrapper-level field. Single source of truth for ARA source association.
    ContentKey contentIdentity;
    uint64_t birthRevision{0};
    AudioModificationBirthState birthState{AudioModificationBirthState::Empty};

    // 内容所有权
    AudioModificationContentState content;

    // 身份更新
    void updateIdentity(juce::ARAAudioModification* modification);
    void attachSource(const AudioSource& source);
    void resetContent() noexcept;
    bool isRenderable() const noexcept;

    // 内容辅助
    bool hasContentState() const noexcept { return content.lifecycle > ContentLifecycle::Empty; }
    std::shared_ptr<const EditableContentSnapshot> snapshotContent() const;

    // 内容生命周期
    ContentKey contentKey() const noexcept;

private:
    AraSourceShape cachedSourceShape_;  // 从 AudioSource 缓存（ARA2 委托）

    // 编辑入口：应用命令并推高 revision
public:
    void applyNotes(const std::vector<Note>& notes);
    void applyPitchCurve(std::shared_ptr<PitchCurve> curve);
    void applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> grid);
    void applyPitchShift(const PitchShiftSettings& settings);
    void applyDetectedKey(const DetectedKey& key);
    void applyF0Analysis(std::shared_ptr<PitchCurve> curve);
    void applyOriginalF0State(OriginalF0State state);
    void applyReferenceFeatures(const ReferenceFeatureSet& features);
};

} // namespace OpenTune

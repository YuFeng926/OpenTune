#pragma once
#include "AudioSource.h"
#include "../Utils/SourceWindow.h"
#include "../Content/ContentKey.h"
#include "../Content/AudioModificationContentState.h"
#include "../Content/EditableContentSnapshot.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace OpenTune {

enum class AudioModificationReadIntent
{
    None,
    UserRead
};

enum class AudioModificationBirthState
{
    Empty,
    WaitingForSource,
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
    AudioModificationReadIntent readIntent{AudioModificationReadIntent::None};

    // 内容所有权
    std::optional<AudioModificationContentState> content;

    // 身份更新
    void updateIdentity(juce::ARAAudioModification* modification);
    bool attachSource(const AudioSource& source);
    void resetContent() noexcept;
    void invalidateDerivedContent() noexcept;
    bool isRenderable() const noexcept;

    // 内容辅助
    bool hasContentState() const noexcept { return content.has_value(); }
    std::shared_ptr<const EditableContentSnapshot> snapshotContent() const;

    // 内容生命周期
    ContentKey contentKey() const noexcept;

private:
    AraSourceShape cachedSourceShape_;  // 从 AudioSource 缓存（ARA2 委托）

public:
    // 编辑入口：应用命令并推高 revision
    void applyNotes(const std::vector<Note>& notes);
    void applyPitchCurve(std::shared_ptr<PitchCurve> curve);
    bool applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> grid);
    void applyPitchShift(const PitchShiftSettings& settings);
    void applyDetectedKey(const DetectedKey& key);
    void applyOriginalF0(std::shared_ptr<PitchCurve> curve);
    void submitSilentGaps(std::vector<SilentGap> gaps);
    void applyOriginalF0State(OriginalF0State state);
    void applyReferenceFeatures(const ReferenceFeatureSet& features);
};

} // namespace OpenTune

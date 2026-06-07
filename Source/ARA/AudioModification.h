#pragma once
#include "AudioSource.h"
#include "../Utils/SourceWindow.h"
#include "../Content/ContentKey.h"
#include "../Content/AudioModificationContentState.h"
#include "../Content/RetiredContentRecord.h"
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
    juce::String sourcePersistentId;
    SourceWindow contentWindow;
    uint64_t sourceId{0};
    uint64_t contentRevision{0};
    uint64_t birthRevision{0};
    double materializationDurationSeconds{0.0};
    AudioModificationBirthState birthState{AudioModificationBirthState::Empty};

    // 内容所有权
    AudioModificationContentState content;
    std::vector<RetiredContentRecord> retiredContentRecords;

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
    void retireCurrentContent();
    bool reviveContent(ContentKey key);
    void releaseRetiredContent(ContentKey key);

    // 编辑入口：应用命令并推高 revision
    void applyNotes(const std::vector<Note>& notes);
    void applyPitchCurve(std::shared_ptr<PitchCurve> curve);
    void applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> grid);
    void applyPitchShift(const PitchShiftSettings& settings);
    void applyDetectedKey(const DetectedKey& key);
    void applyF0Analysis(std::shared_ptr<PitchCurve> curve);
};

} // namespace OpenTune

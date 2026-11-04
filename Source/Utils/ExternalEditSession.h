#pragma once

#include "ExternalEditLaunch.h"
#include "AutomationLane.h"
#include "Note.h"
#include "PitchCurve.h"
#include "PitchShiftSettings.h"
#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <optional>

namespace OpenTune {

// One plugin clip handed to the standalone editor. The audio file is only
// the audition copy. A returned session carries notes and the pitch curve
// those notes produced; the plugin renders them on its own audio.
struct ExternalEditDocument
{
    ContentKey sourceKey;
    std::vector<Note> notes;
    std::shared_ptr<PitchCurve> pitchCurve;
    AutomationLane envelope;
    PitchShiftSettings pitchShift;
    bool returned = false;
};

// 曲线读取三态：Pending = 会话未返回/未就绪；Ok = 返回且曲线可读（或无曲线）；
// CorruptCurve = edit.xml 已返回但 curve.bin 存在却无法解析（可在外部修复后重试）。
enum class ExternalCurveCheckResult
{
    Pending,
    Ok,
    CorruptCurve
};

struct ExternalEditCheckResult
{
    ExternalCurveCheckResult result = ExternalCurveCheckResult::Pending;
    // 仅当 result == Ok 时有效。
    ExternalEditDocument document;
};

class ExternalEditSession
{
public:
    static juce::File createSessionDirectory();
    static bool writeSession(const juce::File& directory,
                             const ContentKey& sourceKey,
                             const EditableContentSnapshot& snapshot);
    static bool writeReturn(const juce::File& directory,
                            const EditableContentSnapshot& snapshot);
    static std::optional<ExternalEditDocument> readDocument(const juce::File& directory);
    // 单线程（消息线程）调用的返回检查：区分 Pending / Ok / CorruptCurve，
    // 不改变会话目录内容，失败会话保持打开以便外部修复后重试。
    static ExternalEditCheckResult checkReturned(const juce::File& directory);

    static juce::File audioFile(const juce::File& directory);
    static juce::File findStandaloneExecutable();
    static bool launchStandalone(const juce::File& directory);
    static juce::File sessionDirectoryFromCommandLine();
};

} // namespace OpenTune

#include "Utils/ExternalEditSession.h"

#include <cstdio>
#include <vector>

// ExternalEditSession 三态检查契约：
//   Pending   — 无 edit.xml / 未标记 returned / curve.bin 缺失以外的失败；
//   CorruptCurve — edit.xml 已返回但 curve.bin 存在且损坏（可修复后重试）；
//   Ok        — 返回且曲线可读，读取结果与写入内容一致。
//
// 纯文件契约测试：不实例化 Processor，不依赖消息线程。

namespace {

using OpenTune::ContentKey;
using OpenTune::DomainKind;
using OpenTune::ExternalCurveCheckResult;
using OpenTune::ExternalEditSession;

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

std::shared_ptr<OpenTune::PitchCurveSnapshot> makeCurve()
{
    // 合法最小曲线：2 帧 F0/energy，无修正段。
    std::vector<float> f0{110.0f, 220.0f};
    std::vector<float> energy{0.5f, 0.6f};
    return std::make_shared<OpenTune::PitchCurveSnapshot>(
        f0, energy, std::vector<OpenTune::PitchCorrectionSegment>{}, 512, 44100.0);
}

OpenTune::EditableContentSnapshot makeSnapshot(std::shared_ptr<OpenTune::PitchCurveSnapshot> curve)
{
    OpenTune::EditableContentSnapshot snapshot;
    auto audio = std::make_shared<const juce::AudioBuffer<float>>(1, 512);
    snapshot.audioBuffer = audio;
    snapshot.audioSampleRate = 44100.0;
    snapshot.pitchCurve = std::move(curve);
    return snapshot;
}

juce::File makeTempDirectory()
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("OpenTune")
        .getChildFile("ExternalEditSessionTests")
        .getChildFile(juce::Uuid().toDashedString());
}

ContentKey testKey()
{
    ContentKey key;
    key.domainKind = DomainKind::RegularVST3Capture;
    key.objectId = 42;
    return key;
}

void testPendingWithoutReturn()
{
    const auto directory = makeTempDirectory();
    const auto snapshot = makeSnapshot(makeCurve());
    check(ExternalEditSession::writeSession(directory, testKey(), snapshot),
          "session written");

    const auto checkResult = ExternalEditSession::checkReturned(directory);
    check(checkResult.result == ExternalCurveCheckResult::Pending,
          "unreturned session is Pending");
    directory.deleteRecursively();
}

void testOkResultRoundTrip()
{
    const auto directory = makeTempDirectory();
    const auto snapshot = makeSnapshot(makeCurve());
    check(ExternalEditSession::writeSession(directory, testKey(), snapshot),
          "session written");

    const auto returned = makeSnapshot(makeCurve());
    check(ExternalEditSession::writeReturn(directory, returned),
          "return written");

    const auto checkResult = ExternalEditSession::checkReturned(directory);
    check(checkResult.result == ExternalCurveCheckResult::Ok,
          "returned session with valid curve is Ok");
    check(checkResult.document.returned, "document is marked returned");
    check(checkResult.document.sourceKey == testKey(), "source key preserved");
    check(checkResult.document.pitchCurve != nullptr, "curve present");
    if (checkResult.document.pitchCurve != nullptr) {
        check(checkResult.document.pitchCurve->getSnapshot()->getOriginalF0().size() == 2,
              "curve F0 frame count round-trips");
        check(checkResult.document.pitchCurve->getSnapshot()->getHopSize() == 512,
              "curve hop size round-trips");
    }
    directory.deleteRecursively();
}

void testCorruptCurveDetected()
{
    const auto directory = makeTempDirectory();
    const auto snapshot = makeSnapshot(makeCurve());
    check(ExternalEditSession::writeSession(directory, testKey(), snapshot),
          "session written");

    const auto returned = makeSnapshot(makeCurve());
    check(ExternalEditSession::writeReturn(directory, returned),
          "return written");

    // 模拟损坏：truncate curve.bin 到非法大小（保留 OTC1 magic 以外的随机字节）。
    const auto curve = directory.getChildFile("curve.bin");
    check(curve.existsAsFile(), "curve.bin exists before corruption");
    check(curve.replaceWithText(juce::String("OTC1broken")),
          "curve.bin corrupted");

    const auto checkResult = ExternalEditSession::checkReturned(directory);
    check(checkResult.result == ExternalCurveCheckResult::CorruptCurve,
          "corrupt returned curve is reported as CorruptCurve");

    // 会话目录保持原样：修复曲线后可重试。
    check(ExternalEditSession::audioFile(directory).existsAsFile(),
          "session directory preserved for retry");
    directory.deleteRecursively();
}

void testMalformedReturnedXmlIsPending()
{
    const auto directory = makeTempDirectory();
    const auto snapshot = makeSnapshot(makeCurve());
    check(ExternalEditSession::writeSession(directory, testKey(), snapshot),
          "session written");
    check(ExternalEditSession::writeReturn(directory, snapshot),
          "return written");

    check(directory.getChildFile("edit.xml").replaceWithText(
              "<<<"),
          "returned XML corrupted");

    const auto checkResult = ExternalEditSession::checkReturned(directory);
    check(checkResult.result == ExternalCurveCheckResult::Pending,
          "malformed returned XML is not reported as a corrupt curve");
    directory.deleteRecursively();
}

} // namespace

int main()
{
    testPendingWithoutReturn();
    testOkResultRoundTrip();
    testCorruptCurveDetected();
    testMalformedReturnedXmlIsPending();

    if (failures == 0)
    {
        std::puts("ExternalEditSessionTests: all checks passed.");
        return 0;
    }

    std::fprintf(stderr, "ExternalEditSessionTests: %d check(s) failed.\n", failures);
    return 1;
}

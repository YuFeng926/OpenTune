// F0 绘制缓存契约：
// 1. 缓存只替换 X 源（全曲线 timeline 秒投影）；段发射/合并/长度判定语义由直算
//    原语按当前窗口执行。缓存 X 源与直算 X 复合链逐位一致 → 输出逐点一致。
// 2. 阻断 4 反例回归：短数据修正段（声明 [48,60) 仅 4 值）的发射判定依赖窗口
//    端点——窗口 [48,52) 发射修正值、窗口 [48,60) 整段跳过（OpenTune），
//    OpenDyne 在跳过区回落 original×gapRatio。两窗口 × 两 scheme 的缓存路径
//    与直算路径逐点一致。
// 3. 缓存键 = 强持有的不可变内容对象（PitchCurveSnapshot / TimeGridSnapshot）
//    + gapRatio + placement 投影 4 double + scheme：插件路径每次读取新建外层
//    快照，内层对象身份稳定 → 命中。
// 4. 拖拽预览（owner ≠ committed）与空曲线（导入待分析）不缓存返回空。

#include "../Source/Standalone/UI/PianoRoll/F0VisualCache.h"
#include "../Source/Standalone/UI/PianoRoll/F0VisualSegments.h"
#include "../Source/Standalone/UI/PianoRoll/PianoRollTimeMap.h"
#include "../Source/Standalone/UI/ViewMapper.h"
#include "../Source/Content/ContentSnapshotProjection.h"
#include "../Source/Utils/TimeGrid.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace OpenTune;

static int failures = 0;

static void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

namespace {

// 96 帧 × 128 hop @ 16kHz = 0.768s；覆盖有声/静音/边界值/NaN 能量/越界频率，
// 以及「声明跨度 > 数据长度」的守卫修正段（发射判定依赖请求窗口端点）。
constexpr int kFrames = 96;
constexpr double kFrameSeconds = 128.0 / 16000.0;
constexpr double kContentSeconds = kFrames * kFrameSeconds;

std::shared_ptr<PitchCurve> makeTestCurve()
{
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(128);
    curve->setSampleRate(16000.0);

    std::vector<float> f0(static_cast<std::size_t>(kFrames));
    for (int f = 0; f < kFrames; ++f)
    {
        if (f == 0)            f0[static_cast<std::size_t>(f)] = 0.0f;       // 静音
        else if (f == 1)       f0[static_cast<std::size_t>(f)] = 19.9f;      // 低于下界
        else if (f == 2)       f0[static_cast<std::size_t>(f)] = 20.0f;      // 下界（有声，含）
        else if (f == 3)       f0[static_cast<std::size_t>(f)] = 2000.0f;    // 上界（有声，含）
        else if (f == 4)       f0[static_cast<std::size_t>(f)] = 2000.1f;    // 高于上界
        else if (f == 5)       f0[static_cast<std::size_t>(f)] = 0.0f;
        else if (f % 7 == 6)   f0[static_cast<std::size_t>(f)] = 0.0f;       // 周期性静音
        else                   f0[static_cast<std::size_t>(f)] = 120.0f + 30.0f * static_cast<float>(f % 11);
    }
    curve->setOriginalF0(f0);

    std::vector<float> energy(static_cast<std::size_t>(kFrames));
    for (int f = 0; f < kFrames; ++f)
        energy[static_cast<std::size_t>(f)] = (f == 17)
            ? std::numeric_limits<float>::quiet_NaN()
            : 0.1f + 0.9f * static_cast<float>((f * 13) % 19) / 18.0f;
    curve->setOriginalEnergy(energy);

    // 修正段：正常段、跨静音段、越界值段、守卫段（声明 [48,60) 仅 4 值）。
    curve->replaceCorrectionSegments({
        PitchCorrectionSegment{8, 24, {220.0f, 230.0f, 240.0f, 250.0f, 260.0f, 270.0f,
                                       280.0f, 290.0f, 300.0f, 310.0f, 320.0f, 330.0f,
                                       340.0f, 350.0f, 360.0f, 1999.0f}},
        PitchCorrectionSegment{30, 38, {660.0f, 0.0f, 680.0f, 0.0f, 690.0f, 700.0f,
                                        710.0f, 720.0f}},
        PitchCorrectionSegment{48, 60, {900.0f, 910.0f, 920.0f, 930.0f}},  // 守卫段
        PitchCorrectionSegment{64, 72, {1100.0f, 1120.0f, 1140.0f, 1160.0f,
                                        1180.0f, 1200.0f, 1220.0f, 1240.0f}},
    });
    return curve;
}

struct Fixture
{
    ContentState state;
    std::shared_ptr<const EditableContentSnapshot> owner;
    ContentTimelineProjection projection;
};

Fixture makeFixture()
{
    Fixture fx;
    fx.state.analysis.pitchCurve = makeTestCurve();
    fx.state.analysis.originalF0State = OriginalF0State::Ready;
    fx.state.pitchShiftSettings = PitchShiftSettings{2, 0};  // gapRatio = 2^(2/12) ≈ 1.12246
    fx.state.timeGrid = TimeGridSnapshot::makeFromHandles(std::vector<TimeHandle>{
        {1, 0.0, 0.0, HandleKind::ClipStart, Confidence::Default},
        {2, 0.4, 0.3, HandleKind::UserAdded, Confidence::Default},
        {3, 0.768, 0.768, HandleKind::ClipEnd, Confidence::Default}});
    fx.state.sourceWindow = SourceWindow{31, {}, 0.0, 0.768};
    fx.state.notes.push_back({});
    fx.state.notes.front().startTime = 0.064;
    fx.state.notes.front().endTime = 0.7;

    fx.owner = std::make_shared<const EditableContentSnapshot>(makeContentSnapshot(fx.state));
    fx.projection = ContentTimelineProjection{10.0, kContentSeconds, 0.0, kContentSeconds};
    return fx;
}

struct Viewport { double visibleStart; double pixelsPerSecond; int contentStartX; int width; };

// 与 drawF0Curve 相同口径的可见帧窗口：视口 [contentStartX, contentStartX+width]
// 外扩 10 帧，clamp 到 [0, frames)。X 对帧单调不减（pps > 0），线性扫描与二分同结果。
void visibleWindow(const PianoRollTimeMap& map, ViewMapper coords,
                   int frames, int& startFrame, int& endFrameExclusive)
{
    const auto frameX = [&](int frame) {
        return map.sourceToXExact(static_cast<double>(frame) * kFrameSeconds);
    };
    const double left = static_cast<double>(coords.contentStartX);
    const double right = static_cast<double>(coords.contentStartX + coords.contentWidth);
    int firstVisible = frames;
    int pastVisible = frames;
    for (int f = 0; f < frames; ++f)
    {
        if (frameX(f) >= left) { firstVisible = f; break; }
    }
    for (int f = firstVisible; f < frames; ++f)
    {
        if (frameX(f) > right) { pastVisible = f; break; }
    }
    startFrame = std::max(0, firstVisible - 10);
    endFrameExclusive = std::min(frames, pastVisible + 10);
}

bool sameSegments(const std::vector<F0VisualSegment>& direct,
                  const std::vector<F0VisualSegment>& cached)
{
    // 逐位相等（NaN 的 hotMix 两路径传播同一 NaN 位型；位型相同即像素相同）。
    auto bitsEqual = [](float a, float b) {
        std::uint32_t ia = 0;
        std::uint32_t ib = 0;
        std::memcpy(&ia, &a, sizeof(float));
        std::memcpy(&ib, &b, sizeof(float));
        return ia == ib;
    };
    if (direct.size() != cached.size())
    {
        std::fprintf(stderr, "  segment count mismatch: direct=%zu cached=%zu\n",
                     direct.size(), cached.size());
        return false;
    }
    for (std::size_t s = 0; s < direct.size(); ++s)
    {
        if (direct[s].pathFirst != cached[s].pathFirst
            || direct[s].pathLast != cached[s].pathLast)
        {
            std::fprintf(stderr, "  seg %zu path range: direct [%d,%d] cached [%d,%d]\n", s,
                         direct[s].pathFirst, direct[s].pathLast,
                         cached[s].pathFirst, cached[s].pathLast);
            return false;
        }
        if (direct[s].points.size() != cached[s].points.size())
        {
            std::fprintf(stderr, "  seg %zu point count: direct=%zu cached=%zu\n", s,
                         direct[s].points.size(), cached[s].points.size());
            return false;
        }
        for (std::size_t p = 0; p < direct[s].points.size(); ++p)
        {
            const auto& a = direct[s].points[p];
            const auto& b = cached[s].points[p];
            if (!bitsEqual(a.x, b.x) || !bitsEqual(a.y, b.y)
                || !bitsEqual(a.levelHotMix, b.levelHotMix)
                || a.noteColour.getARGB() != b.noteColour.getARGB())
            {
                std::fprintf(stderr,
                    "  seg %zu pt %zu: x %.9g/%.9g y %.9g/%.9g hm %.9g/%.9g col %08x/%08x\n",
                    s, p, a.x, b.x, a.y, b.y, a.levelHotMix, b.levelHotMix,
                    a.noteColour.getARGB(), b.noteColour.getARGB());
                return false;
            }
        }
    }
    return true;
}

// 与 drawF0Curve 逐条对应的两路构建；cache 非空时 makeFrameToX 换用缓存 X 源
// （生产路径的缓存分支），发射原语（原始单段 / forEachCorrectionF0Span /
// forEachEffectiveF0Span）与直算完全相同。withOriginal=false 时只跑 corrected
// pass（守卫段发射断言的口径）。
std::vector<F0VisualSegment> buildSegments(bool notesPrimaryScheme,
                                           const EditableContentSnapshot& owner,
                                           const ContentTimelineProjection& projection,
                                           ViewMapper coords,
                                           int startFrame, int endFrameExclusive,
                                           const F0VisualCacheEntry* cache,
                                           bool withOriginal = true)
{
    const auto& pitchSnapshot = *owner.pitchCurve;
    const F0Timeline timeline(pitchSnapshot.getHopSize(), pitchSnapshot.getSampleRate(),
                              static_cast<int>(pitchSnapshot.size()));
    const PianoRollTimeMap map(projection, *owner.timeGrid, coords);
    const auto makeFrameToX = [&](int frame) -> float {
        if (cache != nullptr)
            return static_cast<float>(coords.timeToXExact(cache->timelineSeconds[static_cast<std::size_t>(frame)]));
        return static_cast<float>(map.sourceToXExact(timeline.timeAtFrame(frame)));
    };
    const auto makeFrameToY = [&](int, float frequency) -> float {
        return coords.freqToY(frequency);
    };
    const auto colourAt = [&](int frame) {
        return juce::Colour(0xFF000000u | static_cast<uint32_t>(0x40u + (frame % 7) * 0x11u));
    };
    const float bandX0 = -std::numeric_limits<float>::max();
    const float bandX1 = std::numeric_limits<float>::max();
    const std::vector<float>* originalEnergy = &pitchSnapshot.getOriginalEnergy();
    if (originalEnergy->empty()) originalEnergy = nullptr;

    std::vector<F0VisualSegment> segments;
    if (withOriginal && !notesPrimaryScheme)
    {
        const auto& originalF0 = pitchSnapshot.getOriginalF0();
        const int origStart = startFrame;
        const int origEnd = std::min(endFrameExclusive, static_cast<int>(originalF0.size()));
        auto originalProducer = [&](auto&& sink) {
            if (origStart < origEnd)
                sink(origStart, originalF0.data() + origStart, origEnd - origStart, 1.0f);
        };
        const auto original = buildF0VisualSegments(
            originalEnergy, origEnd, makeFrameToX, makeFrameToY, originalProducer,
            colourAt, bandX0, bandX1);
        segments.insert(segments.end(), original.begin(), original.end());
    }

    const bool shouldDrawCorrected = notesPrimaryScheme
        ? pitchSnapshot.hasOriginalF0Data()
        : pitchSnapshot.hasCorrectionLayer();
    if (shouldDrawCorrected)
    {
        auto correctedProducer = [&](auto&& sink) {
            if (notesPrimaryScheme)
                owner.forEachEffectiveF0Span(startFrame, endFrameExclusive, sink);
            else
                pitchSnapshot.forEachCorrectionF0Span(startFrame, endFrameExclusive,
                    [&](int spanStartFrame, const float* values, int count) {
                        sink(spanStartFrame, values, count, 1.0f);
                    });
        };
        const auto corrected = buildF0VisualSegments(
            originalEnergy, endFrameExclusive, makeFrameToX, makeFrameToY, correctedProducer,
            colourAt, bandX0, bandX1);
        segments.insert(segments.end(), corrected.begin(), corrected.end());
    }
    return segments;
}

void compareSchemeAndViewports(bool notesPrimaryScheme, const Fixture& fx,
                               F0VisualCacheStore& store)
{
    // 时间数组跨 scheme 复用：两种 scheme 解析到同一条目（键不含 scheme/gapRatio）。
    const auto* entry = store.resolve(fx.owner, fx.owner, fx.projection);
    check(entry != nullptr, "committed snapshots resolve to an entry");
    check(entry == store.resolve(fx.owner, fx.owner, fx.projection),
          "time array shared across schemes");

    const Viewport viewports[] = {
        {9.9, 60.0, 60, 900},
        {10.2, 240.0, 60, 800},
        {10.55, 480.0, 60, 640},
    };
    for (const auto& vp : viewports)
    {
        ViewMapper coords;
        coords.visibleStartSeconds = vp.visibleStart;
        coords.pixelsPerSecond = vp.pixelsPerSecond;
        coords.contentStartX = vp.contentStartX;
        coords.contentWidth = vp.width;
        const PianoRollTimeMap map(fx.projection, *fx.owner->timeGrid, coords);
        int startFrame = 0;
        int endFrameExclusive = 0;
        visibleWindow(map, coords, kFrames, startFrame, endFrameExclusive);

        const auto direct = buildSegments(notesPrimaryScheme, *fx.owner, fx.projection, coords,
                                          startFrame, endFrameExclusive, nullptr);
        const auto cachedX = buildSegments(notesPrimaryScheme, *fx.owner, fx.projection, coords,
                                           startFrame, endFrameExclusive, entry);
        check(sameSegments(direct, cachedX),
              notesPrimaryScheme ? "OpenDyne cache-X equals direct"
                                 : "OpenTune cache-X equals direct");
    }
}

bool testF0VisualCacheDirectEquivalence()
{
    const auto fx = makeFixture();
    F0VisualCacheStore store;
    compareSchemeAndViewports(false, fx, store);
    compareSchemeAndViewports(true, fx, store);
    return failures == 0;
}

// 阻断 4 反例回归：守卫修正段（声明 [48,60) 仅 4 值）的发射判定依赖窗口端点。
void compareGuardWindows(bool notesPrimaryScheme, const Fixture& fx, F0VisualCacheStore& store)
{
    const auto* entry = store.resolve(fx.owner, fx.owner, fx.projection);
    check(entry != nullptr, "guard-window resolve");

    ViewMapper coords;
    coords.visibleStartSeconds = 10.0;
    coords.pixelsPerSecond = 2000.0;
    coords.contentStartX = 60;
    coords.contentWidth = 2000;

    const int windows[][2] = {{48, 52}, {48, 60}};
    for (const auto& window : windows)
    {
        const auto direct = buildSegments(notesPrimaryScheme, *fx.owner, fx.projection, coords,
                                          window[0], window[1], nullptr);
        const auto cachedX = buildSegments(notesPrimaryScheme, *fx.owner, fx.projection, coords,
                                           window[0], window[1], entry);
        check(sameSegments(direct, cachedX),
              notesPrimaryScheme ? "OpenDyne guard window cache equals direct"
                                 : "OpenTune guard window cache equals direct");

        // 守卫发射断言的口径是 corrected pass（原始 pass 与守卫判定无关）。
        const auto correctedOnly = buildSegments(notesPrimaryScheme, *fx.owner, fx.projection,
                                                 coords, window[0], window[1], nullptr, false);
        if (window[0] == 48 && window[1] == 52 && !notesPrimaryScheme)
            check(!correctedOnly.empty(), "narrow window emits the guarded segment's data");
        if (window[0] == 48 && window[1] == 60 && !notesPrimaryScheme)
            check(correctedOnly.empty(), "full-span request skips the guarded segment entirely");
        if (window[0] == 48 && window[1] == 60 && notesPrimaryScheme)
        {
            check(!correctedOnly.empty(),
                  "OpenDyne gap-fills the skipped segment with original x gapRatio");

            // 实值断言：frame 49 原始 270Hz，跳过守卫段后回落 original×gapRatio
            // （270 × 2^(2/12) ≈ 303.065Hz）。
            const PianoRollTimeMap map(fx.projection, *fx.owner->timeGrid, coords);
            const float frame49X = static_cast<float>(map.sourceToXExact(
                static_cast<double>(49) * kFrameSeconds));
            const double gapRatio = fx.state.pitchShiftSettings.getPitchRatio();
            const float expectedY = coords.freqToY(270.0f * static_cast<float>(gapRatio));
            bool frame49Matches = false;
            for (const auto& segment : correctedOnly)
                for (const auto& point : segment.points)
                    if (point.x == frame49X && point.y == expectedY)
                        frame49Matches = true;
            check(frame49Matches, "frame 49 falls back to original 270Hz x gapRatio (303.065Hz)");
        }
    }
}

bool testF0VisualCacheGuardWindowEmission()
{
    const auto fx = makeFixture();
    F0VisualCacheStore store;
    compareGuardWindows(false, fx, store);
    compareGuardWindows(true, fx, store);
    return failures == 0;
}

bool testF0VisualCacheStoreKeys()
{
    const auto fx = makeFixture();
    F0VisualCacheStore store;

    // 外层快照身份不同、帧轴/网格/投影相同 → 插件路径（每次新建外层快照）命中。
    auto outerAgain = std::make_shared<const EditableContentSnapshot>(makeContentSnapshot(fx.state));
    check(outerAgain != fx.owner, "plugin path creates a fresh outer snapshot per read");
    const auto* first = store.resolve(fx.owner, fx.owner, fx.projection);
    check(first != nullptr, "resolve builds entry");
    const auto* second = store.resolve(outerAgain, outerAgain, fx.projection);
    check(second == first, "frame axis values key the cache (plugin path hits)");

    // 纯音高/能量变更（新曲线对象、同一帧轴）→ 复用同一时间数组。
    // 网格共享同一 committed 对象（键按网格身份）。
    auto retuned = makeFixture();
    retuned.state.timeGrid = fx.state.timeGrid;
    auto retunedCurve = makeTestCurve()->deriveCopy();
    std::vector<float> shiftedF0(static_cast<std::size_t>(kFrames));
    for (int f = 0; f < kFrames; ++f)
        shiftedF0[static_cast<std::size_t>(f)] = 300.0f;
    retunedCurve->setOriginalF0(shiftedF0);
    retuned.state.analysis.pitchCurve = retunedCurve;
    retuned.owner = std::make_shared<const EditableContentSnapshot>(
        makeContentSnapshot(retuned.state));
    check(retuned.owner->pitchCurve != fx.owner->pitchCurve,
          "pure pitch change publishes a new curve object");
    check(store.resolve(retuned.owner, retuned.owner, retuned.projection) == first,
          "same frame axis reuses the timeline array across curve objects");

    // 拖拽预览（owner ≠ committed，帧轴不变）→ 与 committed 共用同一条目。
    const auto* previewEntry = store.resolve(retuned.owner, fx.owner, fx.projection);
    check(previewEntry == first, "preview pair reuses the committed time array");

    // 帧轴变更（帧数不同）→ 重建独立条目。
    auto shorter = makeFixture();
    auto shorterCurve = makeTestCurve()->deriveCopy();
    {
        std::vector<float> truncatedF0 = shorterCurve->getSnapshot()->getOriginalF0();
        truncatedF0.resize(static_cast<std::size_t>(kFrames) - 8);
        std::vector<float> truncatedEnergy = shorterCurve->getSnapshot()->getOriginalEnergy();
        truncatedEnergy.resize(static_cast<std::size_t>(kFrames) - 8);
        shorterCurve->setOriginalF0(truncatedF0);
        shorterCurve->setOriginalEnergy(truncatedEnergy);
    }
    shorter.state.analysis.pitchCurve = shorterCurve;
    shorter.owner = std::make_shared<const EditableContentSnapshot>(
        makeContentSnapshot(shorter.state));
    const auto* shorterEntry = store.resolve(shorter.owner, shorter.owner, shorter.projection);
    check(shorterEntry != nullptr && shorterEntry != first,
          "frame count change rebuilds a distinct entry");

    // committed 网格身份变更 → 重建独立条目。
    const auto otherFixture = makeFixture();
    const auto* otherGrid = store.resolve(otherFixture.owner, otherFixture.owner,
                                          otherFixture.projection);
    check(otherGrid != nullptr && otherGrid != first && otherGrid != shorterEntry,
          "different committed time grid rebuilds a distinct entry");

    // 投影任一双精度变更 → 重建独立条目。
    const ContentTimelineProjection shifted{10.5, fx.projection.timelineDurationSeconds,
                                            fx.projection.contentStartSeconds,
                                            fx.projection.contentDurationSeconds};
    const auto* shiftedEntry = store.resolve(fx.owner, fx.owner, shifted);
    const auto* afterShift = store.resolve(fx.owner, fx.owner, fx.projection);
    check(shiftedEntry != nullptr && shiftedEntry != afterShift && shiftedEntry != first,
          "projection change rebuilds a distinct entry");

    // 导入待分析：曲线快照为空 → 不缓存（直算侧自身判空不绘制）。
    auto pending = makeFixture();
    auto emptyState = pending.state;
    emptyState.analysis.pitchCurve = nullptr;
    const auto emptyOwner = std::make_shared<const EditableContentSnapshot>(
        makeContentSnapshot(emptyState));
    check(store.resolve(emptyOwner, emptyOwner, pending.projection) == nullptr,
          "null pitch curve does not cache and does not dereference");

    // prune：本轮未命中条目清除，命中条目保留。original 条目是首个插入（索引 0），
    // 其后条目的 erase 不移动索引 0，指针跨 prune 稳定可比较。
    const auto* kept = store.resolve(fx.owner, fx.owner, fx.projection);
    store.prune();
    const auto* afterPrune = store.resolve(fx.owner, fx.owner, fx.projection);
    check(afterPrune == kept, "prune keeps the entry used in the last pass");
    return failures == 0;
}

} // namespace

int main()
{
    testF0VisualCacheDirectEquivalence();
    testF0VisualCacheGuardWindowEmission();
    testF0VisualCacheStoreKeys();

    if (failures != 0)
    {
        std::fprintf(stderr, "F0VisualCacheTests: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("F0VisualCacheTests passed.");
    return 0;
}

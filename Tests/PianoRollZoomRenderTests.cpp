#include "Standalone/UI/PianoRollComponent.h"
#include "Standalone/UI/PianoRoll/PianoRollTimeMap.h"
#include "Utils/PlayHeadState.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/UndoManager.h"

#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace OpenTune {

// JUCE 软件渲染上下文转发器：仅在 strokePath 处记录传入的中心线 Path（观测），
// 其余调用全部转发继续真实绘制。复用生产 renderer 与 PianoRollComponent::drawContent
// 的实际代码路径，不新增生产测试钩子或镜像规则。
class F0StrokeRecordingContext : public juce::LowLevelGraphicsContext
{
public:
    explicit F0StrokeRecordingContext(juce::LowLevelGraphicsContext& target) : target(target) {}

    bool isVectorDevice() const override { return target.isVectorDevice(); }
    void setOrigin(juce::Point<int> origin) override { target.setOrigin(origin); }
    void addTransform(const juce::AffineTransform& transform) override { target.addTransform(transform); }
    float getPhysicalPixelScaleFactor() const override { return target.getPhysicalPixelScaleFactor(); }
    bool clipToRectangle(const juce::Rectangle<int>& area) override { return target.clipToRectangle(area); }
    bool clipToRectangleList(const juce::RectangleList<int>& area) override { return target.clipToRectangleList(area); }
    void excludeClipRectangle(const juce::Rectangle<int>& area) override { target.excludeClipRectangle(area); }
    void clipToPath(const juce::Path& path, const juce::AffineTransform& transform) override
    {
        target.clipToPath(path, transform);
    }
    void clipToImageAlpha(const juce::Image& image, const juce::AffineTransform& transform) override
    {
        target.clipToImageAlpha(image, transform);
    }
    bool clipRegionIntersects(const juce::Rectangle<int>& area) override { return target.clipRegionIntersects(area); }
    juce::Rectangle<int> getClipBounds() const override { return target.getClipBounds(); }
    bool isClipEmpty() const override { return target.isClipEmpty(); }
    void saveState() override { target.saveState(); }
    void restoreState() override { target.restoreState(); }
    void beginTransparencyLayer(float opacity) override { target.beginTransparencyLayer(opacity); }
    void endTransparencyLayer() override { target.endTransparencyLayer(); }
    void setFill(const juce::FillType& fill) override { target.setFill(fill); }
    void setOpacity(float opacity) override { target.setOpacity(opacity); }
    void setInterpolationQuality(juce::Graphics::ResamplingQuality quality) override
    {
        target.setInterpolationQuality(quality);
    }
    void fillRect(const juce::Rectangle<int>& area, bool replaceExistingContents) override
    {
        target.fillRect(area, replaceExistingContents);
    }
    void fillRect(const juce::Rectangle<float>& area) override { target.fillRect(area); }
    void fillRectList(const juce::RectangleList<float>& rects) override { target.fillRectList(rects); }
    void fillPath(const juce::Path& path, const juce::AffineTransform& transform) override
    {
        target.fillPath(path, transform);
    }
    void drawImage(const juce::Image& image, const juce::AffineTransform& transform) override
    {
        target.drawImage(image, transform);
    }
    void drawLine(const juce::Line<float>& line) override { target.drawLine(line); }
    void setFont(const juce::Font& font) override { target.setFont(font); }
    const juce::Font& getFont() override { return target.getFont(); }
    void drawGlyphs(juce::Span<const uint16_t> glyphs, juce::Span<const juce::Point<float>> positions,
                    const juce::AffineTransform& transform) override
    {
        target.drawGlyphs(glyphs, positions, transform);
    }
    std::unique_ptr<juce::ImageType> getPreferredImageTypeForTemporaryImages() const override
    {
        return target.getPreferredImageTypeForTemporaryImages();
    }
    uint64_t getFrameId() const override { return target.getFrameId(); }

    void strokePath(const juce::Path& path, const juce::PathStrokeType& strokeType,
                    const juce::AffineTransform& transform) override
    {
        strokes.push_back(path);
        target.strokePath(path, strokeType, transform);
    }

    std::vector<juce::Path> strokes;

private:
    juce::LowLevelGraphicsContext& target;
};

struct PianoRollComponentTestProbe
{
    static void zoom(PianoRollComponent& roll, int x, double factor)
    {
        roll.beginZoomPreviewAt(x, factor);
    }

    static void endGesture(PianoRollComponent& roll)
    {
        roll.zoomDeadlineTicks_ = 0;
        roll.probeReportWindowStart_ = juce::Time::getMillisecondCounterHiRes();
    }

    static juce::Image capture(PianoRollComponent& roll, float scale)
    {
        juce::Image frame(juce::Image::ARGB, static_cast<int>(roll.getWidth() * scale),
                          static_cast<int>(roll.getHeight() * scale), true);
        juce::Graphics g(frame);
        g.addTransform(juce::AffineTransform::scale(scale));
        roll.paint(g);
        return frame;
    }

    static bool visibleInk(const juce::Image& layer, juce::Rectangle<int> region, float scale)
    {
        if (!layer.isValid())
            return false;
        const auto physical = (region.toFloat() * scale).getSmallestIntegerContainer()
            .getIntersection(layer.getBounds());
        juce::Image::BitmapData pixels(layer, juce::Image::BitmapData::readOnly);
        for (int y = physical.getY(); y < physical.getBottom(); ++y)
            for (int x = physical.getX(); x < physical.getRight(); ++x)
                if (pixels.getPixelColour(x, y).getAlpha() != 0)
                    return true;
        return false;
    }

    static bool markersVisible(const PianoRollComponent& roll, const EditableContentSnapshot& snapshot,
                               float scale, bool notesPrimary)
    {
        const auto view = roll.makeViewMapper();
        const auto projection = roll.activeContentProjection();
        const PianoRollTimeMap map(projection, *snapshot.timeGrid, view);
        const auto at = [&](double source, float frequency) {
            return juce::Rectangle<int>(map.sourceToX(source) - 3,
                roll.rulerHeight_ + juce::roundToInt(view.freqToY(frequency)) - 3, 7, 7);
        };
        return visibleInk(roll.contentSurface_, at(32.0, 220.0f), scale)
            && visibleInk(roll.contentSurface_, at(32.0, 440.0f), scale)
            && (notesPrimary || visibleInk(roll.waveformSurface_,
                    {map.sourceToX(32.0) - 3, roll.rulerHeight_ + 8, 7, 90}, scale));
    }

    static int staticRasters(const PianoRollComponent& roll) { return roll.staticRasterProbe_.count; }
    static int contentRasters(const PianoRollComponent& roll) { return roll.contentRasterProbe_.count; }

    // ── F0 缩放回归验收 ──
    static int contentStartX(const PianoRollComponent& roll) { return roll.getTimelineContentStartX(); }
    static int contentHeight(const PianoRollComponent& roll) { return roll.getTimelineContentViewportHeight(); }
    static int rulerTop(const PianoRollComponent& roll) { return roll.rulerHeight_; }
    static float renderScale(const PianoRollComponent& roll) { return roll.renderScale_; }
    static juce::Image contentSurfaceCopy(const PianoRollComponent& roll) { return roll.contentSurface_.createCopy(); }
    static PianoRollRenderer::ContentRenderItems buildItems(PianoRollComponent& roll)
    {
        return roll.buildContentRenderItems();
    }
    static ViewMapper viewMapper(PianoRollComponent& roll) { return roll.makeViewMapper(); }
    static void rasterContentStrip(PianoRollComponent& roll, const PianoRollRenderer::ContentRenderItems& items,
                                   juce::Rectangle<int> logical, juce::Rectangle<int> physical)
    {
        roll.rasterizeContent(items, logical, physical);
    }
    // 经生产 drawContent 真实绘制一遍内容层，只取 strokePath 传入的 F0 中心线路径。
    static std::vector<juce::Path> captureF0Strokes(PianoRollComponent& roll,
                                                    const PianoRollRenderer::ContentRenderItems& items,
                                                    bool showOriginal, bool showCorrected)
    {
        roll.showOriginalF0_ = showOriginal;
        roll.showCorrectedF0_ = showCorrected;
        juce::Image canvas(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
        juce::LowLevelGraphicsSoftwareRenderer software(canvas);
        F0StrokeRecordingContext recorder(software);
        {
            juce::Graphics g(recorder);
            roll.drawContent(g, roll.viewportState(), roll.getTimelineViewportBounds(),
                             PianoRollComponent::ContentLayer::Notes, items);
        }
        roll.showOriginalF0_ = true;
        roll.showCorrectedF0_ = true;
        return std::move(recorder.strokes);
    }
};

} // namespace OpenTune

namespace {

using namespace OpenTune;
using Probe = PianoRollComponentTestProbe;

bool samePixels(const juce::Image& a, const juce::Image& b)
{
    const juce::Image::BitmapData lhs(a, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData rhs(b, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (lhs.getPixelColour(x, y) != rhs.getPixelColour(x, y))
                return false;
    return true;
}

std::shared_ptr<const EditableContentSnapshot> makeSnapshot()
{
    constexpr int seconds = 48;
    constexpr int rate = 16000;
    constexpr int hop = 160;
    constexpr int framesPerSecond = rate / hop;
    auto snapshot = std::make_shared<EditableContentSnapshot>();
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, seconds * rate);
    audio->clear();
    std::vector<float> f0(seconds * framesPerSecond, 0.0f);
    std::vector<float> energy(f0.size(), 0.0f);
    for (int start : {14, 30}) {
        Note note;
        note.startTime = start;
        note.endTime = start + 4;
        note.pitch = note.originalPitch = 220.0f;
        snapshot->notes.push_back(note);
        for (int i = start * framesPerSecond; i < (start + 4) * framesPerSecond; ++i) {
            f0[static_cast<std::size_t>(i)] = 440.0f;
            energy[static_cast<std::size_t>(i)] = 0.75f;
        }
        for (int i = start * rate; i < (start + 4) * rate; ++i)
            audio->setSample(0, i, i % 16 < 8 ? 0.85f : -0.85f);
    }
    // F0 缩放回归素材：TimeGrid 在 [20,20.5)s 压缩 0.3×、[20.5,21.5)s 展宽 1.5×。
    // 压缩段帧距 < 1px（同列相邻帧），内含一帧陡升 300→900、同列折返峰 1200、
    // 一帧陡降 →300；展宽段帧距约 3.3px，含宽列陡升/陡降与单帧有声断点；
    // 两个修正段之间留间隙，其一覆盖无声区。其余帧无声，段远离生产窗口边界。
    for (int i = 2000; i < 2020; ++i) f0[static_cast<std::size_t>(i)] = 300.0f;
    for (int i = 2020; i < 2025; ++i) f0[static_cast<std::size_t>(i)] = 900.0f;
    f0[static_cast<std::size_t>(2025)] = 1200.0f;
    for (int i = 2026; i < 2050; ++i) f0[static_cast<std::size_t>(i)] = 300.0f;
    for (int i = 2100; i < 2120; ++i) f0[static_cast<std::size_t>(i)] = 400.0f;
    for (int i = 2120; i < 2125; ++i) f0[static_cast<std::size_t>(i)] = 1000.0f;
    for (int i = 2125; i < 2130; ++i) f0[static_cast<std::size_t>(i)] = 400.0f;
    f0[static_cast<std::size_t>(2131)] = 1400.0f;
    // 压力段能量斜坡：全视口能量 min（帧 2000，压力段 A 起点）与 max（帧 2100，
    // 压力段 B 起点）都落在两条带列之外，条带局部化着色参考可被全绘 vs 条带
    // 逐像素比较检出。两压力段内部线性渐变，单帧断点取中值。
    for (int i = 2000; i <= 2049; ++i)
        energy[static_cast<std::size_t>(i)] = 0.05f + 0.40f * static_cast<float>(i - 2000) / 49.0f;
    for (int i = 2100; i <= 2131; ++i)
        energy[static_cast<std::size_t>(i)] = 0.95f - 0.45f * static_cast<float>(i - 2100) / 31.0f;
    std::vector<PitchCorrectionSegment> corrections;
    corrections.emplace_back(2005, 2030, std::vector<float>(25, 500.0f));
    corrections.emplace_back(2075, 2090, std::vector<float>(15, 700.0f));
    snapshot->audioBuffer = std::move(audio);
    snapshot->audioSampleRate = rate;
    snapshot->pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::move(f0), std::move(energy), std::move(corrections), hop, static_cast<double>(rate));
    snapshot->timeGrid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 20.0, 20.0, HandleKind::UserAdded},
        {3, 20.5, 20.15, HandleKind::UserAdded},
        {4, 21.5, 21.65, HandleKind::UserAdded},
        {5, 48.0, 48.0, HandleKind::ClipEnd},
    });
    snapshot->originalF0State = OriginalF0State::Ready;
    snapshot->noteTopologyInitialized = true;
    return snapshot;
}

bool verifyZoom(const std::shared_ptr<const EditableContentSnapshot>& snapshot,
                AudioEditingScheme::Scheme scheme, float scale)
{
    const bool notesPrimary = scheme == AudioEditingScheme::Scheme::NotesPrimary;
    const char* name = notesPrimary ? "OpenDyne" : "OpenTune";
    const auto check = [=](bool ok, const char* message) {
        if (!ok)
            std::fprintf(stderr, "%s scale %.2f: %s\n", name, scale, message);
        return ok;
    };

    PlayHeadState playhead;
    UndoManager undo;
    PianoRollComponent live(playhead, undo, {}, false);
    PianoRollComponent reference(playhead, undo, {}, false);
    const ContentKey key{DomainKind::StandaloneClip, 1};
    constexpr ContentTimelineProjection placement{2.0, 48.0, 0.0, 48.0};
    for (auto* roll : {&live, &reference}) {
        roll->setSize(1000, 600);
        roll->setAudioEditingScheme(scheme);
        roll->setShowWaveform(true);
        roll->setShowPianoKeyboard(!notesPrimary);
        roll->setReadContentSnapshot([=](ContentKey requested) { return requested == key ? snapshot : nullptr; });
        roll->setEditedContentAndProjection(key, snapshot->pitchCurve, snapshot->audioBuffer,
                                            snapshot->audioSampleRate, placement);
        auto& cache = roll->getWaveformMipmapCache();
        while (!cache.isComplete())
            cache.buildIncremental(100.0);
    }

    auto initial = live.viewportState();
    initial.camera.visibleStartSeconds = 15.0;
    initial.camera.pixelsPerSecond = 300.0;
    initial.pixelsPerSemitone = 18.0f;
    initial.verticalScrollOffset = 520.0f;
    live.restoreViewportState(initial);
    Probe::capture(live, scale);
    if (!check(!Probe::markersVisible(live, *snapshot, scale, notesPrimary), "far markers should start offscreen"))
        return false;

    const int x = live.timelinePolicyViewportWidth() / 2;
    constexpr double factors[]{0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 1.25, 0.65, 0.65, 0.65, 0.65,
                               1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35};
    bool sawGlobal = false;
    juce::Image last;
    for (double factor : factors) {
        const auto before = live.timelineCamera();
        Probe::zoom(live, x, factor);
        const auto after = live.timelineCamera();
        if (after.visibleStartSeconds > 0.0) {
            const double startAnchor = before.visibleStartSeconds + x / before.pixelsPerSecond;
            const double endAnchor = after.visibleStartSeconds + x / after.pixelsPerSecond;
            if (!check(std::abs(startAnchor - endAnchor) < 1.0e-9, "zoom anchor drifted"))
                return false;
        }
        last = Probe::capture(live, scale);
        reference.restoreViewportState(live.viewportState());
        if (!check(samePixels(last, Probe::capture(reference, scale)), "gesture frame differs from fresh current camera"))
            return false;

        if (after.visibleStartSeconds == 0.0 && x * 2 / after.pixelsPerSecond > placement.timelineEndSeconds()) {
            sawGlobal = true;
            if (!check(Probe::markersVisible(live, *snapshot, scale, notesPrimary), "newly exposed waveform, F0 or note is missing"))
                return false;
        }
    }

    Probe::endGesture(live);
    const int staticRasters = Probe::staticRasters(live);
    const int contentRasters = Probe::contentRasters(live);
    const auto settled = Probe::capture(live, scale);
    return check(sawGlobal, "zoom trajectory never exposed the entire clip")
        && check(samePixels(last, settled), "first settled frame differs from the last gesture frame")
        && check(Probe::staticRasters(live) == staticRasters && Probe::contentRasters(live) == contentRasters,
                 "ending a presented gesture rebuilt retained surfaces");
}

// ── F0 中心线几何期望（独立前向投影：帧 → source 时间 → 时间轴 → 屏幕 X；频率 → 纵向 Y） ──

struct PathElement
{
    juce::Path::Iterator::PathElementType type;
    float x1 = 0.0f, y1 = 0.0f;
};

std::vector<PathElement> pathElements(const juce::Path& path)
{
    std::vector<PathElement> elements;
    juce::Path::Iterator it(path);
    while (it.next())
        elements.push_back({it.elementType, it.x1, it.y1});
    return elements;
}

// 生产路径形状规格（appendLinearF0Path，逐帧折线）：多点段 = 起点 + 逐点 lineTo；
// 单点段 = ±0.01px 短横线（生产 ptPath）。X 为连续投影（double→float 一次落定）。
std::vector<PathElement> expectedCenterline(const std::vector<juce::Point<float>>& points)
{
    juce::Path path;
    if (points.size() == 1) {
        path.startNewSubPath(points.front().x - 0.01f, points.front().y);
        path.lineTo(points.front().x + 0.01f, points.front().y);
    } else {
        path.startNewSubPath(points.front().x, points.front().y);
        for (std::size_t i = 1; i < points.size(); ++i)
            path.lineTo(points[i].x, points[i].y);
    }
    return pathElements(path);
}

struct F0FixtureModel
{
    const std::vector<float>& original;
    const std::vector<PitchCorrectionSegment>& corrections;

    float originalAt(int frame) const { return original[static_cast<std::size_t>(frame)]; }
    float correctedAt(int frame) const
    {
        for (const auto& segment : corrections)
            if (frame >= segment.startFrame && frame < segment.endFrame)
                return segment.f0Data[static_cast<std::size_t>(frame - segment.startFrame)];
        return 0.0f;
    }
    float effectiveAt(int frame) const
    {
        // 与生产 forEachEffectiveF0Span 同口径：修正段覆盖即取修正值，不看数值正负。
        for (const auto& segment : corrections)
            if (frame >= segment.startFrame && frame < segment.endFrame)
                return segment.f0Data[static_cast<std::size_t>(frame - segment.startFrame)];
        return originalAt(frame);
    }
};

using FreqAt = float (F0FixtureModel::*)(int) const;

// 素材自有扫描域：覆盖全部压力段，其余帧无声；压力段不贴生产可见窗口边界。
std::vector<std::pair<int, int>> voicedRuns(const F0FixtureModel& model, FreqAt freqAt)
{
    constexpr int scanFirst = 1900;
    constexpr int scanLast = 2200;
    std::vector<std::pair<int, int>> runs;
    int first = -1;
    for (int frame = scanFirst; frame < scanLast; ++frame) {
        const float hz = (model.*freqAt)(frame);
        const bool voiced = hz >= 20.0f && hz <= 2000.0f;   // 与生产 buildF0VisualSegments 同判据
        if (voiced && first < 0)
            first = frame;
        if (!voiced && first >= 0) {
            runs.push_back({first, frame - 1});
            first = -1;
        }
    }
    if (first >= 0)
        runs.push_back({first, scanLast - 1});
    return runs;
}

struct SegmentExpectation
{
    std::vector<juce::Point<float>> points;
    std::vector<PathElement> elements;
};

std::vector<SegmentExpectation> expectedSegments(const F0FixtureModel& model, FreqAt freqAt,
                                                 const F0Timeline& timeline,
                                                 const PianoRollTimeMap& map, const ViewMapper& mapper)
{
    std::vector<SegmentExpectation> segments;
    for (const auto& [firstFrame, lastFrame] : voicedRuns(model, freqAt)) {
        std::vector<juce::Point<float>> points;
        for (int frame = firstFrame; frame <= lastFrame; ++frame)
            points.push_back({static_cast<float>(map.sourceToXExact(timeline.timeAtFrame(frame))),
                              mapper.freqToY((model.*freqAt)(frame))});
        segments.push_back({points, expectedCenterline(points)});
    }
    return segments;
}

std::vector<std::vector<PathElement>> recordedSegments(PianoRollComponent& roll,
                                                       const PianoRollRenderer::ContentRenderItems& items,
                                                       bool showOriginal, bool showCorrected)
{
    const auto strokes = Probe::captureF0Strokes(roll, items, showOriginal, showCorrected);
    std::vector<juce::Path> unique;
    for (const auto& stroke : strokes)
        if (unique.empty() || !(unique.back() == stroke))
            unique.push_back(stroke);   // 同段多遍描边（辉光/内辉光/主线）共用同一 Path
    std::vector<std::vector<PathElement>> segments;
    for (const auto& path : unique)
        segments.push_back(pathElements(path));
    return segments;
}

bool centerlineMatches(const std::vector<std::vector<PathElement>>& recorded,
                       const std::vector<std::vector<PathElement>>& expected)
{
    if (recorded.size() != expected.size()) {
        std::fprintf(stderr, "centerline segment count %zu != expected %zu\n",
                     recorded.size(), expected.size());
        return false;
    }
    constexpr float tolerance = 1.0e-4f;
    for (std::size_t segment = 0; segment < expected.size(); ++segment) {
        const auto& got = recorded[segment];
        const auto& want = expected[segment];
        if (got.size() != want.size()) {
            std::fprintf(stderr, "centerline segment %zu element count %zu != %zu\n",
                         segment, got.size(), want.size());
            return false;
        }
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (got[i].type != want[i].type
                || std::abs(got[i].x1 - want[i].x1) > tolerance
                || std::abs(got[i].y1 - want[i].y1) > tolerance) {
                std::fprintf(stderr, "centerline segment %zu element %zu differs\n", segment, i);
                return false;
            }
        }
    }
    return true;
}

// 跨 camera 一致：纵向逐点相等（纵向尺寸不变），横向按 1.25× 精确缩放。
// X 容差 1e-3 覆盖连续投影 double→float 及加法结合次序的舍入噪声（≈几个 float ulp）。
bool crossCameraConsistent(const std::vector<std::vector<PathElement>>& base,
                           const std::vector<std::vector<PathElement>>& zoomed,
                           double ratio, int contentStartX)
{
    if (base.size() != zoomed.size()) {
        std::fprintf(stderr, "cross camera segment count %zu != %zu\n", base.size(), zoomed.size());
        return false;
    }
    constexpr float yTolerance = 1.0e-4f;
    constexpr double xTolerance = 1.0e-3;
    for (std::size_t segment = 0; segment < base.size(); ++segment) {
        if (base[segment].size() != zoomed[segment].size()) {
            std::fprintf(stderr, "cross camera segment %zu element count differs\n", segment);
            return false;
        }
        const auto& b = base[segment];
        const auto& z = zoomed[segment];
        // 单帧短横线：生产固定 ±0.01px 半宽不随缩放，比较中心 X 的缩放、Y 相等
        // 与半宽不变；多点段仍逐元素比较。
        if (b.size() == 2
            && std::abs(b[1].x1 - b[0].x1) <= 0.0201f
            && std::abs(b[1].y1 - b[0].y1) <= yTolerance) {
            const float baseMidX = 0.5f * (b[0].x1 + b[1].x1);
            const float zoomMidX = 0.5f * (z[0].x1 + z[1].x1);
            if (std::abs((zoomMidX - contentStartX) - ratio * (baseMidX - contentStartX)) > xTolerance
                || std::abs(z[0].y1 - b[0].y1) > yTolerance
                || std::abs(std::abs(z[1].x1 - z[0].x1) - std::abs(b[1].x1 - b[0].x1)) > yTolerance) {
                std::fprintf(stderr, "cross camera short line segment %zu differs\n", segment);
                return false;
            }
            continue;
        }
        for (std::size_t i = 0; i < b.size(); ++i) {
            if (b[i].type != z[i].type
                || std::abs(z[i].y1 - b[i].y1) > yTolerance
                || std::abs((z[i].x1 - contentStartX) - ratio * (b[i].x1 - contentStartX)) > xTolerance) {
                std::fprintf(stderr, "cross camera segment %zu element %zu differs\n", segment, i);
                return false;
            }
        }
    }
    return true;
}

int inkRowSpan(const juce::Image& layer, int x0, int x1, int y0, int y1)
{
    juce::Image::BitmapData pixels(layer, juce::Image::BitmapData::readOnly);
    int first = -1, last = -1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (pixels.getPixelColour(x, y).getAlpha() != 0) {
                if (first < 0)
                    first = y;
                last = y;
                break;
            }
    return first < 0 ? 0 : last - first + 1;
}

// 跨 camera 几何验收：同一音频、同一 F0 曲线、固定纵向，横向 1× 与 1.25×。
bool verifyF0ZoomGeometry(const std::shared_ptr<const EditableContentSnapshot>& snapshot,
                          AudioEditingScheme::Scheme scheme)
{
    const bool notesPrimary = scheme == AudioEditingScheme::Scheme::NotesPrimary;
    const char* name = notesPrimary ? "OpenDyne" : "OpenTune";
    const auto check = [=](bool ok, const char* message) {
        if (!ok)
            std::fprintf(stderr, "%s F0 geometry: %s\n", name, message);
        return ok;
    };

    PlayHeadState playhead;
    UndoManager undo;
    PianoRollComponent roll(playhead, undo, {}, false);
    roll.setSize(1000, 600);
    roll.setAudioEditingScheme(scheme);
    roll.setShowWaveform(false);
    roll.setShowPianoKeyboard(!notesPrimary);
    const ContentKey key{DomainKind::StandaloneClip, 1};
    constexpr ContentTimelineProjection placement{2.0, 48.0, 0.0, 48.0};
    roll.setReadContentSnapshot([=](ContentKey requested) { return requested == key ? snapshot : nullptr; });
    roll.setEditedContentAndProjection(key, snapshot->pitchCurve, snapshot->audioBuffer,
                                       snapshot->audioSampleRate, placement);
    const auto items = Probe::buildItems(roll);
    if (!check(!items.empty(), "content render item missing"))
        return false;

    const auto& curve = *snapshot->pitchCurve;
    const F0FixtureModel model{curve.getOriginalF0(), curve.getCorrectionSegments()};
    const F0Timeline timeline(curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size()));

    constexpr double basePixelsPerSecond = 220.0;
    constexpr double zoomRatio = 1.25;
    auto setCamera = [&](double pixelsPerSecond) {
        auto state = roll.viewportState();
        state.camera.visibleStartSeconds = 21.5;
        state.camera.pixelsPerSecond = pixelsPerSecond;
        state.pixelsPerSemitone = 18.0f;
        state.verticalScrollOffset = 300.0f;
        roll.restoreViewportState(state);
    };
    auto expectation = [&]() {
        const ViewMapper mapper = Probe::viewMapper(roll);
        const PianoRollTimeMap map(roll.activeContentProjection(), *snapshot->timeGrid, mapper);
        return std::make_tuple(
            expectedSegments(model, &F0FixtureModel::originalAt, timeline, map, mapper),
            expectedSegments(model, &F0FixtureModel::correctedAt, timeline, map, mapper),
            expectedSegments(model, &F0FixtureModel::effectiveAt, timeline, map, mapper));
    };
    auto elementsOf = [](const std::vector<SegmentExpectation>& segments) {
        std::vector<std::vector<PathElement>> out;
        for (const auto& segment : segments)
            out.push_back(segment.elements);
        return out;
    };

    setCamera(basePixelsPerSecond);
    const int contentStartX = Probe::contentStartX(roll);
    const auto [baseOriginal, baseCorrected, baseEffective] = expectation();

    // 素材自检：压缩段同列折返（相邻帧 X 差 < 1px 而纵向跳变最大）、展宽段宽列
    // 陡变、单帧断点、修正段间隙确实成立。
    if (!check(baseOriginal.size() == 3, "fixture should voice three original runs"))
        return false;
    const auto& runA = baseOriginal[0].points;
    int foldIndex = 0;
    float foldRise = 0.0f;
    for (std::size_t i = 1; i < runA.size(); ++i) {
        const float rise = std::abs(runA[i - 1].y - runA[i].y);
        if (std::abs(runA[i - 1].x - runA[i].x) < 1.0f && rise > foldRise) {
            foldIndex = static_cast<int>(i);
            foldRise = rise;
        }
    }
    if (!check(foldIndex > 0 && foldRise > 300.0f,
               "fixture should fold back inside one pixel column"))
        return false;
    bool steepWide = false;
    const auto& runB = baseOriginal[1].points;
    for (std::size_t i = 1; i < runB.size() && !steepWide; ++i)
        steepWide = runB[i].x - runB[i - 1].x >= 2.0f
            && std::abs(runB[i].y - runB[i - 1].y) > 150.0f;
    if (!check(steepWide, "fixture should carry a wide-column steep transition"))
        return false;
    if (!check(baseOriginal[2].points.size() == 1, "fixture should isolate a single voiced frame"))
        return false;
    if (!check(baseCorrected.size() == 2, "fixture should gap two correction spans"))
        return false;

    const auto baseRecordedOriginal = recordedSegments(roll, items, true, false);
    const auto baseRecordedCorrected = recordedSegments(roll, items, false, true);
    if (notesPrimary) {
        if (!check(baseRecordedOriginal.empty(), "OpenDyne must not stroke a separate original curve"))
            return false;
        if (!check(centerlineMatches(baseRecordedCorrected, elementsOf(baseEffective)),
                   "effective F0 centerline diverges from per-frame forward projection"))
            return false;
    } else {
        if (!check(centerlineMatches(baseRecordedOriginal, elementsOf(baseOriginal)),
                   "original F0 centerline diverges from per-frame forward projection"))
            return false;
        if (!check(centerlineMatches(baseRecordedCorrected, elementsOf(baseCorrected)),
                   "corrected F0 centerline diverges from per-frame forward projection"))
            return false;
    }

    setCamera(basePixelsPerSecond * zoomRatio);
    const auto [zoomOriginal, zoomCorrected, zoomEffective] = expectation();
    const auto zoomRecordedOriginal = recordedSegments(roll, items, true, false);
    const auto zoomRecordedCorrected = recordedSegments(roll, items, false, true);
    if (notesPrimary) {
        if (!check(zoomRecordedOriginal.empty(), "OpenDyne must not stroke a separate original curve"))
            return false;
        if (!check(centerlineMatches(zoomRecordedCorrected, elementsOf(zoomEffective)),
                   "effective F0 centerline diverges at 1.25x camera"))
            return false;
        if (!check(crossCameraConsistent(baseRecordedCorrected, zoomRecordedCorrected, zoomRatio, contentStartX),
                   "1.25x camera changed F0 vertical geometry or broke sequence"))
            return false;
    } else {
        if (!check(centerlineMatches(zoomRecordedOriginal, elementsOf(zoomOriginal)),
                   "original F0 centerline diverges at 1.25x camera"))
            return false;
        if (!check(centerlineMatches(zoomRecordedCorrected, elementsOf(zoomCorrected)),
                   "corrected F0 centerline diverges at 1.25x camera"))
            return false;
        if (!check(crossCameraConsistent(baseRecordedOriginal, zoomRecordedOriginal, zoomRatio, contentStartX),
                   "1.25x camera changed original F0 vertical geometry or broke sequence"))
            return false;
        if (!check(crossCameraConsistent(baseRecordedCorrected, zoomRecordedCorrected, zoomRatio, contentStartX),
                   "1.25x camera changed corrected F0 vertical geometry or broke sequence"))
            return false;
    }
    return true;
}

// 全绘 vs 窄条带：经生产 paint 全量栅格与生产条带补绘逐像素一致（含端部与陡变），
// 并用真实离屏遮罩独立断言同列折返着墨与修正段间隙空白。
bool verifyF0StripPixels(const std::shared_ptr<const EditableContentSnapshot>& snapshot,
                         AudioEditingScheme::Scheme scheme, float scale)
{
    const bool notesPrimary = scheme == AudioEditingScheme::Scheme::NotesPrimary;
    const char* name = notesPrimary ? "OpenDyne" : "OpenTune";
    const auto check = [=](bool ok, const char* message) {
        if (!ok)
            std::fprintf(stderr, "%s scale %.2f F0 strip: %s\n", name, scale, message);
        return ok;
    };

    PlayHeadState playhead;
    UndoManager undo;
    PianoRollComponent roll(playhead, undo, {}, false);
    roll.setSize(1000, 600);
    roll.setAudioEditingScheme(scheme);
    roll.setShowWaveform(false);
    roll.setShowPianoKeyboard(!notesPrimary);
    const ContentKey key{DomainKind::StandaloneClip, 1};
    constexpr ContentTimelineProjection placement{2.0, 48.0, 0.0, 48.0};
    roll.setReadContentSnapshot([=](ContentKey requested) { return requested == key ? snapshot : nullptr; });
    roll.setEditedContentAndProjection(key, snapshot->pitchCurve, snapshot->audioBuffer,
                                       snapshot->audioSampleRate, placement);
    const auto items = Probe::buildItems(roll);
    if (!check(!items.empty(), "content render item missing"))
        return false;

    auto state = roll.viewportState();
    state.camera.visibleStartSeconds = 21.5;
    state.camera.pixelsPerSecond = 220.0;
    state.pixelsPerSemitone = 18.0f;
    state.verticalScrollOffset = 300.0f;
    roll.restoreViewportState(state);

    Probe::capture(roll, scale);
    const auto full = Probe::contentSurfaceCopy(roll);
    const float renderScale = Probe::renderScale(roll);
    const int contentStartX = Probe::contentStartX(roll);
    const int contentTop = Probe::rulerTop(roll);
    const int contentHeight = Probe::contentHeight(roll);

    const auto& curve = *snapshot->pitchCurve;
    const F0FixtureModel model{curve.getOriginalF0(), curve.getCorrectionSegments()};
    const F0Timeline timeline(curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size()));
    const ViewMapper mapper = Probe::viewMapper(roll);
    const PianoRollTimeMap map(roll.activeContentProjection(), *snapshot->timeGrid, mapper);
    const auto originalRuns = expectedSegments(model, &F0FixtureModel::originalAt, timeline, map, mapper);
    const auto correctedRuns = expectedSegments(model, &F0FixtureModel::correctedAt, timeline, map, mapper);

    // 条带 1 从中部切入压缩段（折返峰、陡升、陡降与段末端在带内）；
    // 条带 2 从中部切入展宽段（陡升/陡降与段末端在带内）并覆盖单帧断点。
    const juce::Rectangle<int> strips[] = {
        {contentStartX + 120, contentTop, 32, contentHeight},
        {contentStartX + 360, contentTop, 60, contentHeight},
    };
    if (!check(originalRuns.size() == 3 && correctedRuns.size() == 2
               && originalRuns[0].points.front().x < strips[0].getX()
               && originalRuns[0].points.back().x > strips[0].getX()
               && originalRuns[0].points.back().x < strips[0].getRight()
               && originalRuns[1].points.front().x < strips[1].getX()
               && originalRuns[1].points.back().x > strips[1].getX()
               && originalRuns[1].points.back().x < strips[1].getRight()
               && originalRuns[2].points.front().x >= strips[1].getX()
               && originalRuns[2].points.front().x < strips[1].getRight(),
               "fixture runs no longer overlap the probe strips"))
        return false;

    for (const auto& logical : strips) {
        const auto physical = (logical.toFloat() * renderScale).getSmallestIntegerContainer();
        const auto rasterLogical = (physical.toFloat() / renderScale).getSmallestIntegerContainer();
        Probe::rasterContentStrip(roll, items, rasterLogical, physical);
        if (!check(samePixels(full, Probe::contentSurfaceCopy(roll)),
                   "dirty strip redraw differs from the full raster"))
            return false;
    }

    if (scale == 1.0f) {
        // 修正段间隙（含无声区）必须无墨迹。余量 6px 覆盖最大辉光描边半宽
        // （2.05+1.8=3.85 → 1.93px）加圆帽沿切向外扩与抗锯齿扩散。
        const int gapLeft = static_cast<int>(correctedRuns.back().points.back().x) + 6;
        const int gapRight = static_cast<int>(originalRuns[1].points.front().x) - 6;
        if (!check(gapRight > gapLeft && !Probe::visibleInk(full,
                       {gapLeft, contentTop, gapRight - gapLeft, contentHeight}, 1.0f),
                   "correction gap over silence must stay blank"))
            return false;
        // 原曲线同列折返：峰谷墨迹在同一像素列内贯通。
        if (!notesPrimary) {
            const auto& run = originalRuns[0].points;
            std::size_t foldIndex = 0;
            float foldRise = 0.0f;
            for (std::size_t i = 1; i < run.size(); ++i) {
                const float rise = std::abs(run[i - 1].y - run[i].y);
                if (std::abs(run[i - 1].x - run[i].x) < 1.0f && rise > foldRise) {
                    foldIndex = i;
                    foldRise = rise;
                }
            }
            const int foldLeft = static_cast<int>(
                juce::jmin(run[foldIndex - 1].x, run[foldIndex].x)) - 1;
            const int foldRight = static_cast<int>(
                juce::jmax(run[foldIndex - 1].x, run[foldIndex].x)) + 2;
            if (!check(foldRise > 300.0f
                       && inkRowSpan(full, foldLeft, foldRight, contentTop,
                                     contentTop + contentHeight) > 150,
                       "fold-back peak and valley must ink one column"))
                return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto snapshot = makeSnapshot();
    for (auto scheme : {AudioEditingScheme::Scheme::CorrectedF0Primary, AudioEditingScheme::Scheme::NotesPrimary})
        for (float scale : {1.0f, 1.25f})
            if (!verifyZoom(snapshot, scheme, scale))
                return 1;
    for (auto scheme : {AudioEditingScheme::Scheme::CorrectedF0Primary, AudioEditingScheme::Scheme::NotesPrimary}) {
        if (!verifyF0ZoomGeometry(snapshot, scheme))
            return 1;
        for (float scale : {1.0f, 1.25f})
            if (!verifyF0StripPixels(snapshot, scheme, scale))
                return 1;
    }
    std::puts("PianoRollZoomRenderTests passed (zoom gestures plus F0 cross-camera geometry and strips, "
              "OpenTune and OpenDyne, 1x and 1.25x).");
    return 0;
}

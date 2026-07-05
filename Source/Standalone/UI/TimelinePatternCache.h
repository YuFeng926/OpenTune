#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <string>
#include <optional>
#include <unordered_map>

namespace OpenTune {

// ============================================================================
// PatternTileKey — 完整的 tile 生成参数
// 不同视图、缩放、主题、垂直布局都会产生不同的 key -> 不同的 tile
// ============================================================================
struct PatternTileKey {
    std::string viewKind;        // "pianoroll" / "arrangement"
    double startSeconds;         // 缓存块起始时间（absolute timeline seconds）
    double endSeconds;           // 缓存块结束时间
    double pixelsPerSecond;      // 缩放
    int timeUnit;                // 0 = Seconds, 1 = Bars
    int tempo;                   // BPM
    int timeSigNumerator;        // 拍号分子
    int timeSigDenominator;      // 拍号分母
    int themeId;                 // ThemeId 的 int 值
    std::uint64_t verticalGeometry;  // 垂直布局 hash (64-bit for full vertical window)
    int laneStyle;               // lane 风格 hash（showLanes, scaleRootNote, scaleType, noteNameMode, showUnvoicedFrames）
    int trackHeight = 100;       // track height for arrangement (pixels per track lane)

    bool operator==(const PatternTileKey& other) const {
        return viewKind == other.viewKind
            && startSeconds == other.startSeconds
            && endSeconds == other.endSeconds
            && pixelsPerSecond == other.pixelsPerSecond
            && timeUnit == other.timeUnit
            && tempo == other.tempo
            && timeSigNumerator == other.timeSigNumerator
            && timeSigDenominator == other.timeSigDenominator
            && themeId == other.themeId
            && verticalGeometry == other.verticalGeometry
            && laneStyle == other.laneStyle
            && trackHeight == other.trackHeight;
    }
};

} // namespace OpenTune

namespace std {

template <>
struct hash<OpenTune::PatternTileKey> {
    size_t operator()(const OpenTune::PatternTileKey& k) const {
        size_t h = 0;
        // std::hash combine
        auto combine = [&h](size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        combine(hash<string>{}(k.viewKind));
        combine(hash<double>{}(k.startSeconds));
        combine(hash<double>{}(k.endSeconds));
        combine(hash<double>{}(k.pixelsPerSecond));
        combine(hash<int>{}(k.timeUnit));
        combine(hash<int>{}(k.tempo));
        combine(hash<int>{}(k.timeSigNumerator));
        combine(hash<int>{}(k.timeSigDenominator));
        combine(hash<int>{}(k.themeId));
        // Hash 64-bit verticalGeometry by splitting into two 32-bit parts
        combine(hash<std::uint64_t>{}(k.verticalGeometry));
        combine(hash<int>{}(k.laneStyle));
        combine(hash<int>{}(k.trackHeight));
        return h;
    }
};

} // namespace std

namespace OpenTune {

// ============================================================================
// RenderParams — 传入 TimelineLayerComposer 的渲染参数
// ============================================================================
struct RenderParams {
    double visibleStartSeconds = 0.0;
    double visibleEndSeconds = 0.0;
    double pixelsPerSecond = 100.0;
    int timeUnit = 0;            // 0 = Seconds, 1 = Bars
    int tempo = 120;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    int themeId = 0;
    std::uint64_t verticalGeometry = 0;
    int laneStyle = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    int viewportBoundsX = 0;        // content area start X (e.g. pianoKeyWidth, kArrangementContentStartX)
    int contentOffsetY = 0;         // content tile Y offset (vertically past ruler, e.g. rulerHeight)
    int trackHeight = 100;          // single track height (for arrangement pattern layer)
    std::string viewKind = "pianoroll";  // "pianoroll" / "arrangement"
};

// ============================================================================
// 编码/解码 verticalGeometry 和 laneStyle hash
// verticalGeometry: pixelsPerSemitone(100x, 16bit) | pianoKeyWidth(8bit) | rulerHeight(8bit)
// laneStyle: showLanes(1bit) | scaleRootNote(8bit) | scaleType(8bit)
// 
// For Arrangement: use encodeArrangementVerticalGeometry() which encodes
// contentHeight(16bit) | rulerHeight(8bit) | reserved(8bit)
// ============================================================================
inline std::uint64_t encodeVerticalGeometry(float pixelsPerSemitone, int pianoKeyWidth, int rulerHeight) {
    std::uint64_t h = 0;
    h |= (static_cast<std::uint64_t>(static_cast<int>(pixelsPerSemitone * 100.0f)) & 0xFFFF);
    h |= ((static_cast<std::uint64_t>(pianoKeyWidth) & 0xFF) << 16);
    h |= ((static_cast<std::uint64_t>(rulerHeight) & 0xFF) << 24);
    return h;
}

inline std::uint64_t encodePianoRollVerticalGeometry(float pixelsPerSemitone, int pianoKeyWidth, int rulerHeight,
                                                      float verticalScrollOffset, int contentViewportHeight) {
    std::uint64_t h = 0;
    h |= (static_cast<std::uint64_t>(static_cast<int>(pixelsPerSemitone * 100.0f)) & 0xFFFF);
    h |= ((static_cast<std::uint64_t>(pianoKeyWidth) & 0xFF) << 16);
    h |= ((static_cast<std::uint64_t>(rulerHeight) & 0xFF) << 24);
    h |= ((static_cast<std::uint64_t>(static_cast<int>(verticalScrollOffset)) & 0xFFFF) << 32);
    h |= ((static_cast<std::uint64_t>(contentViewportHeight) & 0xFFFF) << 48);
    return h;
}

// Decode vertical scroll offset from PianoRoll/Pattern geometry (bits 32-47)
inline float decodeVerticalScrollOffset(std::uint64_t verticalGeometry) {
    return static_cast<float>(static_cast<int>((verticalGeometry >> 32) & 0xFFFF));
}

// Decode viewport height from PianoRoll/Pattern geometry (bits 48-63)
inline int decodeViewportHeight(std::uint64_t verticalGeometry) {
    return static_cast<int>((verticalGeometry >> 48) & 0xFFFF);
}

// Arrangement-specific encoding: contentHeight can exceed 255px
inline std::uint64_t encodeArrangementVerticalGeometry(int contentHeight, int rulerHeight) {
    std::uint64_t h = 0;
    h |= (static_cast<std::uint64_t>(contentHeight) & 0xFFFF);           // 16-bit content height (up to 65535px)
    h |= ((static_cast<std::uint64_t>(rulerHeight) & 0xFF) << 16);       // 8-bit ruler height
    // bits 24-63 reserved for future use
    return h;
}

inline int decodeArrangementContentHeight(std::uint64_t verticalGeometry) {
    return static_cast<int>(verticalGeometry & 0xFFFF);  // 16-bit low bits
}

inline int decodeArrangementRulerHeight(std::uint64_t verticalGeometry) {
    return static_cast<int>((verticalGeometry >> 16) & 0xFF);  // 8-bit at bits 16-23
}

inline int encodeLaneStyle(bool showLanes, int scaleRootNote, int scaleType) {
    int h = 0;
    h |= (showLanes ? 1 : 0);
    h |= ((scaleRootNote & 0xFF) << 1);
    h |= ((scaleType & 0xFF) << 9);
    return h;
}

// ============================================================================
// TimelinePatternCache — 固定宽度 pattern tile 缓存
// 所有 pattern 层（ruler/grid/lanes/background）都通过此缓存管理
// 固定 kPatternTileWidthPx = 4096
// ============================================================================
class TimelinePatternCache {
public:
    static constexpr int kPatternTileWidthPx = 4096;

    TimelinePatternCache() = default;
    ~TimelinePatternCache() = default;

    // 确保 tile 可用，如果缺失则通过 buildFn 生成
    // const 方法，使用 mutable 内部状态实现懒生成（memoization）
    const juce::Image& getPatternTile(const PatternTileKey& key,
                                      std::function<juce::Image(const PatternTileKey&)> buildFn) const;

    // 标记 tile 已修改（下次访问需要重建）
    void invalidatePatternTile(const PatternTileKey& key);

    // 清空所有缓存
    void clear();

private:
    struct Tile {
        PatternTileKey key;
        juce::Image image;          // RGB, 大小 = kPatternTileWidthPx × viewportHeight
        bool pendingInvalidate = false;
    };

    mutable std::unordered_map<PatternTileKey, Tile> tiles_;
};

// ============================================================================
// TimelineLayerComposer — 把 pattern tile 绘制到 Graphics
// 所有视图使用同一签名，禁止 view-specific 逻辑
// ============================================================================
class TimelineLayerComposer {
public:
    // 绘制单个 pattern tile
    static void drawPatternTile(
        juce::Graphics& g,
        const juce::Image& tile,
        double cacheStartSeconds,
        const RenderParams& params
    );

    // 绘制 content tile（与 drawPatternTile 共享唯一 blit 公式）
    static void drawContentTile(
        juce::Graphics& g,
        const juce::Image& tile,
        double cacheStartSeconds,
        const RenderParams& params
    );

    // 生成 pattern tile 内容（由 cache 的 buildFn 调用）
    static juce::Image buildPatternTile(const PatternTileKey& key);

private:
    // 内部绘制助手
    static void drawGridLines(juce::Graphics& g, const RenderParams& params);
    static void drawTimeRuler(juce::Graphics& g, const RenderParams& params);
    static void drawLaneStripRepeats(juce::Graphics& g, const RenderParams& params);
    static void drawBackground(juce::Graphics& g, const RenderParams& params);

    static double selectBeatInterval(double pixelsPerBeat);
    static double selectMarkerInterval(double pixelsPerSecond);
};

} // namespace OpenTune

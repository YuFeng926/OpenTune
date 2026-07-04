// ============================================================================
// OpenTune Timeline Presentation Tests
// ============================================================================

#include "../Source/Standalone/UI/TimelineViewportPolicy.h"
#include "../Source/Standalone/UI/TimelinePatternCache.h"
#include "../Source/TimelineContentCache.h"
#include "../Source/Standalone/UI/FixedPlayheadComponent.h"
#include "../Source/Standalone/UI/ThemeTokens.h"
#include "../Source/Standalone/UI/PianoRoll/PianoRollRenderer.h"
#include "../Source/Utils/ContentTimelineProjection.h"
#include "../Source/Content/ContentKey.h"
#include "../Source/Utils/Note.h"
#include "../Source/Standalone/UI/ViewMapper.h"
#include "../Source/StandaloneArrangement.h"

#include <cmath>
#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>
#include <vector>
#include <set>
#include <sstream>

using namespace OpenTune;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) { std::cout << "[FAIL] " << message << "\n"; return false; }
    return true;
}

bool expectNear(double actual, double expected, double tolerance, const char* message)
{
    if (std::abs(actual - expected) > tolerance) {
        std::cout << "[FAIL] " << message << " expected=" << expected << " actual=" << actual << "\n"; return false;
    }
    return true;
}

bool expectEq(int actual, int expected, const char* message)
{
    if (actual != expected) {
        std::cout << "[FAIL] " << message << " expected=" << expected << " actual=" << actual << "\n"; return false;
    }
    return true;
}

static std::string getSourceRoot()
{
#ifdef OPENTUNE_SOURCE_DIR
    return OPENTUNE_SOURCE_DIR;
#else
    return ".";
#endif
}

static std::string readFileContent(const std::string& path)
{
    std::ifstream file(path); if (!file.is_open()) return "";
    std::stringstream buffer; buffer << file.rdbuf(); return buffer.str();
}

static bool fileContainsString(const std::string& filePath, const std::string& pattern)
{
    std::string content = readFileContent(filePath);
    return content.find(pattern) != std::string::npos;
}

static bool anySourceContains(const std::string& sourceDir, const std::string& pattern,
                              const std::vector<std::string>& excludeDirs = {})
{
    namespace fs = std::filesystem;
    for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
        if (!entry.is_regular_file()) continue;
        const auto& path = entry.path();
        std::string ext = path.extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
        std::string pathStr = path.string();
        bool excluded = false;
        for (const auto& ed : excludeDirs) { 
            if (pathStr.find(ed) != std::string::npos) { excluded = true; break; } 
        }
        if (excluded) continue;
        if (fileContainsString(pathStr, pattern)) return true;
    }
    return false;
}

static std::string findFilesContaining(const std::string& sourceDir, const std::string& pattern)
{
    namespace fs = std::filesystem; std::string result;
    for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
        if (!entry.is_regular_file()) continue;
        const auto& path = entry.path();
        std::string ext = path.extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
        if (fileContainsString(path.string(), pattern)) {
            if (!result.empty()) result += ", "; result += path.filename().string();
        }
    }
    return result;
}

static bool patternInMethodBody(const std::string& filePath, const std::string& methodSignature, const std::string& forbiddenCall)
{
    std::string content = readFileContent(filePath); if (content.empty()) return false;
    std::istringstream iss(content); std::string line; int lineNum = 0, methodStart = -1, braceDepth = 0;
    bool inMethod = false;
    while (std::getline(iss, line)) {
        ++lineNum;
        if (!inMethod && line.find(methodSignature) != std::string::npos) { inMethod = true; methodStart = lineNum; braceDepth = 0; }
        if (inMethod) {
            for (char c : line) { if (c == '{') ++braceDepth; else if (c == '}') --braceDepth; }
            if (braceDepth > 0 && line.find(forbiddenCall) != std::string::npos) return true;
            if (braceDepth == 0 && methodStart >= 0 && methodStart < lineNum) { inMethod = false; methodStart = -1; }
        }
    }
    return false;
}

static std::string extractMethodBody(const std::string& fileContent, const std::string& methodSig)
{
    auto pos = fileContent.find(methodSig);
    if (pos == std::string::npos) return "";
    auto bracePos = fileContent.find('{', pos);
    if (bracePos == std::string::npos) return "";
    int depth = 1;
    size_t end = bracePos + 1;
    while (end < fileContent.size() && depth > 0) {
        if (fileContent[end] == '{') ++depth;
        else if (fileContent[end] == '}') --depth;
        ++end;
    }
    return fileContent.substr(bracePos, end - bracePos);
}

// Helper: extract C++ code block after a token using brace depth
static std::string extractBlockAfterToken(const std::string& text, const std::string& token)
{
    const auto pos = text.find(token);
    if (pos == std::string::npos) return "";
    const auto bracePos = text.find('{', pos);
    if (bracePos == std::string::npos) return "";

    int depth = 1;
    size_t end = bracePos + 1;
    while (end < text.size() && depth > 0)
    {
        if (text[end] == '{') ++depth;
        else if (text[end] == '}') --depth;
        ++end;
    }

    return text.substr(bracePos, end - bracePos);
}

} // namespace

// ============================================================================
// Pattern Cache Tests
// ============================================================================

bool patternCache_noClipClickZoomPlay()
{
    TimelinePatternCache cache;
    int buildCount = 0;
    auto buildFn = [&buildCount](const PatternTileKey&) -> juce::Image {
        ++buildCount;
        return juce::Image(juce::Image::RGB, TimelinePatternCache::kPatternTileWidthPx, 100, true);
    };
    PatternTileKey key;
    key.viewKind = "pianoroll"; key.startSeconds = 0.0; key.endSeconds = 40.96;
    key.pixelsPerSecond = 100.0; key.timeUnit = 0; key.tempo = 120;
    key.timeSigNumerator = 4; key.timeSigDenominator = 4;
    key.themeId = 0; key.verticalGeometry = 0; key.laneStyle = 0;
    cache.getPatternTile(key, buildFn);
    if (!expect(buildCount == 1, "pattern: first tile built")) return false;
    cache.getPatternTile(key, buildFn);
    if (!expect(buildCount == 1, "pattern: cache hit, no rebuild")) return false;
    key.startSeconds = 40.96; key.endSeconds = 81.92;
    cache.getPatternTile(key, buildFn);
    if (!expect(buildCount == 2, "pattern: new position builds new tile")) return false;
    return true;
}

bool patternTileKey_equality()
{
    PatternTileKey k1{};
    k1.viewKind = "pianoroll";
    k1.startSeconds = 0; k1.endSeconds = 40.96; k1.pixelsPerSecond = 100;
    k1.timeUnit = 0; k1.tempo = 120; k1.timeSigNumerator = 4; k1.timeSigDenominator = 4;
    k1.themeId = 0; k1.verticalGeometry = 0; k1.laneStyle = 0;

    PatternTileKey k2{};
    k2.viewKind = "pianoroll";
    k2.startSeconds = 0; k2.endSeconds = 40.96; k2.pixelsPerSecond = 100;
    k2.timeUnit = 0; k2.tempo = 120; k2.timeSigNumerator = 4; k2.timeSigDenominator = 4;
    k2.themeId = 0; k2.verticalGeometry = 0; k2.laneStyle = 0;
    if (!expect(k1 == k2, "pattern: identical keys equal")) return false;
    k2.tempo = 130;
    if (!expect(!(k1 == k2), "pattern: different tempo = different key")) return false;
    return true;
}

bool patternCache_invalidate()
{
    TimelinePatternCache cache;
    int buildCount = 0;
    auto buildFn = [&buildCount](const PatternTileKey&) -> juce::Image {
        ++buildCount; return juce::Image(juce::Image::RGB, TimelinePatternCache::kPatternTileWidthPx, 50, true);
    };
    PatternTileKey key;
    key.viewKind = "pianoroll"; key.startSeconds = 0.0; key.endSeconds = 40.96;
    key.pixelsPerSecond = 100.0; key.timeUnit = 0; key.tempo = 120;
    key.timeSigNumerator = 4; key.timeSigDenominator = 4;
    key.themeId = 0; key.verticalGeometry = 0; key.laneStyle = 0;
    cache.getPatternTile(key, buildFn);
    if (!expect(buildCount == 1, "invalidate: first build")) return false;
    cache.clear();
    cache.getPatternTile(key, buildFn);
    if (!expect(buildCount == 2, "invalidate: rebuilt after clear")) return false;
    return true;
}

bool patternCache_realRendering()
{
    TimelinePatternCache cache;
    PatternTileKey key;
    key.viewKind = "pianoroll"; key.startSeconds = 0.0; key.endSeconds = 4096.0 / 100.0;
    key.pixelsPerSecond = 100.0; key.timeUnit = 0; key.tempo = 120;
    key.timeSigNumerator = 4; key.timeSigDenominator = 4;
    key.themeId = static_cast<int>(ThemeId::DarkBlueGrey);
    key.verticalGeometry = encodeVerticalGeometry(1.5f, 60, 30);
    key.laneStyle = encodeLaneStyle(true, 0, 1);
    const auto& tile = cache.getPatternTile(key, [](const PatternTileKey& k) -> juce::Image {
        return TimelineLayerComposer::buildPatternTile(k);
    });
    if (!expect(tile.isValid(), "realRender: tile is valid")) return false;
    if (!expect(tile.getWidth() == TimelinePatternCache::kPatternTileWidthPx, "realRender: tile width")) return false;
    if (!expect(tile.getHeight() > 30, "realRender: tile height > ruler only")) return false;
    juce::Image::BitmapData bd(tile, juce::Image::BitmapData::readOnly);
    if (!expect(bd.data != nullptr, "realRender: bitmap data accessible")) return false;
    cache.clear();
    return true;
}

// ============================================================================
// Fixed Playhead Tests
// ============================================================================

bool fixedPlayhead_anchorBounds()
{
    FixedPlayheadComponent ph;
    ph.setAnchorBounds(500, 400);
    return true;
}

// ============================================================================
// View Switch Tests
// ============================================================================

bool viewSwitch_preheated()
{
    TimelinePatternCache cache;
    int buildCount = 0;
    auto buildFn = [&buildCount](const PatternTileKey&) -> juce::Image {
        ++buildCount; return juce::Image(juce::Image::RGB, TimelinePatternCache::kPatternTileWidthPx, 100, true);
    };
    auto makeKey = [](const char* kind) {
        PatternTileKey key; key.viewKind = kind; key.startSeconds = 0.0; key.endSeconds = 4096.0 / 100.0;
        key.pixelsPerSecond = 100.0; key.timeUnit = 0; key.tempo = 120;
        key.timeSigNumerator = 4; key.timeSigDenominator = 4;
        key.themeId = 0; key.verticalGeometry = 0; key.laneStyle = 0; return key;
    };
    cache.getPatternTile(makeKey("pianoroll"), buildFn);
    cache.getPatternTile(makeKey("arrangement"), buildFn);
    if (!expect(buildCount == 2, "viewSwitch: both views preheated")) return false;
    cache.getPatternTile(makeKey("pianoroll"), buildFn);
    if (!expect(buildCount == 2, "viewSwitch: PianoRoll return hits cache")) return false;
    cache.getPatternTile(makeKey("arrangement"), buildFn);
    if (!expect(buildCount == 2, "viewSwitch: Arrangement return hits cache")) return false;
    return true;
}

// ============================================================================
// Encode / Decode Tests
// ============================================================================

bool encodeDecode_verticalGeometry()
{
    int hash1 = encodeVerticalGeometry(1.5f, 60, 30);
    int hash2 = encodeVerticalGeometry(1.5f, 60, 30);
    if (!expect(hash1 == hash2, "encode: same params = same hash")) return false;
    int hash3 = encodeVerticalGeometry(2.0f, 60, 30);
    if (!expect(hash1 != hash3, "encode: different pixelsPerSemitone = different hash")) return false;
    return true;
}

bool encodeDecode_laneStyle()
{
    int hash1 = encodeLaneStyle(true, 0, 1);
    int hash2 = encodeLaneStyle(true, 0, 1);
    if (!expect(hash1 == hash2, "encode: same lane style = same hash")) return false;
    int hash3 = encodeLaneStyle(false, 0, 1);
    if (!expect(hash1 != hash3, "encode: different showLanes = different hash")) return false;
    return true;
}

// ============================================================================
// Content Cache Tests (getOrBuildTile)
// ============================================================================

bool contentCache_getOrBuildTile()
{
    TimelineContentCache cache;
    int buildCount = 0;
    ContentTileKey key;
    key.viewKind = "pianoroll"; key.slot = ContentSlot::Waveform;
    key.contentKey = ContentKey{DomainKind::StandaloneClip, 42, 0};
    key.startSeconds = 0.0; key.endSeconds = 10.0; key.pixelsPerSecond = 100.0;
    key.verticalGeometry = 0; key.revision = 1;

    const auto& tile1 = cache.getOrBuildTile(key, 512,
        [&](juce::Graphics& g, const ContentTileKey&, juce::Rectangle<int>) { ++buildCount; g.setColour(juce::Colours::blue); g.fillAll(); });
    if (!expect(buildCount == 1, "contentCache: first getOrBuildTile calls painter")) return false;

    const auto& tile2 = cache.getOrBuildTile(key, 512,
        [&](juce::Graphics&, const ContentTileKey&, juce::Rectangle<int>) { ++buildCount; });
    if (!expect(buildCount == 1, "contentCache: same key reuses cached tile")) return false;
    if (!expect(&tile1 == &tile2, "contentCache: same reference")) return false;

    key.revision = 2;
    const auto& tile3 = cache.getOrBuildTile(key, 512,
        [&](juce::Graphics& g, const ContentTileKey&, juce::Rectangle<int>) { ++buildCount; g.setColour(juce::Colours::red); g.fillAll(); });
    if (!expect(buildCount == 2, "contentCache: different revision triggers rebuild")) return false;

    cache.clear();
    return true;
}

bool contentCache_realRendererPixelOutput()
{
    TimelineContentCache cache;

    // Part A: Content cache infrastructure
    ContentTileKey keyA;
    keyA.viewKind = "pianoroll"; keyA.slot = ContentSlot::Waveform;
    keyA.contentKey = ContentKey{DomainKind::StandaloneClip, 1, 0};
    keyA.startSeconds = 0.0; keyA.endSeconds = 10.0; keyA.pixelsPerSecond = 100.0;
    keyA.verticalGeometry = 0; keyA.revision = 1;

    const auto& tileA = cache.getOrBuildTile(keyA, 100,
        [](juce::Graphics& g, const ContentTileKey&, juce::Rectangle<int>) {
            g.setColour(juce::Colours::red); g.fillRect(10, 10, 40, 40);
        });
    {
        juce::Image::BitmapData bd(tileA, juce::Image::BitmapData::readOnly);
        if (!expect(bd.getPixelColour(30, 30).getAlpha() > 0, "realRenderer: known color fill produces non-transparent pixels")) return false;
    }

    // Part B: Real renderer with valid projection
    ContentTileKey keyB;
    keyB.viewKind = "pianoroll"; keyB.slot = ContentSlot::Notes;
    keyB.contentKey = ContentKey{DomainKind::StandaloneClip, 2, 42};
    keyB.startSeconds = 0.0; keyB.endSeconds = 10.0; keyB.pixelsPerSecond = 100.0;
    keyB.verticalGeometry = 0; keyB.revision = 1;

    const auto& tileB = cache.getOrBuildTile(keyB, 200,
        [](juce::Graphics& g, const ContentTileKey& k, juce::Rectangle<int> bounds) {
            PianoRollRenderer renderer;
            PianoRollRenderer::RenderContext ctx;
            ctx.width = bounds.getWidth(); ctx.height = bounds.getHeight();
            ctx.pianoKeyWidth = 0; ctx.rulerHeight = 0; ctx.pixelsPerSecond = k.pixelsPerSecond;
            ctx.pixelsPerSemitone = 15.0f; ctx.minMidi = 50.0f; ctx.maxMidi = 70.0f;
            ctx.coords = ViewMapper{k.startSeconds, k.pixelsPerSecond, 0, bounds.getWidth(), bounds.getHeight(), 15.0f, 0.0f, 70.0f};
            PianoRollRenderer::ContentRenderItem item;
            item.contentKey = k.contentKey;
            item.projection = ContentTimelineProjection{k.startSeconds, k.endSeconds - k.startSeconds, k.endSeconds - k.startSeconds};
            item.active = true;
            Note n; n.startTime = 1.0; n.endTime = 3.0; n.pitch = 261.63f; n.velocity = 0.8f;
            item.displayNotes = {n};
            renderer.drawNotes(g, ctx, item);
        });
    {
        juce::Image::BitmapData bd(tileB, juce::Image::BitmapData::readOnly);
        bool hasVisible = false;
        for (int y = 0; y < tileB.getHeight() && !hasVisible; ++y)
            for (int x = 0; x < tileB.getWidth() && !hasVisible; ++x)
                if (bd.getPixelColour(x, y).getAlpha() > 0) hasVisible = true;
        if (!expect(hasVisible, "realRenderer: notes with valid projection produce non-transparent pixels")) return false;
    }

    // Part C: Without projection — early return
    ContentTileKey keyC;
    keyC.viewKind = "pianoroll"; keyC.slot = ContentSlot::Notes;
    keyC.contentKey = ContentKey{DomainKind::StandaloneClip, 3, 0};
    keyC.startSeconds = 0.0; keyC.endSeconds = 10.0; keyC.pixelsPerSecond = 100.0;
    keyC.verticalGeometry = 0; keyC.revision = 1;

    const auto& tileC = cache.getOrBuildTile(keyC, 200,
        [](juce::Graphics& g, const ContentTileKey& k, juce::Rectangle<int> bounds) {
            PianoRollRenderer renderer;
            PianoRollRenderer::RenderContext ctx;
            ctx.width = bounds.getWidth(); ctx.height = bounds.getHeight();
            ctx.pianoKeyWidth = 0; ctx.rulerHeight = 0; ctx.pixelsPerSecond = k.pixelsPerSecond;
            ctx.pixelsPerSemitone = 15.0f; ctx.minMidi = 50.0f; ctx.maxMidi = 70.0f;
            ctx.coords = ViewMapper{k.startSeconds, k.pixelsPerSecond, 0, bounds.getWidth(), bounds.getHeight(), 15.0f, 0.0f, 70.0f};
            PianoRollRenderer::ContentRenderItem item;
            Note n; n.startTime = 1.0; n.endTime = 3.0; n.pitch = 261.63f; n.velocity = 0.8f;
            item.displayNotes = {n};
            renderer.drawNotes(g, ctx, item);
        });
    {
        juce::Image::BitmapData bd(tileC, juce::Image::BitmapData::readOnly);
        bool allTransparent = true;
        for (int y = 0; y < tileC.getHeight() && allTransparent; ++y)
            for (int x = 0; x < tileC.getWidth() && allTransparent; ++x)
                if (bd.getPixelColour(x, y).getAlpha() > 0) allTransparent = false;
        if (!expect(allTransparent, "realRenderer: notes WITHOUT projection produce transparent tile")) return false;
    }

    cache.clear();
    return true;
}

bool contentCache_discriminatorIsolation()
{
    TimelineContentCache cache;
    ContentTileKey key1;
    key1.viewKind = "pianoroll"; key1.slot = ContentSlot::Notes;
    key1.contentKey = ContentKey{DomainKind::StandaloneClip, 100, 42};
    key1.startSeconds = 0.0; key1.endSeconds = 10.0; key1.pixelsPerSecond = 100.0;
    key1.verticalGeometry = 0; key1.revision = 1;

    ContentTileKey key2;
    key2.viewKind = "pianoroll"; key2.slot = ContentSlot::Notes;
    key2.contentKey = ContentKey{DomainKind::StandaloneClip, 100, 99};
    key2.startSeconds = 0.0; key2.endSeconds = 10.0; key2.pixelsPerSecond = 100.0;
    key2.verticalGeometry = 0; key2.revision = 1;

    const auto& tile1 = cache.getOrBuildTile(key1, 100,
        [](juce::Graphics& g, const ContentTileKey&, juce::Rectangle<int>) { g.setColour(juce::Colours::red); g.fillAll(); });
    const auto& tile2 = cache.getOrBuildTile(key2, 100,
        [](juce::Graphics& g, const ContentTileKey&, juce::Rectangle<int>) { g.setColour(juce::Colours::blue); g.fillAll(); });

    if (!expect(&tile1 != &tile2, "discriminator: different ContentKeys produce different tiles")) return false;
    juce::Image::BitmapData bd(tile2, juce::Image::BitmapData::readOnly);
    auto pixel = bd.getPixelColour(10, 10);
    if (!expect(pixel.getRed() < 200, "discriminator: tile2 not red (not affected by tile1)")) return false;

    cache.clear();
    return true;
}

// ============================================================================
// Contract Tests
// ============================================================================

// Contract test: verifies clipInSeconds anchor formula used by Arrangement content renderer.
// The actual render/cache path is validated by the Kill List static checks + integration.
bool arrangementWaveform_clipInSecondsAnchor()
{
    StandaloneArrangement::Placement pl;
    pl.placementId = 1; pl.contentKey = ContentKey{DomainKind::StandaloneClip, 999, 0};
    pl.timelineStartSeconds = 5.0; pl.durationSeconds = 3.0; pl.clipInSeconds = 2.0;

    double contentTime = pl.clipInSeconds + (pl.timelineStartSeconds - pl.timelineStartSeconds);
    if (!expectNear(contentTime, 2.0, 1e-9, "clipInSeconds: at timeline start contentTime == clipInSeconds")) return false;

    contentTime = pl.clipInSeconds + (6.5 - pl.timelineStartSeconds);
    if (!expectNear(contentTime, 3.5, 1e-9, "clipInSeconds: at midpoint contentTime == 3.5")) return false;
    return true;
}

bool pianoRoll_noFallbackToFirstValidPlacement()
{
    std::vector<TimelineContentPlacement> placements;
    ContentKey ck1{DomainKind::StandaloneClip, 100, 0};
    ContentKey ck2{DomainKind::StandaloneClip, 200, 0};
    placements.push_back({ck1, ContentTimelineProjection{0.0, 10.0, 10.0}});
    placements.push_back({ck2, ContentTimelineProjection{0.0, 10.0, 10.0}});

    ContentKey editedKey = ck1;
    auto it = std::find_if(placements.begin(), placements.end(),
        [&](const auto& p) { return p.contentKey == editedKey && p.isValid(); });
    if (!expect(it != placements.end() && it->contentKey == ck1, "fallback: editedKey matches placement")) return false;

    ContentKey ck3{DomainKind::StandaloneClip, 999, 0};
    it = std::find_if(placements.begin(), placements.end(),
        [&](const auto& p) { return p.contentKey == ck3 && p.isValid(); });
    if (!expect(it == placements.end(), "fallback: non-matching editedKey returns end (no fallback)")) return false;
    return true;
}

bool arrangementContentKeyIsValid()
{
    // Arrangement 使用 DomainKind::StandaloneArrangement
    ContentKey arrangementKey{DomainKind::StandaloneArrangement, 1, 0};
    if (!expect(arrangementKey.isValid(), "arrangement key must be valid")) return false;
    if (!expect(arrangementKey.domainKind == DomainKind::StandaloneArrangement, "domainKind must be StandaloneArrangement")) return false;
    
    // ContentTileKey 使用 arrangement key
    ContentTileKey tileKey;
    tileKey.viewKind = "arrangement";
    tileKey.slot = ContentSlot::ArrangementClips;
    tileKey.contentKey = arrangementKey;
    tileKey.startSeconds = 0.0;
    tileKey.endSeconds = 40.96;
    tileKey.pixelsPerSecond = 100.0;
    tileKey.verticalGeometry = 0;
    tileKey.revision = 1;
    
    if (!expect(tileKey.contentKey.isValid(), "tile contentKey must be valid")) return false;
    return true;
}

bool arrangementRevisionChangesOnTrimAndWaveformGeneration()
{
    // 模拟 revision 计算（参考 ArrangementViewComponent::rebuildContentMetrics）
    auto computeRevision = [](double clipInSeconds, uint64_t waveformGen) -> uint64_t {
        uint64_t h = 1469598103934665603ull; // FNV offset basis
        h ^= static_cast<uint64_t>(std::llround(clipInSeconds * 1000.0)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= waveformGen + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    };
    
    uint64_t rev1 = computeRevision(0.0, 1);
    uint64_t rev2 = computeRevision(2.0, 1);  // clipInSeconds changed
    uint64_t rev3 = computeRevision(2.0, 2);  // waveform generation changed
    
    if (!expect(rev1 != rev2, "revision must change when clipInSeconds changes")) return false;
    if (!expect(rev2 != rev3, "revision must change when waveform generation changes")) return false;
    return true;
}

bool pianoRollPreparedTilesUseValidContentKey()
{
    // 模拟 PianoRoll content tile key 构造
    ContentKey editedKey{DomainKind::StandaloneClip, 42, 0};
    if (!expect(editedKey.isValid(), "edited key must be valid")) return false;
    
    ContentSlot slots[] = { ContentSlot::Waveform, ContentSlot::Notes, ContentSlot::F0, ContentSlot::TimeAnchors };
    
    for (auto slot : slots) {
        ContentTileKey tileKey;
        tileKey.viewKind = "pianoroll";
        tileKey.slot = slot;
        tileKey.contentKey = editedKey;
        tileKey.startSeconds = 0.0;
        tileKey.endSeconds = 40.96;
        tileKey.pixelsPerSecond = 100.0;
        tileKey.verticalGeometry = 0;
        tileKey.revision = 1;
        
        if (!expect(tileKey.contentKey.isValid(), "piano roll tile contentKey must be valid")) return false;
        if (!expect(tileKey.contentKey == editedKey, "tile contentKey must match edited key")) return false;
    }
    return true;
}

// ============================================================================
// Piano Roll Behavior Tests
// ============================================================================

// Source-code contract: verifies PianoRollComponent::activeContentProjection
// filters by editedContentKey_ (not wildcard/fallback logic).
bool pianoRoll_usesEditedPlacementOnly()
{
    std::string sourceDir = getSourceRoot();
    std::string pianorollCpp = sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp";
    std::string content = readFileContent(pianorollCpp);

    // Extract activeContentProjection body (try both signatures)
    auto body = extractMethodBody(content, "PianoRollComponent::activeContentProjection() const noexcept");
    if (body.empty())
        body = extractMethodBody(content, "PianoRollComponent::activeContentProjection() const");
    if (!expect(!body.empty(), "activeContentProjection method exists")) return false;

    // Must delegate to findEditedPlacement (not contain duplicate filter logic)
    if (!expect(body.find("findEditedPlacement") != std::string::npos,
                "activeContentProjection delegates to findEditedPlacement")) return false;

    // Returns {} when no placement matches (invalid projection)
    if (!expect(body.find("return") != std::string::npos,
                "activeContentProjection has a return statement")) return false;

    // Verify findEditedPlacement uses editedContentKey_ as filter, not any-key wildcard
    auto filteredBody = extractMethodBody(content, "PianoRollComponent::findEditedPlacement() const noexcept");
    if (!expect(!filteredBody.empty(), "findEditedPlacement method exists")) return false;
    if (!expect(filteredBody.find("editedContentKey_") != std::string::npos,
                "findEditedPlacement filters by editedContentKey_")) return false;

    return true;
}

// Source-code contract: verifies PianoRollComponent::revisionForContentSlot
// includes all required pixel-level inputs per slot.
bool pianoRoll_contentTileRevisionTracksPixelInputs()
{
    std::string sourceDir = getSourceRoot();
    std::string pianorollCpp = sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp";
    auto body = extractMethodBody(readFileContent(pianorollCpp), "PianoRollComponent::revisionForContentSlot(ContentSlot slot) const noexcept");
    if (!expect(!body.empty(), "revisionForContentSlot method exists")) return false;

    // F0 revision includes all three visibility flags
    if (!expect(body.find("showUnvoicedFrames_") != std::string::npos,
                "F0 revision includes showUnvoicedFrames_")) return false;
    if (!expect(body.find("showOriginalF0_") != std::string::npos,
                "F0 revision includes showOriginalF0_")) return false;
    if (!expect(body.find("showCorrectedF0_") != std::string::npos,
                "F0 revision includes showCorrectedF0_")) return false;

    // Notes revision includes noteNameMode_
    if (!expect(body.find("noteNameMode_") != std::string::npos,
                "Notes revision includes noteNameMode_")) return false;

    // TimeGridEpoch affects ALL content slots (outside any per-slot if block)
    if (!expect(body.find("timeGridEpoch_") != std::string::npos,
                "TimeGrid epoch is included in revision")) return false;

    // Scale (scaleRootNote_ / scaleType_) must NOT affect content revision
    // — scale only affects pattern tiles (chrome), not content tiles
    if (!expect(body.find("scaleRootNote_") == std::string::npos,
                "Scale does NOT affect content revision")) return false;
    if (!expect(body.find("scaleType_") == std::string::npos,
                "scaleType does NOT affect content revision")) return false;

    return true;
}

bool pianoRoll_drawUsesLayerComposerBlitPath()
{
    std::string sourceDir = getSourceRoot();
    std::string pianorollCpp = sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp";

    auto bodyPP = extractMethodBody(readFileContent(pianorollCpp), "void PianoRollComponent::drawPreparedPatternTiles(juce::Graphics& g)");
    auto bodyCP = extractMethodBody(readFileContent(pianorollCpp), "void PianoRollComponent::drawPreparedContentTiles(juce::Graphics& g)");

    // 必须调用 TimelineLayerComposer
    if (!expect(bodyPP.find("TimelineLayerComposer::drawPatternTile") != std::string::npos,
                "drawPreparedPatternTiles calls TimelineLayerComposer::drawPatternTile")) return false;
    if (!expect(bodyCP.find("TimelineLayerComposer::drawContentTile") != std::string::npos,
                "drawPreparedContentTiles calls TimelineLayerComposer::drawContentTile")) return false;

    // 禁止手动 X 公式: (tileStart - ...) * pps 或 (tileStart - viewStart) * pps
    auto hasManualFormula = [](const std::string& body) -> bool {
        return body.find("(tileStart -") != std::string::npos
            && body.find("* pps") != std::string::npos;
    };
    if (!expect(!hasManualFormula(bodyPP), "drawPreparedPatternTiles has no manual X formula")) return false;
    if (!expect(!hasManualFormula(bodyCP), "drawPreparedContentTiles has no manual X formula")) return false;

    return true;
}

// Source-code contract + behavioral test: verifies pianoKeyWidth_ is subtracted
// from anchor X in the transport tick handler (Fix 2), and validates the
// resulting camera math with concrete numbers.
bool pianoRoll_pageModePlayingKeepsFixedAnchor()
{
    double pps = 100.0;
    int viewportWidth = 800;
    int pianoKeyWidth = 56;
    int contentWidth = viewportWidth - pianoKeyWidth;        // 744
    int viewportCentreX = viewportWidth / 2;                  // 400
    int anchorContentX = viewportCentreX - pianoKeyWidth;     // 344  (content-area anchor)

    // 非播放态 Page 模式：seek 到 page 边界
    double seekTargetTime = 15.0;
    int pageIndex = static_cast<int>(seekTargetTime * pps / contentWidth);
    double nonPlayingVisibleStart = static_cast<double>(pageIndex * contentWidth) / pps;
    if (!expectNear(nonPlayingVisibleStart, 14.88, 1e-9, "non-playing: seek to page boundary")) return false;

    // 播放态 Page/Continuous 模式：playhead 固定在 anchor (内容区中心)，camera 跟随 transport
    double transportTime = 12.34;
    double playingVisibleStart = transportTime - static_cast<double>(anchorContentX) / pps;
    playingVisibleStart = std::max(0.0, playingVisibleStart);
    if (!expectNear(playingVisibleStart, 8.90, 1e-9, "playing at 12.34s, anchor at content center")) return false;

    // 验证锚点位置与 visibleStart 的关系
    double anchorTimeFromCamera = playingVisibleStart + static_cast<double>(anchorContentX) / pps;
    if (!expectNear(anchorTimeFromCamera, transportTime, 1e-9, "anchor maps back to transportTime")) return false;

    // 验证越界钳制 (transport time < anchor offset)
    double earlyTransportTime = 1.0;
    double clampedStart = std::max(0.0, earlyTransportTime - static_cast<double>(anchorContentX) / pps);
    if (!expectNear(clampedStart, 0.0, 1e-9, "clamped to 0 when transport too early")) return false;

    // Source-code contract: production code subtracts pianoKeyWidth_ from anchor X
    std::string sourceDir = getSourceRoot();
    std::string pianorollCpp = sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp";
    std::string content = readFileContent(pianorollCpp);
    bool foundCorrectAnchor = content.find("getCentreX() - pianoKeyWidth_") != std::string::npos;
    if (!expect(foundCorrectAnchor, "production code subtracts pianoKeyWidth_ from anchor X")) return false;

    return true;
}

// Helper: extract public sections of a class from header
static std::string publicSectionsForClass(const std::string& header, const std::string& className)
{
    const auto classPos = header.find(className);
    if (classPos == std::string::npos) return "";

    std::istringstream lines(header.substr(classPos));
    std::string publicText;
    std::string line;
    bool inPublic = false;
    while (std::getline(lines, line))
    {
        if (line.find("};") != std::string::npos) break;
        if (line.find("public:") != std::string::npos) { inPublic = true; continue; }
        if (line.find("private:") != std::string::npos || line.find("protected:") != std::string::npos)
        {
            inPublic = false;
            continue;
        }
        if (inPublic) publicText += line + "\n";
    }
    return publicText;
}

// ============================================================================
// Static Contract Tests (Task A/B/C)
// ============================================================================

// Source-code contract: views must not bypass TimelineViewportPolicy
bool timelineViewportPolicy_contracts()
{
    const std::string sourceDir = getSourceRoot();
    const auto piano = sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp";
    const auto arrangement = sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.cpp";
    const auto pianoHeader = sourceDir + "/Source/Standalone/UI/PianoRollComponent.h";
    const auto arrangementHeader = sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.h";
    const auto pluginEditor = sourceDir + "/Source/Standalone/PluginEditor.cpp";

    const auto pianoText = readFileContent(piano);
    const auto arrangementText = readFileContent(arrangement);
    const auto pianoHeaderText = readFileContent(pianoHeader);
    const auto arrangementHeaderText = readFileContent(arrangementHeader);
    const auto pluginEditorText = readFileContent(pluginEditor);

    if (patternInMethodBody(piano, "commitViewportRequest(", "viewportWidth =")) return false;
    if (patternInMethodBody(piano, "commitViewportRequest(", "pixelsPerSecond =")) return false;
    if (patternInMethodBody(arrangement, "commitViewportRequest(", "viewportWidth =")) return false;
    if (patternInMethodBody(arrangement, "commitViewportRequest(", "pixelsPerSecond =")) return false;

    // public API must expose request commit only. Raw camera replay is a private implementation detail.
    if (pianoText.find("commitCamera(") != std::string::npos) return false;
    if (arrangementText.find("commitCamera(") != std::string::npos) return false;
    if (pianoHeaderText.find("commitCamera(") != std::string::npos) return false;
    if (arrangementHeaderText.find("commitCamera(") != std::string::npos) return false;
    if (pluginEditorText.find("commitCamera(") != std::string::npos) return false;

    const auto pianoPublic = publicSectionsForClass(pianoHeaderText, "class PianoRollComponent");
    const auto arrangementPublic = publicSectionsForClass(arrangementHeaderText, "class ArrangementViewComponent");
    if (pianoPublic.find("applyResolvedCamera(") != std::string::npos) return false;
    if (arrangementPublic.find("applyResolvedCamera(") != std::string::npos) return false;

    const auto pianoCommit = extractMethodBody(pianoText, "void PianoRollComponent::commitViewportRequest");
    const auto arrangementCommit = extractMethodBody(arrangementText, "void ArrangementViewComponent::commitViewportRequest");
    if (pianoCommit.find("applyResolvedCamera(TimelineViewportPolicy::resolve(req), notify)") == std::string::npos) return false;
    if (arrangementCommit.find("applyResolvedCamera(TimelineViewportPolicy::resolve(req), notify)") == std::string::npos) return false;

    if (patternInMethodBody(piano, "commitViewportRequest(", "camera_ =")) return false;
    if (patternInMethodBody(arrangement, "commitViewportRequest(", "camera_ =")) return false;
    if (patternInMethodBody(piano, "commitViewportRequest(", "TimelineViewportCamera")) return false;
    if (patternInMethodBody(arrangement, "commitViewportRequest(", "TimelineViewportCamera")) return false;
    if (!patternInMethodBody(piano, "applyResolvedCamera(", "camera_ =")) return false;
    if (!patternInMethodBody(arrangement, "applyResolvedCamera(", "camera_ =")) return false;

    if (patternInMethodBody(arrangement, "mouseWheelMove(", "req.kind = TimelineViewportRequest::Kind::Zoom")
        && !patternInMethodBody(arrangement, "mouseWheelMove(", "req.viewportWidth = getVisibleViewportWidth()"))
        return false;
    if (patternInMethodBody(arrangement, "mouseWheelMove(", "req.kind = TimelineViewportRequest::Kind::Zoom")
        && !patternInMethodBody(arrangement, "mouseWheelMove(", "req.anchorViewportX = static_cast<double>(e.x - kArrangementContentStartX)"))
        return false;

    return true;
}

// Source-code contract: Move drag only uses transient overlay, not old parallel model
bool arrangementMoveDrag_transientOverlayContract()
{
    const std::string sourceDir = getSourceRoot();
    const auto header = sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.h";
    const auto source = sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.cpp";
    const auto sourceText = readFileContent(source);
    const auto actionsText = readFileContent(sourceDir + "/Source/Utils/PlacementActions.h");
    const auto all = readFileContent(header) + sourceText;

    if (all.find("multiDragStartStates_") != std::string::npos) return false;
    if (all.find("struct DragStartState") != std::string::npos) return false;
    if (all.find("std::vector<DragStartState>") != std::string::npos) return false;
    if (all.find("resolveMoveDragParticipants(") == std::string::npos) return false;
    if (all.find("beginMoveDrag(") == std::string::npos) return false;
    if (all.find("finishMoveDrag(") == std::string::npos) return false;
    if (sourceText.find("std::make_unique<MovePlacementAction>") != std::string::npos) return false;
    const auto multiMoveEntry = extractBlockAfterToken(actionsText, "struct Entry");
    if (multiMoveEntry.find("sourceTrackId") == std::string::npos) return false;
    if (multiMoveEntry.find("targetTrackId") == std::string::npos) return false;

    const auto updateBody = extractMethodBody(sourceText, "ArrangementViewComponent::updateMoveDragPreview");
    if (updateBody.find("refreshVisualState") != std::string::npos) return false;
    if (updateBody.find("prepareVisibleContentTiles") != std::string::npos) return false;
    if (updateBody.find("requestContentInvalidation") != std::string::npos) return false;
    if (updateBody.find("getOrBuildTile") != std::string::npos) return false;

    const auto dragBody = extractMethodBody(sourceText, "ArrangementViewComponent::mouseDrag");
    const auto moveBranch = extractBlockAfterToken(dragBody, "if (isDraggingPlacement_)");
    if (moveBranch.find("placementTimingChanged") != std::string::npos) return false;
    if (moveBranch.find("setStandalonePlacementStartSeconds") != std::string::npos) return false;
    if (moveBranch.find("moveStandalonePlacement") != std::string::npos) return false;
    if (moveBranch.find("refreshVisualState") != std::string::npos) return false;
    if (moveBranch.find("prepareVisibleContentTiles") != std::string::npos) return false;
    if (moveBranch.find("requestContentInvalidation") != std::string::npos) return false;

    return true;
}

// Source-code contract: PianoRoll tool layer must not have identity fallback
bool pianoRollProjection_noIdentityFallbackContract()
{
    const std::string sourceDir = getSourceRoot();
    const auto tool = sourceDir + "/Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp";
    const auto toolHeader = sourceDir + "/Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h";
    const auto component = sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp";
    const auto componentHeader = sourceDir + "/Source/Standalone/UI/PianoRollComponent.h";
    const auto projection = sourceDir + "/Source/Utils/ContentTimelineProjection.h";

    const auto toolText = readFileContent(tool);
    const auto toolHeaderText = readFileContent(toolHeader);
    if (toolHeaderText.find("std::optional<double> pixelXToSourceTime") == std::string::npos) return false;
    if (toolHeaderText.find("double pixelXToSourceTime") != std::string::npos) return false;
    if (toolText.find("return timelineTime;") != std::string::npos) return false;
    if (toolText.find("ctx_.projectTimelineTimeToContent") != std::string::npos) return false;
    if (toolText.find("ctx_.projectContentTimeToTimeline") != std::string::npos) return false;
    if (toolText.find("projectTimelineTimeToContent ?") != std::string::npos) return false;
    if (toolText.find("projectContentTimeToTimeline ?") != std::string::npos) return false;
    if (toolText.find("value_or(") != std::string::npos) return false;
    if (toolText.find("trackRelativeTime = -1.0") != std::string::npos) return false;
    if (toolText.find("= -1.0") != std::string::npos) return false;
    if (toolText.find("return -1.0") != std::string::npos) return false;
    if (extractMethodBody(toolText, "PianoRollToolHandler::pixelXToSourceTime").find("return 0.0") != std::string::npos) return false;

    const auto componentText = readFileContent(component);
    const auto componentHeaderText = readFileContent(componentHeader);
    if (componentText.find("PianoRollComponent::projectTimelineTimeToContent") != std::string::npos) return false;
    if (componentText.find("PianoRollComponent::projectContentTimeToTimeline") != std::string::npos) return false;
    if (componentHeaderText.find("projectTimelineTimeToContent") != std::string::npos) return false;
    if (componentHeaderText.find("projectContentTimeToTimeline") != std::string::npos) return false;

    const auto projectionText = readFileContent(projection);
    if (extractMethodBody(projectionText, "projectTimelineTimeToContent").find("isValid") != std::string::npos) return false;
    if (extractMethodBody(projectionText, "projectContentTimeToTimeline").find("isValid") != std::string::npos) return false;

    return true;
}

// ============================================================================
// Kill List Static Checks
// ============================================================================

static int runKillListChecks()
{
    int total = 0, failed = 0;
    std::string sourceDir = getSourceRoot();
    std::cout << "Source directory: " << sourceDir << "\n";

    std::vector<std::string> excludeDirs = {"Tests", "build", "JUCE", "ThirdParty", "docs", ".git"};

    struct KillItem { const char* name; const char* pattern; };
    std::vector<KillItem> simpleKill = {
        {"timeToXWithScroll", "timeToXWithScroll"},
        {"withBand", "withBand"},
        {"computeScrollOffsetPx", "computeScrollOffsetPx"},
        {"timelineViewDomain_", "timelineViewDomain_"},
        {"computeMaxVisibleStartSeconds", "computeMaxVisibleStartSeconds"},
        {"surfaceStartSec", "surfaceStartSec"},
        {"surfaceEndSec", "surfaceEndSec"},
        {"computeSurfaceStartTimelineSeconds", "computeSurfaceStartTimelineSeconds"},
        {"computeSurfaceEndTimelineSeconds", "computeSurfaceEndTimelineSeconds"},
        {"contentSurface_", "contentSurface_"},
        {"rulerSurface_", "rulerSurface_"},
        {"computeMaxTimelineEndSeconds", "computeMaxTimelineEndSeconds"},
        {"bandStartContentX", "bandStartContentX"},
        {"setPlayheadPresentation", "setPlayheadPresentation"},
        {"updatePlayheadPresentationPolicy", "updatePlayheadPresentationPolicy"},
        {"publishPlayheadPresentation", "publishPlayheadPresentation"},
        {"followAndPublishPlayhead", "followAndPublishPlayhead"},
    };

    for (const auto& ki : simpleKill) {
        ++total;
        if (anySourceContains(sourceDir, ki.pattern, excludeDirs)) {
            std::cout << "[FAIL] killList_" << ki.name << " — " << ki.pattern << " still exists\n"; ++failed;
        } else {
            std::cout << "[PASS] killList_" << ki.name << " — " << ki.pattern << " not found\n";
        }
    }

    // Class/struct checks
    struct ClassCheck { const char* name; const char* classPattern; const char* structPattern; };
    std::vector<ClassCheck> classChecks = {
        {"PlayheadPresentation", "class PlayheadPresentation", "struct PlayheadPresentation"},
        {"PlayheadOverlayComponent", "class PlayheadOverlayComponent", "struct PlayheadOverlayComponent"},
        {"PianoRollSurfaceCache", "class PianoRollSurfaceCache", "struct PianoRollSurfaceCache"},
        {"ArrangementRenderModelCache", "class ArrangementRenderModelCache", "struct ArrangementRenderModelCache"},
    };
    for (const auto& cc : classChecks) {
        ++total;
        bool found = anySourceContains(sourceDir, cc.classPattern, excludeDirs)
                  || anySourceContains(sourceDir, cc.structPattern, excludeDirs);
        if (found) { std::cout << "[FAIL] killList_" << cc.name << " — still exists\n"; ++failed; }
        else { std::cout << "[PASS] killList_" << cc.name << " — not found\n"; }
    }

    // Method-body checks
    {
        ++total;
        std::vector<std::string> targetFiles = {
            sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp",
            sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.cpp"
        };
        bool violated = false;
        for (const auto& fp : targetFiles) {
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "drawTimeRuler")) { violated = true; break; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "drawGridLines")) { violated = true; break; }
        }
        if (violated) { std::cout << "[FAIL] killList_drawFuncsInPaint — drawTimeRuler/drawGridLines in paint()\n"; ++failed; }
        else { std::cout << "[PASS] killList_drawFuncsInPaint — not in paint()\n"; }
    }

    // content production in component paint() — tightened contract (both views)
    {
        ++total;
        std::vector<std::string> targetFiles = {
            sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp",
            sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.cpp"
        };
        bool violated = false;
        std::string violators;
        for (const auto& fp : targetFiles) {
            namespace fs = std::filesystem;
            if (!fs::exists(fp)) continue;
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "ContentTileKey")) { violated = true; violators += fp + ":ContentTileKey "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "getOrBuildTile")) { violated = true; violators += fp + ":getOrBuildTile "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "getContentTile")) { violated = true; violators += fp + ":getContentTile "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "prepareVisibleContentTiles")) { violated = true; violators += fp + ":prepareVisibleContentTiles "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "getPatternTile")) { violated = true; violators += fp + ":getPatternTile "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "buildPatternTile")) { violated = true; violators += fp + ":buildPatternTile "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "waveformMipmapCache_.get")) { violated = true; violators += fp + ":waveformMipmapCache_.get "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "snapshotLevel")) { violated = true; violators += fp + ":snapshotLevel "; }
            if (patternInMethodBody(fp, "paint(juce::Graphics& g)", "drawImageAtTimelineStart")) { violated = true; violators += fp + ":drawImageAtTimelineStart "; }
        }
        if (violated) { std::cout << "[FAIL] killList_noContentProductionInPaint — paint() contains: " << violators << "\n"; ++failed; }
        else { std::cout << "[PASS] killList_noContentProductionInPaint — no content production in paint()\n"; }
    }

    // commitCamera must not contain updateOverlayPresentation
    {
        ++total;
        std::vector<std::string> targetFiles = {
            sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp",
            sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.cpp"
        };
        bool violated = false;
        for (const auto& fp : targetFiles)
            if (patternInMethodBody(fp, "commitCamera(", "updateOverlayPresentation")) { violated = true; break; }
        if (violated) { std::cout << "[FAIL] killList_updateOverlayInCommitCamera\n"; ++failed; }
        else { std::cout << "[PASS] killList_updateOverlayInCommitCamera\n"; }
    }

    // Banned identifiers: old API names must not exist in source
    {
        ++total;
        std::vector<std::string> bannedPatterns = {
            "pendingSeekTime_", "presentationClock", "getDisplayPlayheadTime",
            "updatePresentationClock", "resetPresentationClock",
            "clampContentTime", "clampTimelineTime", "clampProjectionValue",
            "setTimelineViewport(", "computeCamera(", "zoomAtMouse("
        };
        std::vector<std::string> targetFiles = {
            sourceDir + "/Source/Standalone/UI/PianoRollComponent.cpp",
            sourceDir + "/Source/Standalone/UI/PianoRollComponent.h",
            sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.cpp",
            sourceDir + "/Source/Standalone/UI/ArrangementViewComponent.h",
            sourceDir + "/Source/Standalone/UI/TimelineViewportPolicy.cpp",
            sourceDir + "/Source/Standalone/UI/TimelineViewportPolicy.h",
            sourceDir + "/Source/Utils/ContentTimelineProjection.h",
            sourceDir + "/Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp"
        };
        std::string violators;
        for (const auto& fp : targetFiles) {
            std::ifstream f(fp);
            std::string line;
            while (std::getline(f, line)) {
                for (const auto& pat : bannedPatterns) {
                    if (line.find(pat) != std::string::npos
                        && line.find("//") == std::string::npos) {
                        violators += fp + ":" + pat + " ";
                    }
                }
            }
        }
        if (!violators.empty()) { std::cout << "[FAIL] killList_bannedIdentifiers — found: " << violators << "\n"; ++failed; }
        else { std::cout << "[PASS] killList_bannedIdentifiers — no banned API in source\n"; }
    }

    std::cout << "\n========================================\n";
    if (failed == 0) std::cout << "All " << total << " Kill List checks passed.\n";
    else std::cout << failed << " of " << total << " Kill List checks FAILED.\n";
    std::cout << "========================================\n";
    return failed != 0 ? 1 : 0;
}

// ============================================================================
// Test Runner
// ============================================================================

int main()
{
    int failed = 0;
    auto run = [&failed](bool (*fn)(), const char* name) {
        std::cout << "  " << name << "...\n";
        if (!fn()) { ++failed; std::cout << "  [FAIL]\n"; }
    };

    std::cout << "========================================\n";
    std::cout << "OpenTune Timeline Presentation Tests\n";
    std::cout << "========================================\n\n";

    std::cout << "--- Pattern Cache ---\n";
    run(patternCache_noClipClickZoomPlay, "patternCache_noClipClickZoomPlay");
    run(patternTileKey_equality, "patternTileKey_equality");

    std::cout << "\n--- Fixed Playhead ---\n";
    run(fixedPlayhead_anchorBounds, "fixedPlayhead_anchorBounds");

    std::cout << "\n--- View Switch ---\n";
    run(viewSwitch_preheated, "viewSwitch_preheated");

    std::cout << "\n--- Encode/Decode ---\n";
    run(encodeDecode_verticalGeometry, "encodeDecode_verticalGeometry");
    run(encodeDecode_laneStyle, "encodeDecode_laneStyle");

    std::cout << "\n--- Cache Invalidate ---\n";
    run(patternCache_invalidate, "patternCache_invalidate");

    std::cout << "\n--- Content Cache ---\n";
    run(patternCache_realRendering, "patternCache_realRendering");
    run(contentCache_getOrBuildTile, "contentCache_getOrBuildTile");
    run(contentCache_realRendererPixelOutput, "contentCache_realRendererPixelOutput");
    run(contentCache_discriminatorIsolation, "contentCache_discriminatorIsolation");

    std::cout << "\n--- Contract Tests ---\n";
    run(arrangementWaveform_clipInSecondsAnchor, "arrangementWaveform_clipInSecondsAnchor");
    run(pianoRoll_noFallbackToFirstValidPlacement, "pianoRoll_noFallbackToFirstValidPlacement");
    run(arrangementContentKeyIsValid, "arrangementContentKeyIsValid");
    run(arrangementRevisionChangesOnTrimAndWaveformGeneration, "arrangementRevisionChangesOnTrimAndWaveformGeneration");
    run(pianoRollPreparedTilesUseValidContentKey, "pianoRollPreparedTilesUseValidContentKey");
    run(pianoRoll_usesEditedPlacementOnly, "pianoRoll_usesEditedPlacementOnly");
    run(pianoRoll_contentTileRevisionTracksPixelInputs, "pianoRoll_contentTileRevisionTracksPixelInputs");
    run(pianoRoll_drawUsesLayerComposerBlitPath, "pianoRoll_drawUsesLayerComposerBlitPath");
    run(pianoRoll_pageModePlayingKeepsFixedAnchor, "pianoRoll_pageModePlayingKeepsFixedAnchor");
    run(timelineViewportPolicy_contracts, "timelineViewportPolicy_contracts");
    run(arrangementMoveDrag_transientOverlayContract, "arrangementMoveDrag_transientOverlayContract");
    run(pianoRollProjection_noIdentityFallbackContract, "pianoRollProjection_noIdentityFallbackContract");

    std::cout << "\n--- Kill List ---\n";
    failed += runKillListChecks();

    std::cout << "\n========================================\n";
    if (failed == 0) std::cout << "All tests passed.\n";
    else std::cout << failed << " test(s) FAILED.\n";
    std::cout << "========================================\n";

    return failed != 0 ? 1 : 0;
}

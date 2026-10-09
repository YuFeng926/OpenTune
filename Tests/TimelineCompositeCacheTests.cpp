#include "../Source/Standalone/UI/TimelineCompositeCache.h"

#include <cstdio>

using namespace OpenTune;

int main()
{
    TimelineCompositeCache cache;
    BackgroundGenerationSignature background;
    background.pixelsPerSecond = 100.0;
    ForegroundGenerationSignature foreground;
    foreground.pixelsPerSecond = 100.0;

    int backgroundBuilds = 0;
    int foregroundBuilds = 0;
    const auto buildBackground = [&backgroundBuilds](juce::Graphics&, juce::Rectangle<int>,
                                                      TimelineCompositeCache::TileKey) {
        ++backgroundBuilds;
    };
    const auto buildForeground = [&foregroundBuilds](juce::Graphics&, juce::Rectangle<int>,
                                                      TimelineCompositeCache::TileKey) {
        ++foregroundBuilds;
    };

    cache.prepare(background, foreground, 0, 0, 0, 0, buildBackground, buildForeground);
    if (backgroundBuilds != 1 || foregroundBuilds != 1) {
        std::fprintf(stderr, "initial prepare should build both tile layers once\n");
        return 1;
    }

    cache.prepare(background, foreground, 0, 0, 0, 0, buildBackground, buildForeground);
    if (backgroundBuilds != 1 || foregroundBuilds != 1) {
        std::fprintf(stderr, "unchanged signatures should reuse both tile layers\n");
        return 1;
    }

    background.themeId = 1;
    cache.prepare(background, foreground, 0, 0, 0, 0, buildBackground, buildForeground);
    if (backgroundBuilds != 2 || foregroundBuilds != 1) {
        std::fprintf(stderr, "background changes should rebuild only the background layer\n");
        return 1;
    }

    ++foreground.contentRevision;
    cache.prepare(background, foreground, 0, 0, 0, 0, buildBackground, buildForeground);
    if (backgroundBuilds != 2 || foregroundBuilds != 2) {
        std::fprintf(stderr, "content changes should rebuild only the foreground layer\n");
        return 1;
    }

    const auto* tile = cache.findTile({0, 0});
    if (tile == nullptr || !tile->hasBackgroundFor(background)
        || !tile->hasForegroundFor(foreground)) {
        std::fprintf(stderr, "prepared tile should retain matching per-layer signatures\n");
        return 1;
    }

    TimelineCompositeCache incrementalCache;
    int incrementalBackgroundBuilds = 0;
    int incrementalForegroundBuilds = 0;
    const auto incrementalBackground = [&incrementalBackgroundBuilds](juce::Graphics&, juce::Rectangle<int>, TimelineCompositeCache::TileKey) { ++incrementalBackgroundBuilds; };
    const auto incrementalForeground = [&incrementalForegroundBuilds](juce::Graphics&, juce::Rectangle<int>, TimelineCompositeCache::TileKey) { ++incrementalForegroundBuilds; };
    incrementalCache.prepareTile(background, foreground, {1, 0}, incrementalBackground, incrementalForeground);
    incrementalCache.prepareTile(background, foreground, {1, 0}, incrementalBackground, incrementalForeground);
    if (incrementalBackgroundBuilds != 1 || incrementalForegroundBuilds != 1) {
        std::fprintf(stderr, "prepareTile should reuse matching signatures\n");
        return 1;
    }
    auto changedBackground = background;
    changedBackground.themeId++;
    incrementalCache.prepareTile(changedBackground, foreground, {1, 0}, incrementalBackground, incrementalForeground);
    if (incrementalBackgroundBuilds != 2 || incrementalForegroundBuilds != 1) {
        std::fprintf(stderr, "prepareTile should rebuild only the changed layer\n");
        return 1;
    }

    std::puts("TimelineCompositeCache tests passed.");
    return 0;
}

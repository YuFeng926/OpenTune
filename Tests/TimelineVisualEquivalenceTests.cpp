// ============================================================================
// OpenTune Timeline Visual Equivalence Tests
// ============================================================================
// Plan: docs/plans/2026-07-06-equivalence-hard-closeout-refactor-repair.md
//
// Read-only visual golden comparison tests.
// Normal validation must compare against committed golden PNGs and must fail
// if goldens are missing. It must not regenerate files.
//
// Baseline commit: 0cb5f23
// Golden path: Tests/Golden/TimelineEquivalence/
// ============================================================================

#include "../Source/Standalone/UI/ArrangementViewComponent.h"
#include "../Source/Standalone/UI/PianoRollComponent.h"
#include "../Source/Standalone/UI/TimelineViewportPolicy.h"
#include "../Source/PluginProcessor.h"
#include "../Source/StandaloneArrangement.h"
#include "../Source/Standalone/StandaloneArrangementHelpers.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace OpenTune;

namespace {

// ------------------------------------------------------------------
// Test infrastructure
// ------------------------------------------------------------------
static int failures = 0;

struct PixelDiff {
    int    mismatchedPixels{0};
    int    totalPixels{0};
    double changedPixelRatio{0.0};
    int    maxChannelDelta{0};
};

// Golden image directory
static std::filesystem::path goldenDir()
{
    return std::filesystem::path(OPENTUNE_SOURCE_DIR) / "Tests/Golden/TimelineEquivalence";
}

static juce::Image loadGolden(const std::string& name)
{
    const auto path = goldenDir() / name;
    juce::File f(path.string());
    if (!f.existsAsFile()) {
        std::cout << "[FAIL] Golden file not found: " << path.string() << "\n";
        ++failures;
        return {};
    }

    juce::PNGImageFormat png;
    juce::FileInputStream stream(f);
    if (!stream.openedOk()) {
        std::cout << "[FAIL] Cannot open golden file: " << path.string() << "\n";
        ++failures;
        return {};
    }
    return png.decodeImage(stream);
}

template <typename Component>
static juce::Image renderComponent(Component& component, int width, int height)
{
    component.setBounds(0, 0, width, height);
    component.resized();

    juce::Image image(juce::Image::ARGB, width, height, true);
    juce::Graphics g(image);
    component.paintEntireComponent(g, true);
    return image;
}

static ContentKey makeVisualTestContentKey(uint64_t objectId)
{
    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = objectId;
    return key;
}

static uint64_t insertVisualTestPlacement(StandaloneArrangement& arrangement,
                                          int trackId,
                                          double startSeconds,
                                          double durationSeconds,
                                          uint64_t objectId)
{
    StandaloneArrangement::Placement placement;
    placement.contentKey = makeVisualTestContentKey(objectId);
    placement.timelineStartSeconds = startSeconds;
    placement.durationSeconds = durationSeconds;
    placement.gain = 1.0f;

    if (!arrangement.insertPlacement(trackId, placement)) {
        std::cout << "[FAIL] Could not insert visual test placement\n";
        ++failures;
        return 0;
    }

    return arrangement.getPlacementId(trackId, arrangement.getNumPlacements(trackId) - 1);
}

static PixelDiff comparePixels(const juce::Image& a, const juce::Image& b)
{
    PixelDiff diff;
    diff.totalPixels = a.getWidth() * a.getHeight();

    int maxDelta = 0;
    int mismatched = 0;

    for (int y = 0; y < a.getHeight(); ++y) {
        for (int x = 0; x < a.getWidth(); ++x) {
            const auto pa = a.getPixelAt(x, y);
            const auto pb = b.getPixelAt(x, y);
            const int dr = std::abs(pa.getRed()   - pb.getRed());
            const int dg = std::abs(pa.getGreen() - pb.getGreen());
            const int db = std::abs(pa.getBlue()  - pb.getBlue());
            const int da = std::abs(pa.getAlpha() - pb.getAlpha());
            const int maxC = std::max({dr, dg, db, da});
            maxDelta = std::max(maxDelta, maxC);
            if (maxC > 0) ++mismatched;
        }
    }

    diff.mismatchedPixels = mismatched;
    diff.maxChannelDelta  = maxDelta;
    diff.changedPixelRatio = (diff.totalPixels > 0)
        ? static_cast<double>(mismatched) / diff.totalPixels
        : 0.0;
    return diff;
}

static void writeActualFailureImage(const juce::Image& image, const std::string& goldenName)
{
    const auto outDir = std::filesystem::current_path() / "TimelineVisualActual";
    std::filesystem::create_directories(outDir);

    const auto path = outDir / goldenName;
    juce::File file(path.string());
    if (file.existsAsFile())
        file.deleteFile();

    juce::PNGImageFormat png;
    juce::FileOutputStream out(file);
    if (out.openedOk() && png.writeImageToStream(image, out))
        std::cout << "  wrote actual failure image: " << path.string() << "\n";
}

static bool goldenMatches(const juce::Image& actual, const std::string& goldenName,
                          double maxPixelRatio = 0.001, int maxChannelDelta = 1)
{
    auto expected = loadGolden(goldenName);
    if (!expected.isValid()) return false;

    if (actual.getWidth() != expected.getWidth() || actual.getHeight() != expected.getHeight()) {
        std::cout << "[FAIL] " << goldenName << ": size mismatch actual="
                  << actual.getWidth() << "x" << actual.getHeight()
                  << " expected=" << expected.getWidth() << "x" << expected.getHeight() << "\n";
        ++failures;
        return false;
    }

    auto diff = comparePixels(actual, expected);
    std::cout << "  " << goldenName << ": " << diff.mismatchedPixels
              << " / " << diff.totalPixels << " pixels differ"
              << " (ratio=" << diff.changedPixelRatio
              << ", maxDelta=" << diff.maxChannelDelta << ")\n";

    if (diff.maxChannelDelta > maxChannelDelta || diff.changedPixelRatio > maxPixelRatio) {
        std::cout << "[FAIL] " << goldenName << ": pixel diff exceeds threshold\n";
        writeActualFailureImage(actual, goldenName);
        ++failures;
        return false;
    }

    return true;
}

// ------------------------------------------------------------------
// Golden comparison tests
// ------------------------------------------------------------------
bool arrangementClipPixelsMatchBaseline()
{
    std::cout << "--- arrangementClipPixelsMatchBaseline ---\n";

    OpenTuneAudioProcessor processor;
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr) {
        std::cout << "[FAIL] arrangementClipPixelsMatchBaseline: missing standalone arrangement\n";
        ++failures;
        return false;
    }

    arrangement->setTrackColour(1, juce::Colour::fromRGB(0xd4, 0x87, 0x3f));
    const auto placementId = insertVisualTestPlacement(*arrangement, 1, 1.0, 4.0, 1001);
    if (placementId == 0)
        return false;

    arrangement->selectPlacement(1, placementId);

    ArrangementViewComponent view(processor);
    view.setVisibleTrackCount(12);
    view.setBounds(0, 0, 1400, 520);
    view.resized();

    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.viewKind = TimelineViewportRequest::ViewKind::Arrangement;
    req.targetTime = 0.0;
    req.viewportWidth = view.timelinePolicyViewportWidth();
    req.pixelsPerSecond = 120.0;
    view.commitViewportRequest(req, juce::sendNotification);

    const auto actual = renderComponent(view, 1400, 520);
    return goldenMatches(actual, "arrangement_clip_waveform_fade_label.png");
}

bool pianoRollChromeSurvivesPatternTiles()
{
    std::cout << "--- pianoRollChromeSurvivesPatternTiles ---\n";

    PianoRollComponent view;
    view.setScrollMode(PianoRollComponent::ScrollMode::Page);

    auto curve = std::make_shared<PitchCurve>();
    std::vector<float> f0(400, 220.0f);
    curve->setOriginalF0(f0);

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 44100 * 4);
    buffer->clear();

    ContentKey key = makeVisualTestContentKey(2001);
    view.setEditedContent(key, curve, buffer, 44100);
    view.setShowWaveform(true);
    view.setBounds(0, 0, 1400, 720);
    view.resized();

    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.viewKind = TimelineViewportRequest::ViewKind::PianoRoll;
    req.targetTime = 0.0;
    req.viewportWidth = view.timelinePolicyViewportWidth();
    req.pixelsPerSecond = 120.0;
    view.commitViewportRequest(req, juce::sendNotification);

    const auto actual = renderComponent(view, 1400, 720);
    return goldenMatches(actual, "pianoroll_chrome_pattern_tiles.png");
}

} // anonymous namespace

// ============================================================================
// Main
// ============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::cout << "=== OpenTune Timeline Visual Equivalence Tests ===\n\n";

    arrangementClipPixelsMatchBaseline();
    pianoRollChromeSurvivesPatternTiles();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL VISUAL TESTS PASSED\n";
        return 0;
    } else {
        std::cout << failures << " VISUAL TEST(S) FAILED\n";
        return 1;
    }
}

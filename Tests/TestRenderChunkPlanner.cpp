#include "../Source/Render/RenderChunkPlanner.h"
#include "../Source/Utils/TimeCoordinate.h"

#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cout << "[FAIL] " << message << "\n";
        return false;
    }
    return true;
}

bool expectChunkSize(size_t expected,
                     const std::vector<OpenTune::RenderChunkPlanner::ChunkRange>& chunks,
                     const char* message)
{
    if (!expect(chunks.size() == expected, message))
        return false;
    return true;
}

bool expectChunk(int64_t expectedStart,
                 int64_t expectedEnd,
                 const OpenTune::RenderChunkPlanner::ChunkRange& chunk,
                 const char* message)
{
    if (!expect(chunk.startSample == expectedStart && chunk.endSampleExclusive == expectedEnd,
                message))
        return false;
    return true;
}

bool runLocalEditSelectsCompleteGlobalChunk()
{
    constexpr int64_t contentSampleCount = 441000; // 10 s @ 44.1 kHz
    constexpr int hopSize = 512;

    const std::vector<OpenTune::SilentGap> silentGaps {
        { 220500, 229320, -80.0f } // 5.0 s - 5.2 s
    };

    const auto chunks = OpenTune::RenderChunkPlanner::selectChunksIntersectingRange(
        contentSampleCount,
        silentGaps,
        88200,  // 2.0 s
        132300, // 3.0 s
        hopSize);

    if (!expectChunkSize(1, chunks,
                         "localEditSelectsCompleteGlobalChunk: edit fully inside a chunk "
                         "must select exactly one global chunk"))
        return false;

    if (!expectChunk(0, 224768, chunks[0],
                     "localEditSelectsCompleteGlobalChunk: selected chunk must span the full "
                     "first global segment (0 .. 224768 samples ≈ 5.097 s), not the edit range"))
        return false;

    return true;
}

bool runSilentGapOutsideEditRangeStillDeterminesBoundary()
{
    constexpr int64_t contentSampleCount = 441000; // 10 s @ 44.1 kHz
    constexpr int hopSize = 512;

    const std::vector<OpenTune::SilentGap> silentGaps {
        { 176400, 185220, -80.0f } // 4.0 s - 4.2 s
    };

    const auto chunks = OpenTune::RenderChunkPlanner::selectChunksIntersectingRange(
        contentSampleCount,
        silentGaps,
        88200,  // 2.0 s
        132300, // 3.0 s
        hopSize);

    if (!expectChunkSize(1, chunks,
                         "silentGapOutsideEditRangeStillDeterminesBoundary: edit before the gap "
                         "must still terminate at the global chunk boundary"))
        return false;

    if (!expectChunk(0, 180736, chunks[0],
                     "silentGapOutsideEditRangeStillDeterminesBoundary: chunk must end at the "
                     "gap-aligned sample (180736 ≈ 4.098 s), not at the content tail (441000)"))
        return false;

    return true;
}

bool runEditCrossingBoundarySelectsBothChunks()
{
    constexpr int64_t contentSampleCount = 441000; // 10 s @ 44.1 kHz
    constexpr int hopSize = 512;

    const std::vector<OpenTune::SilentGap> silentGaps {
        { 220500, 229320, -80.0f } // 5.0 s - 5.2 s
    };

    const auto chunks = OpenTune::RenderChunkPlanner::selectChunksIntersectingRange(
        contentSampleCount,
        silentGaps,
        198450, // 4.5 s
        242550, // 5.5 s
        hopSize);

    if (!expectChunkSize(2, chunks,
                         "editCrossingBoundarySelectsBothChunks: edit that crosses a global "
                         "boundary must return both complete adjacent chunks"))
        return false;

    if (!expectChunk(0, 224768, chunks[0],
                     "editCrossingBoundarySelectsBothChunks: first chunk must keep the global "
                     "boundary before the gap (0 .. 224768)"))
        return false;

    if (!expectChunk(224768, 441000, chunks[1],
                     "editCrossingBoundarySelectsBothChunks: second chunk must start at the "
                     "global boundary after the gap (224768 .. 441000)"))
        return false;

    return true;
}

bool runNoSelectedChunkOverlapsAnother()
{
    constexpr int64_t contentSampleCount = 1323000; // 30 s @ 44.1 kHz
    constexpr int hopSize = 512;

    const std::vector<OpenTune::SilentGap> silentGaps {
        { 220500, 229320, -80.0f }, // 5.0 s - 5.2 s
        { 441000, 449820, -80.0f }, // 10.0 s - 10.2 s
        { 661500, 670320, -80.0f }  // 15.0 s - 15.2 s
    };

    const auto chunks = OpenTune::RenderChunkPlanner::selectChunksIntersectingRange(
        contentSampleCount,
        silentGaps,
        110250, // 2.5 s
        705600, // 16.0 s
        hopSize);

    if (!expect(chunks.size() >= 3,
                "noSelectedChunkOverlapsAnother: edit spanning multiple silent gaps must "
                "produce at least three global chunks"))
        return false;

    for (size_t i = 0; i < chunks.size(); ++i) {
        if (!expect(chunks[i].endSampleExclusive > chunks[i].startSample,
                    "noSelectedChunkOverlapsAnother: each returned chunk must be non-empty"))
            return false;
    }

    for (size_t i = 1; i < chunks.size(); ++i) {
        if (!expect(chunks[i].startSample >= chunks[i - 1].endSampleExclusive,
                    "noSelectedChunkOverlapsAnother: returned chunks must be ordered and must "
                    "not overlap (chunks[i].start >= chunks[i-1].end)"))
            return false;
    }

    return true;
}

bool runEditStartDoesNotCreateArbitraryBoundary()
{
    constexpr int64_t contentSampleCount = 441000; // 10 s @ 44.1 kHz
    constexpr int hopSize = 512;

    const std::vector<OpenTune::SilentGap> silentGaps {}; // no silent gaps

    const auto chunks = OpenTune::RenderChunkPlanner::selectChunksIntersectingRange(
        contentSampleCount,
        silentGaps,
        110250, // 2.5 s
        154350, // 3.5 s
        hopSize);

    if (!expectChunkSize(1, chunks,
                         "editStartDoesNotCreateArbitraryBoundary: edit inside gap-less content "
                         "must collapse to the single whole-content chunk"))
        return false;

    if (!expectChunk(0, 441000, chunks[0],
                     "editStartDoesNotCreateArbitraryBoundary: chunk must start at 0 and run to "
                     "the content tail; the edit start (110250) must not become a boundary"))
        return false;

    return true;
}

} // namespace

int main()
{
    // -------------------------------------------------------------------------
    // Existing global-chunk selection contract
    // -------------------------------------------------------------------------
    {
        constexpr int64_t contentSampleCount = 24000;
        constexpr int hopSize = 512;

        const std::vector<OpenTune::SilentGap> silentGaps {
            { 4600, 5600, -80.0f },
            { 12200, 13200, -82.0f },
            { 18300, 19300, -78.0f }
        };

        const auto boundaries = OpenTune::RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(
            contentSampleCount,
            silentGaps,
            hopSize);

        if (!expect(boundaries == std::vector<int64_t>({ 0, 5120, 12800, 18944, 24000 }),
                    "global silent gaps must produce stable full-content chunk boundaries")) {
            return 1;
        }

        const auto chunks = OpenTune::RenderChunkPlanner::selectChunksIntersectingRange(
            contentSampleCount,
            silentGaps,
            6100,
            7000,
            hopSize);

        if (!expect(chunks.size() == 1, "local edit must select exactly one intersecting global chunk")) {
            return 1;
        }

        if (!expect(chunks[0].startSample == 5120 && chunks[0].endSampleExclusive == 12800,
                    "selected render job must use the complete global chunk, not the edit-local range")) {
            return 1;
        }

        const auto bridgeChunks = OpenTune::RenderChunkPlanner::selectChunksIntersectingRange(
            contentSampleCount,
            silentGaps,
            12600,
            13000,
            hopSize);

        if (!expect(bridgeChunks.size() == 2, "edit range crossing a global boundary must select both complete chunks")) {
            return 1;
        }

        if (!expect(bridgeChunks[0].startSample == 5120 && bridgeChunks[0].endSampleExclusive == 12800,
                    "first selected chunk must preserve the global boundary before the edit range")) {
            return 1;
        }

        if (!expect(bridgeChunks[1].startSample == 12800 && bridgeChunks[1].endSampleExclusive == 18944,
                    "second selected chunk must preserve the global boundary after the edit range")) {
            return 1;
        }
    }

    // -------------------------------------------------------------------------
    // Render mutation hard-cut chunk planner contract
    // -------------------------------------------------------------------------
    if (!runLocalEditSelectsCompleteGlobalChunk())                       return 1;
    if (!runSilentGapOutsideEditRangeStillDeterminesBoundary())          return 1;
    if (!runEditCrossingBoundarySelectsBothChunks())                     return 1;
    if (!runNoSelectedChunkOverlapsAnother())                            return 1;
    if (!runEditStartDoesNotCreateArbitraryBoundary())                   return 1;

    std::cout << "[PASS] RenderChunkPlanner global chunk selection\n";
    return 0;
}

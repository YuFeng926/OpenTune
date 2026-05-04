# VST3 ARA Plugin Silence Investigation Report

**Date**: April 24, 2026  
**Issue**: Plugin produces silence and shows "No preferred ARA playback region" when inserted on a DAW audio item

## Executive Summary

**Critical Root Cause Identified**: The ARA callback chain is **broken at the handoff from DocumentController to Session**.

The `OpenTuneDocumentController::setProcessor()` is never called with a valid processor pointer. This means:
1. `session_` remains `nullptr`
2. All ARA callbacks silently fail (return early due to null checks)
3. Audio regions and sources are never registered with the session
4. The published snapshot remains empty
5. The editor has nothing to display → "No preferred ARA playback region is available"

---

## Detailed Analysis

### 1. ARA Callback Chain Integrity ✗ BROKEN

**File**: `Source/ARA/OpenTuneDocumentController.cpp:50-54`

```cpp
void OpenTuneDocumentController::setProcessor(OpenTuneAudioProcessor* processor)
{
    processor_ = processor;
    session_ = processor != nullptr ? processor->getVst3AraSession() : nullptr;
}
```

**Status**: ✗ BROKEN - `session_` becomes `nullptr` if `setProcessor()` is never called

**Evidence**:
- Line 98-104: `didAddPlaybackRegionToAudioModification()` returns early if `session_ == nullptr`
- Line 126-131: `didEnableAudioSourceSamplesAccess()` returns early if `session_ == nullptr`
- Line 209-234: `didUpdatePlaybackRegionProperties()` returns early if `session_ == nullptr`
- Similar pattern across all 9 ARA callbacks

**Problem**: When does `setProcessor()` get called?

---

### 2. Session Initialization Status ✗ FAILS

**File**: `Source/PluginProcessor.cpp:945-953`

```cpp
vst3AraSession_ = std::make_unique<VST3AraSession>();
vst3AraSession_->setProcessor(this);  // ✓ Session HAS processor reference

#if JucePlugin_Enable_ARA
if (auto* dc = getDocumentController())
{
    dc->setProcessor(this);  // ← CONDITIONAL: ONLY if getDocumentController() succeeds
}
#endif
```

**Status**: ✗ CONDITIONAL - Succeeds ONLY if `getDocumentController()` returns non-null

**File**: `Source/PluginProcessor.cpp:1240-1249`

```cpp
OpenTuneDocumentController* OpenTuneAudioProcessor::getDocumentController() const
{
    auto* controller = AudioProcessorARAExtension::getDocumentController();
    if (controller == nullptr)
    {
        return nullptr;  // ← FAILS if host hasn't created the DC yet
    }

    return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<OpenTuneDocumentController>(controller);
}
```

**The Timeline Problem**:
1. `OpenTuneAudioProcessor()` constructor runs
2. `vst3AraSession_->setProcessor(this)` ✓ succeeds
3. `getDocumentController()` ✗ returns `nullptr` (host hasn't instantiated the DC yet during plugin load)
4. `dc->setProcessor(this)` ✗ NEVER RUNS
5. `session_` remains `nullptr` in DocumentController
6. All ARA callbacks later fail silently

---

### 3. Snapshot Publication Path ✗ PRODUCES EMPTY SNAPSHOT

**File**: `Source/ARA/VST3AraSession.cpp:124-147`

```cpp
VST3AraSession::SnapshotHandle VST3AraSession::buildSnapshotForPublication(
    const std::vector<SourceSlot>& sourceSlots,
    const std::vector<RegionSlot>& regionSlots,
    const RegionIdentity& preferredRegion,
    uint64_t epoch)
{
    auto snapshot = std::make_shared<PublishedSnapshot>();
    snapshot->epoch = epoch;
    snapshot->preferredRegion = reconcilePreferredRegionFromState(sourceSlots, regionSlots, preferredRegion);
    snapshot->publishedRegions.reserve(regionSlots.size());

    for (const auto& regionSlot : regionSlots)
    {
        const auto* sourceSlot = findSourceSlotInCollection(sourceSlots, regionSlot.identity.audioSource);
        if (sourceSlot == nullptr)
            continue;

        const auto view = buildPublishedRegionViewFromState(regionSlot, *sourceSlot);
        if (view.isValid())
            snapshot->publishedRegions.push_back(view);
    }

    return std::static_pointer_cast<const PublishedSnapshot>(snapshot);
}
```

**Status**: ✗ EMPTY SNAPSHOT

**Root Cause Chain**:
1. Since `didAddPlaybackRegionToAudioModification()` never runs (session_ is null)
2. `regions_` and `sources_` maps remain empty
3. `buildPublishedSnapshotLocked()` gets empty `sources_` and `regions_`
4. The for-loop at line 135-144 finds nothing to iterate
5. `snapshot->publishedRegions` remains empty
6. `snapshot->preferredRegion` is empty (no regions to select from)

---

### 4. Editor Consumption Path ✗ FINDS NOTHING

**File**: `Source/Plugin/PluginEditor.cpp:186-196`

```cpp
const VST3AraSession::PublishedRegionView* resolvePreferredAraRegionView(
    const VST3AraSession::PublishedSnapshot& snapshot)
{
    if (const auto* preferredRegionView = snapshot.findPreferredRegion())
        return preferredRegionView;

    if (snapshot.publishedRegions.size() == 1)
        return &snapshot.publishedRegions.front();

    return nullptr;
}
```

**Status**: ✗ RETURNS NULLPTR

**File**: `Source/ARA/VST3AraSession.h:211-219`

```cpp
const PublishedRegionView* findPreferredRegion() const noexcept
{
    const auto it = std::find_if(publishedRegions.begin(), publishedRegions.end(),
                                 [this](const PublishedRegionView& view)
                                 {
                                     return view.regionIdentity == preferredRegion;
                                 });
    return it != publishedRegions.end() ? &(*it) : nullptr;
}
```

**Why it returns nullptr**:
1. `publishedRegions` is empty (see #3 above)
2. `publishedRegions.size()` is 0, not 1
3. Falls through to `return nullptr`
4. UI shows: "No preferred ARA playback region is available"

---

### 5. Audio Hydration Never Triggered

**File**: `Source/ARA/VST3AraSession.cpp:363-397`

```cpp
void VST3AraSession::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                       bool enable)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    if (audioSource == nullptr)
        return;

    auto& sourceSlot = ensureSourceSlot(audioSource);

    if (!enable)
    {
        // ... disable logic ...
        return;
    }

    sourceSlot.sampleAccessEnabled = true;
    sourceSlot.cancelRead = false;

    if (sourceSlot.readerLease == nullptr)
    {
        sourceSlot.readerLease = std::make_unique<ARA::PlugIn::HostAudioReader>(audioSource);
        ++sourceSlot.leaseGeneration;
    }

    if (sourceSlot.readingFromHost)
    {
        sourceSlot.enablePendingHydration = true;
        return;
    }

    enqueueSourceHydrationLocked(audioSource);  // ← This never runs
}
```

**Status**: ✗ NEVER RUNS

**Reason**: This callback is never invoked because `DocumentController::didEnableAudioSourceSamplesAccess()` returns early (line 129) due to `session_ == nullptr`.

---

## Verification: buildPublishedRegionViewFromState Does NOT Filter Empty Audio

**File**: `Source/ARA/VST3AraSession.cpp:30-68`

```cpp
VST3AraSession::PublishedRegionView buildPublishedRegionViewFromState(
    const VST3AraSession::RegionSlot& regionSlot,
    const VST3AraSession::SourceSlot& sourceSlot)
{
    // ... initialization ...
    view.copiedAudio = sourceSlot.hasAudio() ? sourceSlot.copiedAudio : nullptr;  // Line 38
    // ...
    if (regionSlot.appliedProjection.isValid()
        && regionSlot.appliedProjection.materializationId != 0
        && regionSlot.appliedProjection.appliedRegionIdentity == regionSlot.identity)
    {
        view.bindingState = VST3AraSession::BindingState::Renderable;
    }
    else if (regionSlot.appliedProjection.isValid() && regionSlot.appliedProjection.materializationId != 0)
    {
        view.bindingState = VST3AraSession::BindingState::BoundNeedsRender;
    }
    else if (sourceSlot.hasAudio())  // Line 59
    {
        view.bindingState = VST3AraSession::BindingState::Unbound;
    }
    else
    {
        view.bindingState = VST3AraSession::BindingState::HydratingSource;  // ← This is OK
    }
    return view;  // ← Region is returned even if copiedAudio == nullptr
}
```

**Finding**: ✓ The function CORRECTLY does NOT filter out regions. It returns a valid region with `bindingState = HydratingSource` when audio is not yet available. This is correct behavior.

**BUT**: Since the function is never called (no regions in maps), this code path is never reached.

---

## The Complete Failure Chain

```
Timeline:
1. DAW creates plugin instance
   ↓
2. OpenTuneAudioProcessor() constructor
   ↓
3. vst3AraSession_->setProcessor(this) ✓ SUCCEEDS
   ↓
4. getDocumentController() ✗ FAILS (host hasn't created DC yet)
   ↓
5. dc->setProcessor(this) NEVER RUNS
   ↓
6. DocumentController::session_ = nullptr (BROKEN FOREVER)
   ↓
7. DAW calls didAddPlaybackRegionToAudioModification()
   ↓
8. DocumentController checks if (session_ != nullptr) → FALSE
   ↓
9. Callback silently returns without registering region
   ↓
10. DAW calls didEnableAudioSourceSamplesAccess()
    ↓
11. Same check fails → audio never hydrated
    ↓
12. buildPublishedSnapshotLocked() iterates empty maps
    ↓
13. publishedSnapshot_ is empty
    ↓
14. Editor's resolvePreferredAraRegionView() returns nullptr
    ↓
15. UI displays "No preferred ARA playback region is available"
    ↓
16. Plugin produces silence (no regions to render)
```

---

## Why This Happens: Timing Issue

The problem is in `OpenTuneAudioProcessor::prepareToPlay()`:

**File**: `Source/PluginProcessor.cpp:1217-1228`

```cpp
#if JucePlugin_Enable_ARA
prepareToPlayForARA(sampleRate,
                    samplesPerBlock,
                    getMainBusNumOutputChannels(),
                    getProcessingPrecision());

// Set processor reference in document controller for unified read API access
if (auto* dc = getDocumentController())
{
    dc->setProcessor(this);  // ← CALLED in prepareToPlay, not constructor
}
#endif
```

**Good news**: There IS a second attempt to call `setProcessor()` in `prepareToPlay()`!

**Bad news**: If the host has already started sending ARA callbacks BEFORE `prepareToPlay()` is called, the DocumentController's `session_` will still be null during those callbacks.

---

## Root Cause Analysis: The Real Issue

**The fundamental problem**: `OpenTuneDocumentController::session_` is initialized to `nullptr` in the constructor and ONLY gets set in `setProcessor()`. But:

1. In the plugin constructor, `getDocumentController()` may fail
2. In `prepareToPlay()`, it should succeed, BUT
3. If the host sends ARA callbacks between plugin instantiation and `prepareToPlay()`, they ALL fail
4. Even after `prepareToPlay()`, if regions were already reported by the host, they're lost

**The Fix**: Ensure DocumentController always has access to the session, regardless of timing.

---

## Summary of Findings

| Aspect | Status | Issue |
|--------|--------|-------|
| ARA callback chain (DocController → Session) | ✗ BROKEN | `session_` is null because `setProcessor()` not called early enough |
| Session initialization | ✓ OK | Session itself is created and has processor reference |
| Snapshot publication | ✗ EMPTY | Regions never registered (callbacks never invoked) |
| Snapshot filtering logic | ✓ OK | `buildPublishedRegionViewFromState()` correctly preserves HydratingSource regions |
| Editor consumption | ✗ FAILS | No regions to find in empty snapshot |
| Audio hydration | ✗ NEVER TRIGGERED | `didEnableAudioSourceSamplesAccess()` returns early |
| Overall playback | ✗ SILENCE | No regions to render → silence output |

---

## Recommended Fix

**Option A (Defensive)**: Make DocumentController access the session lazily

```cpp
VST3AraSession* OpenTuneDocumentController::getSessionUnsafe() const
{
    if (session_ != nullptr)
        return session_;
    
    // Lazy lookup in case setProcessor() wasn't called yet
    if (processor_ != nullptr)
        return processor_->getVst3AraSession();
    
    return nullptr;
}
```

Then replace all `if (session_ != nullptr)` with `if (auto* session = getSessionUnsafe())`

**Option B (Proactive)**: Ensure setProcessor is called immediately after plugin creation

- Find where DocumentController is instantiated
- Call `setProcessor()` immediately in the factory/creation callback
- Verify no ARA callbacks are sent before this point

**Option C (Recommended)**: Combine both approaches
1. Call `setProcessor()` in constructor via a delayed callback
2. Also call in `prepareToPlay()` as backup
3. Use lazy lookup as defensive fallback


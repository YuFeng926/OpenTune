# Standalone Import Track-Target Drop UX Plan

**Goal:** Replace the current "always ask which track to import into" Standalone import UX with a DAW-style explicit placement flow:

1. Dragging audio onto an existing track imports into that track.
2. Dragging audio into the Arrangement blank area below visible tracks creates a new visible track and imports there.
3. File-menu / chooser import keeps a fast default path and does not reintroduce a track-choice modal for the common single-file case.
4. Multi-file import remains explicit, but the explicit choice becomes import mode, not per-drop track guessing.

**Verification Source:** `.planning/plans/2026-05-27-standalone-import-track-target-drop-ux-test-verification.md`

**Architecture:** Keep `ImportPlacement` as an explicit editor-owned decision. The processor must continue receiving resolved `trackId + startSeconds` and must not infer track intent from pointer position, hover state, or Arrangement geometry. All drag hover / preview state remains Standalone UI-only transient state.

---

## Current Root Causes

### 1. Drag-drop throws away the only signal needed for DAW-style placement

Current chain:

- `PluginEditor::filesDropped(const juce::StringArray& files, int x, int y)` receives the drop point.
- The current implementation ignores `x` and `y`.
- Drag-drop therefore cannot route to the track under the pointer or distinguish track lanes from blank Arrangement space.

This is the direct reason the current drop flow cannot behave like a DAW.

### 2. Drag-drop still uses a legacy modal track picker

Current chain:

- `filesDropped(...)` validates the file and always calls `promptTrackSelectionForDroppedFile(...)`.
- `promptTrackSelectionForDroppedFile(...)` opens a modal button list for `轨道1..N`.

This makes drag-drop slower than necessary and disconnects the final import result from the user's spatial drop intent.

### 3. Menu import and drag-drop currently follow different interaction contracts

Current chain:

- Single-file chooser import already has a fast path: it imports into the current active track and appends at the track end.
- Drag-drop ignores the drop target and opens a track picker instead.

The inconsistency is now a product problem: the editor already owns explicit placement, but only one entry path uses it cleanly.

### 4. "Create track on blank drop" is missing a defined boundary

The current Standalone UI has track-count controls, but the import flow has no declared rule for:

- when a blank drop means "new track";
- whether the new track must also become visible/active/selected;
- what happens at `MAX_TRACKS`;
- whether non-Arrangement drops should still fall back to the active-track import path.

Without fixing this contract first, implementation would drift into ad-hoc conditionals.

---

## Product Contract

### A. Drag-drop into Arrangement

- If the pointer is over an existing visible track lane at drop time, import into that track.
- The imported clip starts at the horizontal drop time, snapped by the same Arrangement time policy used for clip placement/import.
- If the pointer is below the last visible track lane but still inside the Arrangement drop surface, create one new visible track and import there.
- The new track becomes the active track and the imported placement becomes the current selection context.
- If the project is already at `MAX_TRACKS`, blank-area drop must not open the legacy track picker. It should surface a direct "cannot create more tracks" message and abort cleanly.

### B. Drag-drop outside Arrangement

- A drop that lands outside the Arrangement drop surface should not guess from unrelated coordinates.
- The fallback contract is: import into the current active track using the existing append/default policy.
- This keeps drag-drop usable from the top-level window without inventing fake lane geometry for non-Arrangement UI zones.

### C. File chooser import

- Single-file chooser import remains the fast path: current active track, no track picker, explicit `ImportPlacement`.
- Multi-file chooser import may still ask for import mode, but the choice should stay at the mode level:
  - append sequentially to the active track;
  - distribute starting from the active track across consecutive tracks, creating visible tracks if needed and allowed.
- The chooser flow should not regress into "pick a track for every file".

### D. Preview / feedback

- File drag over Arrangement must show which track will receive the import.
- Blank-area drop-to-create-track must preview the pending new lane target instead of silently deciding at release time.
- This preview is UI-only and must not create placements, tracks, undo actions, or processor state before drop.

---

## Non-Negotiable Boundaries

- Do not move track-target inference into `OpenTuneAudioProcessor`.
- Do not let `commitPreparedImportAsPlacement()` infer `trackId` or `startSeconds`.
- Do not reintroduce a drag-drop track-choice modal as the default path.
- Do not persist drag-hover/import-preview state into project state, arrangement state, undo history, playback snapshots, or VST3/ARA state.
- Do not mutate `StandaloneArrangement` during drag hover. Real track creation/import commit happens only after the drop is accepted.
- Do not make blank-area drop depend on hidden knowledge from non-Arrangement widgets.
- Do not break the existing chooser single-file fast path.
- Do not use the broad `ui` runner as the primary automated proof gate while its exit-code issue remains unresolved.

---

## Allowed Files

Primary implementation scope:

- `Source/Standalone/PluginEditor.h`
- `Source/Standalone/PluginEditor.cpp`
- `Source/Standalone/UI/ArrangementViewComponent.h`
- `Source/Standalone/UI/ArrangementViewComponent.cpp`
- `Source/Standalone/UI/TrackPanelComponent.h`
- `Source/Standalone/UI/TrackPanelComponent.cpp`

Test scope:

- `Tests/TestTimelineRenderingPipeline.cpp`
- `Tests/TestArrangementContract.cpp`
- `Tests/TestMain.cpp`

Planning scope:

- This plan
- Its matching test-verification document
- Top-level `.planning` status docs only for synchronization

Stop and ask before touching:

- `Source/PluginProcessor.*`
- `Source/StandaloneArrangement.*`
- `Source/Utils/PlacementActions.*`
- VST3/ARA files

---

## Task 1: Lock The Import UX Contract With Focused Tests

Add focused tests or source guards before implementation changes.

Required coverage:

- `StandaloneImportDrop_UsesDropTrackInsteadOfPrompt`
  - Drag-drop path must resolve placement from drop geometry and must not route through the legacy track picker.
- `StandaloneImportDrop_BlankArrangementAreaTargetsNewTrack`
  - A blank Arrangement-area drop resolves to a "new visible track" target when under `MAX_TRACKS`.
- `StandaloneImportDrop_OutOfArrangementFallsBackToActiveTrack`
  - Top-level non-Arrangement drop keeps a deterministic fallback path.
- `StandaloneImportChooser_SingleFileRemainsActiveTrackFastPath`
  - Single-file chooser import must stay popup-free.
- `StandaloneImportDrop_PreviewIsTransientOnly`
  - Hover/drop preview does not create placements or tracks before commit.

Prefer a focused suite or source-scan guard rather than broad UI execution.

---

## Task 2: Introduce An Explicit Import Drop Target Resolver

Add one Standalone-editor-side resolver that turns drag/drop context into an explicit import target.

Required output shape:

- existing track target
- new-track target
- fallback active-track target
- reject reason (for example: max tracks reached, invalid file, no Arrangement surface hit)

Required inputs:

- file-drag pointer position
- Arrangement lane geometry
- visible track count
- current active track
- track-count limits

The resolver must live in Standalone UI/editor code only. It must end by constructing an explicit `ImportPlacement` decision, never by asking the processor to "figure it out".

---

## Task 3: Add File-Drag Hover Preview For Import

Implement `fileDragEnter` / `fileDragMove` / `fileDragExit` handling for the Standalone editor.

Required behavior:

- hovering over an existing track highlights that target lane;
- hovering over blank Arrangement space previews the pending new-track target;
- hovering outside Arrangement clears import target preview and implies active-track fallback only at actual drop time;
- preview state is cheap, transient, and fully cleared on exit/cancel/drop completion.

Preferred structure:

- keep hover state in `PluginEditor` or a small Arrangement-facing value object;
- let Arrangement rendering consume target-lane preview state;
- if a new-track preview is needed, express it as a transient visual affordance rather than materializing a real track before drop.

---

## Task 4: Replace Legacy Drop Prompt With Placement-By-Target Commit

After the resolver exists, remove the modal track picker from drag-drop.

Required behavior:

- existing-track drop:
  - commit import to resolved `trackId`;
  - use drop time for `timelineStartSeconds` rather than always appending.
- blank-area drop:
  - grow visible track count by one if allowed;
  - resolve the new track id;
  - commit import there and sync active/selected context.
- non-Arrangement drop:
  - keep explicit active-track fallback behavior.

Delete/replace:

- Replace `promptTrackSelectionForDroppedFile(...)` as the normal drag-drop path.
- Preserve it only if a strictly limited internal fallback is still needed during migration; otherwise delete it.

---

## Task 5: Normalize Chooser Import Around The Same Product Rules

The chooser path is already closer to the desired UX, but it must be explicitly aligned with the new contract.

Required behavior:

- single-file chooser import:
  - current active track;
  - no track picker;
  - append or chosen default placement policy stays explicit in editor code.
- multi-file chooser import:
  - keep mode selection explicit;
  - if distributing across tracks requires more visible tracks, create them up to `MAX_TRACKS`;
  - if the request exceeds `MAX_TRACKS`, surface a direct mode-level error instead of per-file track prompts.

This keeps one consistent story:

- spatial target decides drag-drop;
- active track decides chooser default;
- editor always owns the final explicit placement.

---

## Task 6: Focused Verification

Run the matching verification document in order:

1. Static contract gate.
2. Focused `OpenTuneTests` build.
3. Focused import/Arrangement suites.
4. Manual Standalone smoke for the reported DAW-style journeys.

Do not claim the manual L5 journey PASS without user confirmation.

---

## Exit Condition

This task is complete only when:

- dropping onto track N imports into track N without a track-choice modal;
- dropping into Arrangement blank space creates one new visible track and imports there when allowed;
- dropping outside Arrangement still has a deterministic active-track fallback;
- single-file chooser import remains popup-free;
- all placement commits still go through explicit editor-owned `ImportPlacement`;
- automated focused gates pass in the current checkout;
- manual Standalone smoke is either user-confirmed or explicitly left pending.

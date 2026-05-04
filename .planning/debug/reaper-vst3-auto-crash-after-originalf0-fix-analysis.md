---
status: diagnosed
trigger: "Diagnose only: reaper-vst3-auto-crash-after-originalf0-fix"
created: 2026-04-15T11:47:51.2089801+08:00
updated: 2026-04-15T11:55:11.4134762+08:00
---

## Current Focus

hypothesis: the crash comes from erasing on `OpenTuneAudioProcessor::TrackState::AudioClip::notes` through a raw reference returned by `getClipNotesRef()` after `tracksLock_` is released; AUTO now reaches that path because OriginalF0 finally becomes `Ready`
test: verify the full AUTO call chain, enumerate all `vector::erase` sites actually reachable on that chain, and compare them with the recent OriginalF0-related diff
expecting: confirm `setNotes()` is the only shared-state erase on the AUTO path and that recent OriginalF0 work only exposed this path instead of introducing a new erase site
next_action: finalize evidence and return the diagnose-only root cause with container, call chain, and relation to the OriginalF0 fix

## Symptoms

expected: in REAPER VST3, after OriginalF0 is displayed normally, clicking AUTO should apply auto tune to the selection without assertion or host crash
actual: clicking AUTO shows an MSVC Debug Assertion `vector erase iterator outside range`, then the plugin and REAPER crash
errors: MSVC Debug Assertion `vector erase iterator outside range`
reproduction: in REAPER VST3, load state where OriginalF0 is ready and visible, then click AUTO
started: reported after the previous OriginalF0 fix

## Eliminated

- hypothesis: `UndoManager::addAction()` scale+auto merge is the primary crash site
  evidence: `Source/Plugin/PluginEditor.cpp:792` updates scale without pushing undo, and `Source/Utils/UndoAction.h:571-600` only erases `actions_.begin()` on history overflow; this is not the first AUTO-path erase and does not explain the immediate host-specific assertion
  timestamp: 2026-04-15T11:54:15.7508124+08:00

- hypothesis: VST3 editor `backgroundTasks_` / `deferredImportPostProcessQueue_` erases are the AUTO crash site
  evidence: the VST3 AUTO path is `autoTuneRequested() -> applyAutoTuneToSelection() -> correction worker -> consumeCompletedCorrectionResults() -> setNotes()`; the queue erases in `Source/Plugin/PluginEditor.cpp` are timer/import-management code and are not on this call chain
  timestamp: 2026-04-15T11:54:15.7508124+08:00

## Evidence

- timestamp: 2026-04-15T11:48:42.7322622+08:00
  checked: .planning/debug/knowledge-base.md
  found: no knowledge-base file exists yet
  implication: no prior resolved pattern can be used as a shortcut hypothesis

- timestamp: 2026-04-15T11:48:42.7322622+08:00
  checked: .planning/PROJECT.md, .planning/REQUIREMENTS.md, .planning/ROADMAP.md, .planning/STATE.md
  found: current live tree is in v2.1 PianoRoll single-entry/single-VBlank convergence; VST3 editor remains an isolated shell reusing shared `PianoRollComponent` while shared clip truth still lives in processor `tracks_` guarded by `tracksLock_`
  implication: AUTO crash should be explained through shared piano-roll/processor contracts, not through a VST3-only overlay path

- timestamp: 2026-04-15T11:48:42.7322622+08:00
  checked: .planning/codebase/*.md and common-bug-patterns.md
  found: repository docs already flag shared mutable clip state under `tracksLock_` plus VST3 track-0 semantics; the bug-pattern checklist maps this symptom to boundary/state-management candidates such as shared mutation, race, and dual source of truth
  implication: the leading branch is an unsafe shared-vector mutation or stale iterator in the AUTO note-application path

- timestamp: 2026-04-15T11:54:15.7508124+08:00
  checked: `Source/Plugin/PluginEditor.cpp`, `Source/Standalone/UI/PianoRollComponent.cpp`, `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.cpp`
  found: the concrete AUTO chain is `autoTuneRequested()` -> `PianoRollComponent::applyAutoTuneToSelection()` -> worker `NoteGenerator::generate/applyCorrectionToRange()` -> `onVisualVBlankCallback()` -> `consumeCompletedCorrectionResults()` -> `setNotes(completed->notes)`
  implication: the first shared-state note mutation after AUTO completion is `PianoRollComponent::setNotes()`, so that is the highest-probability site for the assertion

- timestamp: 2026-04-15T11:54:15.7508124+08:00
  checked: `Source/Standalone/UI/PianoRollComponent.cpp:2083-2172` and `Source/PluginProcessor.cpp:3307-3348`
  found: `setNotes()` mutates `auto& clipNotes = getCurrentClipNotes();` and then calls `clipNotes.erase(remove_if(...), clipNotes.end())`; `getCurrentClipNotes()` resolves to `processor_->getClipNotesRef(...)`, and `getClipNotesRef()` returns `tracks_[trackId].clips[clipIndex].notes` after a scoped lock has already been destroyed
  implication: AUTO mutates a processor-owned `std::vector<Note>` through an unlocked raw reference, so iterator/range validity is not protected across the erase sequence

- timestamp: 2026-04-15T11:54:15.7508124+08:00
  checked: reachable `erase(...)` sites on the AUTO path versus recent git diff
  found: the other reachable erase sites are `NoteGenerator::generate()` and pitch-curve segment cleanup on local copied vectors, while the recent OriginalF0 diff only changes `Source/Plugin/PluginEditor.cpp` extraction/deferred-postprocess flow and does not touch `PianoRollComponent::setNotes()` or `OpenTuneAudioProcessor::getClipNotesRef()`
  implication: the most likely root cause predates the OriginalF0 fix; that fix mainly removed the earlier gate so VST3 AUTO can now execute into the unsafe shared-note mutation path

## Resolution

root_cause: `PianoRollComponent::setNotes()` erases on the processor-owned clip note container `OpenTuneAudioProcessor::TrackState::AudioClip::notes` via `getClipNotesRef()`, which returns that vector by reference after `tracksLock_` has already been released. In the VST3 AUTO flow this is the first shared-state `vector::erase` site after auto-generated notes come back, so it is the most likely source of the MSVC `vector erase iterator outside range` assertion.
fix:
verification:
files_changed: []

---
status: investigating
trigger: "Diagnose the new local reproduction behind REAPER VST3 AUTO crash."
created: 2026-04-15T12:16:38.6284564+08:00
updated: 2026-04-15T12:16:38.6284564+08:00
---

## Current Focus

hypothesis: workspace state differs from provided context; must confirm which local edits are authoritative before investigating undo corruption
test: compare user-described modified files against actual git status
expecting: mismatch means current tree may not match the reported reproduction path
next_action: ask user whether to proceed with the broader modified tree or restore/restate the intended workspace snapshot

## Symptoms

expected: diagnose whether the remaining failing local AUTO test is a second production bug, corruption from the current patch, or a test-only artifact
actual: `AUTO consumes completed result safely for synced ready clip` now fails after the targeted `setNotes()` fix path is applied; trace shows `processor.getUndoManager()` is healthy in ctor but becomes `currentIndex=0 maxHistory=0` before `insertClipSnapshot()`
errors: `UndoManager::addAction begin size=0 currentIndex=0 maxHistory=0`; later test/process failure after `commitTransaction`
reproduction: run the existing core test `AUTO consumes completed result safely for synced ready clip` with verbose `ctest` after the targeted fix for unlocked note-vector writes in `PianoRollComponent::setNotes()`
started: after applying the targeted fix for unlocked note-vector writes in `PianoRollComponent::setNotes()`

## Eliminated

## Evidence

- timestamp: 2026-04-15T12:16:38.6284564+08:00
  checked: user-provided repro context
  found: prior strong candidate is unlocked mutation of processor-owned `AudioClip::notes` in `PianoRollComponent::setNotes()`, but the local failing test now advances past `setNotes()` and dies in undo transaction flow
  implication: there may be a second bug or the current patch may have shifted corruption later in the AUTO path

- timestamp: 2026-04-15T12:16:38.6284564+08:00
  checked: `git status --short`
  found: modified files in the workspace differ from the context list; actual dirty files include `Source/Plugin/PluginEditor.cpp`, `Source/Plugin/PluginEditor.h`, `Source/PluginProcessor.h`, `Source/Standalone/PluginEditor.cpp`, `Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.cpp`, `Source/Standalone/UI/PianoRollComponent.cpp`, `Source/Utils/UndoAction.h`, `Tests/TestMain.cpp`, plus untracked `QQ20260415-113345.png` and `Source/Services/ImportedClipF0Extraction.h`
  implication: the live tree may not match the reported reproduction snapshot, so further diagnosis risks attributing the failure to the wrong change set without user confirmation

## Resolution

root_cause:
fix:
verification:
files_changed: []

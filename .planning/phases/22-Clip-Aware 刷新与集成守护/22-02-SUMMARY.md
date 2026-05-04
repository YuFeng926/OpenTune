---
phase: 22-Clip-Aware 刷新与集成守护
plan: 02
subsystem: renderer
tags: [pianoroll, renderer, clip-aware, notes, waveform, f0]
requires:
  - phase: 22-Clip-Aware 刷新与集成守护
    provides: Phase 22 verification contract and RED baseline
provides:
  - Unified renderer visible-time-window helper
  - Visible-note-window traversal for drawNotes
affects: [22-03, 22-04]
tech-stack:
  added: []
  patterns: [shared-visible-window-helper, binary-search-note-window]
key-files:
  created: []
  modified:
    - Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
key-decisions:
  - "Waveform / F0 / notes 必须共用 computeVisibleTimeWindow(...)，不能再各算各的可见区。"
  - "drawNotes() 用 std::lower_bound + visible iterator window 替代 full scan。"
patterns-established:
  - "Keep clip-aware logic inside PianoRollRenderer instead of pushing it back into PianoRollComponent."
requirements-completed: [PAINT-02]
duration: 0h 0m
completed: 2026-04-15
---

# Phase 22 Plan 02: renderer clip-aware convergence Summary

**`PianoRollRenderer` 现在已经有统一的 `computeVisibleTimeWindow(...)`，而 `drawNotes()` 也改成了 `std::lower_bound` 驱动的 visible note window 遍历，`PAINT-02` 的 live-tree 缺口已经被结构性填平。**

## Accomplishments

- 在 `PianoRollRenderer.cpp` 增加 `computeVisibleTimeWindow(...)`，统一 notes / waveform / F0 的可见时间窗语义。
- 把 `drawNotes()` 从 full scan 改成 visible iterator window，仅遍历当前可见区内的 notes。
- 保持 `drawWaveform()` / `drawF0Curve()` 的既有 visible-range 业务语义不变，只把它们切到统一 helper。
- fresh `OpenTuneTests.exe` 已证明 `PAINT_02_PianoRollDrawNotesUsesVisibleNoteWindow` 与 `PAINT_02_PianoRollRendererUsesUnifiedVisibleTimeWindowForNotesWaveformAndF0` 为绿。

## Files Created/Modified

- `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp` - 新增 `computeVisibleTimeWindow(...)`，并把 `drawWaveform()` / `drawNotes()` / `drawF0Curve()` 切到同一 helper。

## Decisions Made

- 不在 `PianoRollComponent` 侧复制可见区算法，所有 clip-aware 语义都留在 renderer 内部。
- 对 overlapping visible-start 边界只回退一个前驱 note，而不是重新退回全表扫描。

## Issues Encountered

- 无额外阻塞；Wave 1 已经提前识别并固定了可用 toolchain 与测试产物路径，因此 Wave 2 只需重建并运行 `OpenTuneTests.exe`。

## Self-Check: PASSED

- `computeVisibleTimeWindow(...)` exists and is used by waveform / notes / F0 paths.
- `drawNotes()` no longer relies on `for (const auto& note : notes)` as its primary traversal.
- Phase 22 renderer-level tests passed in fresh `build-phase22-docs/OpenTuneTests.exe`.

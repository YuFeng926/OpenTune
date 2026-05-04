---
phase: 13-旧队列语义归一
verified: 2026-04-14T23:05:00+08:00
status: passed
score: 4/4 must-haves verified
---

# Phase 13: 旧队列语义归一 Verification Report

**Phase Goal:** 保持旧 `RenderCache / pending / revision` 队列核心不变，只让 render-affecting 变化进入它，并阻断 stale publish。

## Current Reading

- 早期 Phase 13 计划曾设想把 `renderRevision / layoutEpoch` 明确接成 queue runtime 主语。
- 当前 live tree 的更优结构没有回补这一层：队列真相继续直接保留在 `RenderCache::Chunk::{desiredRevision,publishedRevision}`，而 Phase 15-18 的 sample-authoritative 边界工作又进一步强化了 publish 正确性。
- 因此当前 phase closure 以“旧队列未被替换、mapping-only 不入队、stale publish 被阻断、没有第二套队列”为准，而不是机械回补旧的 `layoutEpoch` runtime 方案。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | 旧 `RenderCache` 队列核心仍是唯一 render queue | ✓ PASS | `Source/Inference/RenderCache.h/.cpp` still own `PendingJob`, `desiredRevision`, `publishedRevision`, `completeChunkRender()` |
| 2 | plugin clip 的 render-affecting 变化仍进入同一旧队列 | ✓ PASS | `Source/PluginProcessor.cpp:3507` defines `enqueuePluginClipPartialRender()` |
| 3 | mapping-only 不进入 render queue | ✓ PASS | `REV_01_SetPluginClipStartSecondsBumpsMappingRevisionWithoutRenderInvalidation` PASS |
| 4 | stale publish 会回退到 Pending 并重排最新 revision | ✓ PASS | `Source/Inference/RenderCache.cpp:456` and `QUEUE_03_RenderCacheStaleCompletionRequeuesLatestRevision` PASS |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| Phase 13 tests | `build\OpenTuneTests.exe` | `QUEUE_01_EnqueuePluginClipPartialRenderTargetsResolvedClipCache` and `QUEUE_03_RenderCacheStaleCompletionRequeuesLatestRevision` PASS | ✓ PASS |
| Existing queue regressions | `build\OpenTuneTests.exe` | `STAB-01` section 4/4 PASS | ✓ PASS |
| 静态审计 | `rg -n "desiredRevision|publishedRevision|getNextPendingJob|completeChunkRender|enqueuePluginClipPartialRender" Source` | 命中 | ✓ PASS |

## Gate Status

- PASS

---
status: partial
phase: 23-snapshot-region
source:
  - 23-01-SUMMARY.md
  - 23-02-SUMMARY.md
  - 23-03-SUMMARY.md
started: 2026-04-16T14:56:28.3147373+08:00
updated: 2026-04-16T15:03:11.1856531+08:00
---

## Current Test

[testing paused — 3 items outstanding]

## Tests

### 1. 同源多 Region 切换
expected: 在宿主中让同一个 ARA audio source 生成两个 playback region，并让它们指向不同的 source range。依次选中或导入这两个 region 时，OpenTune 应分别显示并播放各自片段，不会把 A 的片段套到 B，也不会两个 region 都落到同一个 clip truth。
result: blocked
blocked_by: prior-phase
reason: "做完所有phase之前，这个人工测试不具备做的条件，先挂起来吧。"

### 2. 编辑批次结束后一次性同步
expected: 在宿主里一次编辑同一个 region 的位置或范围并结束编辑批次后，OpenTune 应直接反映最终结果；不会先短暂显示旧范围、半更新状态，或必须重开、重选后才稳定。
result: [pending]

### 3. Preferred Region 无重写切换
expected: 当两个 region 的内容副本与 revision 不变、只是切换 preferred region 或当前目标 region 时，OpenTune 仍应跟到新的 region truth；导入、显示与播放命中的应是新 region，而不是停留在上一个 region。
result: [pending]

## Summary

total: 3
passed: 0
issues: 0
pending: 2
skipped: 0
blocked: 1

## Gaps

[none yet]

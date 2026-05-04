---
status: testing
phase: 15-样本真相源与冻结边界
source:
  - 15-01-SUMMARY.md
  - 15-02-SUMMARY.md
  - 15-03-SUMMARY.md
started: 2026-04-15T03:28:19.2812544+08:00
updated: 2026-04-15T03:28:19.2812544+08:00
---

## Current Test

number: 1
name: 导入后真实长度保持一致
expected: |
  导入一段音频后，时间轴里的 clip 长度应当和原音频真实长度一致。
  把视图拉到结尾时，不应看到被额外拉长的一小段尾巴，也不应在真实结尾前被提前截断。
awaiting: user response

## Tests

### 1. 导入后真实长度保持一致
expected: 导入一段音频后，时间轴里的 clip 长度应当和原音频真实长度一致；查看结尾时，不会多出被 padding 拉长的尾巴，也不会在真实结尾前被提前截断。
result: [pending]

### 2. 重渲染后尾部不被静默修形
expected: 对一个长度不整齐的片段触发需要重渲染的操作后，播放到结尾应当自然结束；不会多出一小段尾音或静音，也不会在结尾前被截掉。
result: [pending]

## Summary

total: 2
passed: 0
issues: 0
pending: 2
skipped: 0
blocked: 0

## Gaps

[]

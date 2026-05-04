# Test Verification: Memory Optimization Render Pipeline

**Date**: 2026-05-01  
**Scope**: 验证内存优化重构的正确性

## L1 - Static Validation (编译)

| ID | 验证项 | 命令 |
|----|--------|------|
| L1-1 | VST3 Release 编译 | `cmake --build build --config Release --target OpenTune_VST3` |
| L1-2 | Standalone Release 编译 | `cmake --build build --config Release --target OpenTune_Standalone` |
| L1-3 | Tests 编译 | `cmake --build build --config Release --target OpenTuneTests` |

## L2 - Unit Tests

### MaterializationStore 测试

| ID | 测试名 | 验证目标 |
|----|--------|----------|
| M1 | MaterializationStore_CreateDoesNotAllocateDrySignal | createMaterialization 不再创建 drySignalBuffer |
| M2 | MaterializationStore_PlaybackReadSourceReturnsAudioBuffer | getPlaybackReadSource 返回 audioBuffer(44.1kHz) |
| M3 | MaterializationStore_RetireClearsRenderCacheButKeepsAudioBuffer | retire 清空 renderCache 但保留 audioBuffer/pitchCurve |
| M4 | MaterializationStore_ReviveAfterRetirePreservesAudioBuffer | revive 后 audioBuffer/pitchCurve 仍可用 |
| M5 | MaterializationStore_ReplaceAudioNoLongerAcceptsDeviceSampleRate | replaceAudio 签名不含 deviceSampleRate |

### RenderCache 测试

| ID | 测试名 | 验证目标 |
|----|--------|----------|
| R1 | RenderCache_CacheLimitIs256MB | kDefaultGlobalCacheLimitBytes == 256MB |
| R2 | RenderCache_ChunkHasNoResampledAudio | Chunk 结构无 resampledAudio 字段 |
| R3 | RenderCache_OverlayReadsFromChunkAudioAtRenderSampleRate | overlayPublishedAudioForRate 从 chunk.audio 按比率读取 |
| R4 | RenderCache_OverlayWithDifferentTargetSampleRate | 设备SR != 44.1kHz 时仍能正确覆盖 |

### PlaybackReadSource 测试

| ID | 测试名 | 验证目标 |
|----|--------|----------|
| P1 | PlaybackReadSource_HasAudioMethod | hasAudio() 正确判断 audioBuffer 可用性 |
| P2 | PlaybackReadSource_CanReadRequiresAudio | canRead() 依赖 hasAudio() |

### VocoderRenderScheduler 测试

| ID | 测试名 | 验证目标 |
|----|--------|----------|
| V1 | VocoderScheduler_QueueDepthLimit50 | kMaxQueueDepth == 50 |
| V2 | VocoderScheduler_OverflowCallsOnCompleteForDiscarded | 超限丢弃的 job 会收到 onComplete(false) 回调 |

## L3 - Integration Tests

| ID | 测试名 | 验证目标 |
|----|--------|----------|
| I1 | Integration_CreateMaterializationAndReadPlayback | 创建 materialization → getPlaybackReadSource → audioBuffer 可读 |
| I2 | Integration_RetireAndReviveRoundTrip | retire → revive → 播放数据恢复完整 |

## L6 - Regression Suite

| ID | 命令 | 验证目标 |
|----|------|----------|
| L6-1 | `OpenTuneTests.exe` | 全套测试通过 |

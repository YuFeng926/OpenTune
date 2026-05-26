# Test Verification: VST3 silentGap + DML device output 内存修复

**日期:** 2026-05-26  
**状态:** Proposed  
**关联方案:** `.planning/plans/2026-05-26-vst3-silentgap-dml-device-output-memory-fix.md`

## 验证目标

本轮只接受以下合同：

1. VST3 ARA birth 路径重新接回 `silentGaps`，render enqueue 不再退化成整段单 job。
2. DML output 使用 device output binding，不再长期持有历史最大 CPU output 预分配。
3. `GPU First` / `CPU First` 两态保持不变。
4. `VocoderRenderScheduler` 的同 `chunkKey` 去重仍然存在。

## L1 - 静态守护

执行：

```powershell
git diff --check
```

预期：

- Exit code 0
- 无空白错误
- 无 patch 格式错误

## L2 - 架构守护测试

构建：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
```

必须新增或更新以下守护：

- `AraBirthPath_DetectsSilentGapsBeforeCommit`
- `DmlVocoder_UsesDeviceOutputBinding`
- `RenderingPriority_RemainsGpuFirstAndCpuFirstOnly`
- `VocoderScheduler_QueueDepthLimit50`
- `AraFinal_BirthPathOwnsOriginalF0Release`

预期：

- 新增守护全部 PASS
- 现有 ARA final / memory focused suites 不因本轮改动退化

## L3 - 处理器与后端 focused suites

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
```

预期：

- `processor` PASS
- `memory` PASS
- 已有 `F0InferenceService::releaseImmediately()` 守护仍 PASS

## L4 - 构建矩阵

执行：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

预期：

- ARA VST3 PASS
- non-ARA VST3 PASS
- Standalone PASS

## L5 - 宿主行为验证

本层在用户确认前不能标记 PASS。

### VST3 ARA 长音频 birth 分块

步骤：

1. 加载本轮 ARA VST3。
2. 在 ARA 宿主中导入约 3-5 分钟音频。
3. 触发 ARA birth / 自动 OriginalF0。
4. 检查日志里 render pending 不再出现 `start=0.000 end=<整段总时长>` 单条整段任务。
5. 检查日志里出现多个按静息处切分的 chunk render enqueue。

### DML 内存行为

步骤：

1. 选择 `GPU First`。
2. 执行同样的长音频 AUTO 渲染。
3. 观察 RAM 高水位是否明显低于旧版。
4. 切换到 `CPU First`，确认后端能正常切回 CPU，不受 DML output 改造影响。

用户确认口径：

```text
宿主侧 VST3 ARA 分块与 DML 内存行为已完成验证。请确认结果：符合 / 不符合
```

## L6 - 完成证据

收尾时必须给出：

- `silentGaps` 接回点位
- DML device output 绑定点位
- 渲染优先级两态保持不变的说明
- 队列 dedup 仍完好的说明
- focused suite 与 build 结果
- 宿主 L5 是否完成；未完成必须明确标记 pending

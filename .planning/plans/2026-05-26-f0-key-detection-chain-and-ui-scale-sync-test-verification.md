# F0 提取主链调式检测接回与 UI 调式同步验证

**日期:** 2026-05-26  
**状态:** Proposed  
**关联方案:** `.planning/plans/2026-05-26-f0-key-detection-chain-and-ui-scale-sync.md`

## 验证目标

证明两件事：

1. ARA `birthAraMaterializationWithOriginalF0(...)` 已重新接入既有调式智能检测契约。
2. 当前激活素材在异步 `detectedKey` 写回后，Standalone UI 能自动显示最新调式；VST3 现有统一同步链未被破坏。

## L1 静态检查

```powershell
git diff --check
```

预期：

- Exit code 0
- 无空白错误

## L2 focused architecture/source guards

构建：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

必须覆盖：

- `KeyDetection_AraBirthPathRunsChromaDetectionBeforeReady`
- `ScaleSync_StandaloneTimerPullsDetectedKeyForActivePlacement`

预期：

- 两项 PASS
- 不引入新的 architecture 回归

## L3 focused behavior regression

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
```

预期：

- shared processor suite 继续 PASS
- 本轮没有破坏非 ARA `requestMaterializationRefresh(...)` 主链

## L4 构建回归

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

预期：

- ARA VST3 PASS
- Standalone PASS

## L5 人工行为口径

本轮若未完成宿主手工验证，必须明确标记为 pending，不得口头宣称已验证。

建议口径：

- Standalone：导入音频后，F0 完成后无需切换选区即可看到调式显示更新
- ARA：触发 auto-birth 后，若 F0 成功完成，则 born materialization 应有 `detectedKey`

## 完成证据

交付时必须说明：

- 哪个主链补回了 `ChromaKeyDetector`
- Standalone 哪个心跳路径补回了 scale pull-sync
- 跑了哪些 suite / build
- 哪些人工宿主验证仍为 pending

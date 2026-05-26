# VST3 ARA 自动 OriginalF0 最小正确架构测试与验证

**日期：** 2026-05-25  
**状态：** Proposed  
**关联方案：** `.planning/plans/2026-05-25-vst3-ara-originalf0-minimal-pipeline.md`

## 范围

验证重构后的 VST3 ARA 自动 `OriginalF0` 路径是否满足以下目标：

- 只 birth 一次完整可播放 materialization 音频缓存
- RMVPE 输入直接来自 ARA 读取过程，而不是从已 birth 音频回读重建
- VST3 ARA 自动路径不再进入 GAME reference-note generation
- `OriginalF0` 成功可见后，RMVPE 与分析专用闲置资源立即释放

本验证文档**不宣称**修改了 Standalone 的 GAME 行为。

## 需求断言

- **ARA-F0-01：** VST3 ARA 自动读取对目标 source window 只 birth 一次 playable materialization。
- **ARA-F0-02：** VST3 ARA 自动 `OriginalF0` 不再从已 birth 的 materialization `audioBuffer` 回建分析输入。
- **ARA-F0-03：** VST3 ARA 自动 `OriginalF0` 不会 enqueue GAME note generation。
- **ARA-F0-04：** `OriginalF0State::Ready` 在播放可见完成状态之前完成提交。
- **ARA-F0-05：** F0 曲线可见后，RMVPE 推理资源会被显式释放。
- **ARA-F0-06：** 自动链结束后，进程内只保留播放所需 audio/cache 状态。
- **ARA-F0-07：** 现有 ARA 播放链仍从 born materialization playback buffer 读取干声。

## L1 静态检查

命令：

```powershell
git diff --check
```

预期：

- Exit code 0
- 无空白错误和 patch 格式问题

## L2 契约测试

构建命令：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

运行命令：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

预期重点：

- VST3 ARA 自动路径不再依赖 `snapshot.audioBuffer -> monoAudio` 这条回读分析链
- VST3 ARA 自动路径不再触达 `ensureNoteGeneratorReady()` / `noteGeneratorPool_`
- ARA 自动链存在显式的“成功后立即释放 RMVPE”步骤
- Standalone 或 non-ARA 需要保留的 note-generator 契约仍然可达
- playback read source 仍是 `materialization.audioBuffer` + `renderCache`

## L3 构建冒烟

构建命令：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

预期：

- ARA VST3 构建成功
- non-ARA VST3 构建成功
- Standalone 构建成功
- 没有引入新的 build-mode 耦合

## L4 聚焦运行时 / Processor 测试

运行命令：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
```

预期：

- ARA auto-read / auto-birth 的 processor guard 通过
- memory 方向测试或诊断能证明 ARA 自动路径不再 hidden enqueue GAME
- playback/materialization ownership 相关测试仍通过
- ARA renderer 的 playback-read 契约无回归

## L5 宿主旅程验证

这一层在拿到宿主证据前不能宣称 PASS，需要用户确认。

### Studio One ARA 长音频自动 OriginalF0 旅程

1. 在 Studio One 的 ARA 工作流中加载重建后的 OpenTune ARA VST3。
2. 对一个 **大于 5 分钟** 的音频 event 执行 `Read Audio`。
3. 确认自动链能完成 `OriginalF0`，且不会进入 hidden GAME note generation。
4. 确认日志中依次出现：
   - pipeline start
   - playable materialization birth
   - RMVPE commit
   - F0 可见后显式释放 RMVPE
5. 确认完成后可正常播放。
6. 确认 `OriginalF0` 可见后，内存从推理高峰回落，而不是继续被闲置推理资源占用。

用户确认口径：

```text
用户旅程 “Studio One ARA 长音频自动 OriginalF0 最小链路” 已完成验证。请确认是否符合您的预期：符合 / 不符合
```

## L6 更宽回归边界

命令：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

预期：

- 列出的命令全部通过
- ARA 播放仍从 born materialization playback buffer 读取
- VST3 ARA 自动路径不再承担 hidden GAME 的内存成本
- `ui` suite 仍不纳入本轮 pass claim，除非现有 runner 退出问题先被解释清楚

## 必须留存的证据

本方案实施完成前，验证至少要能留存以下证据：

- 证明 ARA 自动路径不再 enqueue GAME 的日志
- 证明 F0 可见后显式释放 RMVPE 的日志
- 自动读取前、RMVPE 运行中、F0 可见后的内存观测
- 证明释放推理资源后，仍能从 born materialization playback buffer 正常播放的证据

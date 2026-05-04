---
status: awaiting_human_verify
trigger: "Investigate issue: reaper-vst3-record-originalf0-missing"
created: 2026-04-15T10:15:31.4348741+08:00
updated: 2026-04-15T11:06:19.3622137+08:00
---

## Current Focus

hypothesis: 新日志已证明剩余断点不只是 silent-gap 提取算法，而是 editor 层暴露的 `initializeInferenceIfNeeded() -> getF0Service()` 双步契约在 REAPER/VST3 下会失真；现在已把“导入 clip 的 OriginalF0 提取”收回到 processor 内部原子执行，同时保留 silent-gap-aware 共享实现
test: 使用最新构建的 VST3 在 REAPER 中重做“插入音频 item -> 点击录音/读取音频”路径，并检查日志里是否出现 `RecordTrace: OriginalF0 extraction committed`，或新的 processor-owned failure diagnostics（`F0 initialization unavailable` / `F0 service missing after init`）
expecting: 如果 processor-owned 提取契约修复了 REAPER 下的失真，OriginalF0 会随 spinner 一起正常完成；若仍失败，新日志会把失败精确收敛到 processor 实例级别，而不是只剩 editor 层的空指针症状
next_action: request another REAPER verification with the rebuilt VST3 and the new processor-level F0 diagnostics

## Symptoms

expected: 在 REAPER 的 VST3/ARA 场景中，点击录音按钮后应读取当前音频 item，对应 clip 的波形出现在钢琴卷帘中，进入 OriginalF0 提取流程，阻塞动画可见并在提取结束后退出，最终界面显示 OriginalF0。
actual: 点击录音按钮后，OriginalF0 不可见，阻塞动画也不可见；用户观察上像是读取/提取/显示链路至少有一段没有被触发或没有打通。
errors: 用户表示日志里有异常线索。已有日志摘录：`OpenTune2026-04-15_10-03-20.log` 只有启动和 render worker 启停；`OpenTune2026-04-15_10-03-25.log` 显示 ARA audio import success、AudioSource->Clip binding 注册成功、F0 inference service 初始化成功，但未见用户描述的“点击录音按钮后进入 OriginalF0 提取并显示”的明确后续链路证据。日志里也有 DirectML.dll not found，但随后明确回退 CPU 并成功加载 RMVPE，不应直接等同于本问题根因，除非找到证据证明它会切断录音按钮链路。
reproduction: 在 REAPER 中导入音频，在音频 item 上插入片段 FX，插入 OpenTune VST3 插件，点击录音按钮。
started: 最近几个 phase 后开始出现，属于回归。

## Eliminated

## Evidence

- timestamp: 2026-04-15T10:15:31.4348741+08:00
  checked: user-provided symptoms and logs summary
  found: ARA import, AudioSource->Clip binding, and F0 inference service init happen before the click path under investigation; no evidence yet that record-button-triggered OriginalF0 chain runs.
  implication: regression is downstream of basic import/service boot, likely in record-button dispatch, extract trigger conditions, publish, or UI visual loop.

- timestamp: 2026-04-15T10:15:31.4348741+08:00
  checked: `.planning/PROJECT.md`, `.planning/REQUIREMENTS.md`, `.planning/ROADMAP.md`, `.planning/STATE.md`, `.planning/codebase/ARCHITECTURE.md`, `.planning/codebase/STRUCTURE.md`, `.planning/codebase/VST3Merge.md`, `.planning/codebase/INTEGRATIONS.md`
  found: live-tree intent says VST3 editor shell must stay isolated while shared processor remains the truth source for clip/render/import; Phase 20/21 recently rewired PianoRoll to unified invalidation + single VBlank and explicitly removed editor-side visual clock responsibility.
  implication: this regression can plausibly come either from VST3 editor->shared processor call-chain breakage or from UI visibility no longer being driven after Phase 20/21.

- timestamp: 2026-04-15T10:15:31.4348741+08:00
  checked: `.planning/debug/knowledge-base.md`, `.planning/debug/resolved/vst3-plugin-editor-isolation-regression.md`
  found: the only resolved VST3 pattern is a same-day fix that moved plugin editor/factory sources out of shared code after wrapper-specific macro isolation was broken.
  implication: recent VST3/editor source relocation is a credible regression origin, but the known pattern explains wrong shell loading rather than missing OriginalF0 by itself; must test whether functionality was dropped during that move.

- timestamp: 2026-04-15T10:16:40.7774058+08:00
  checked: `C:\Users\TP-RD\AppData\Roaming\OpenTune\Logs\OpenTune2026-04-15_10-03-25.log`
  found: real REAPER session log contains `RecordTrace: ARA audio imported...` and `Registered AudioSource -> Clip binding`, plus F0 service initialization, proving the record button reached `recordRequested()` and completed ARA import; what is missing is any later visual or extraction success/failure evidence.
  implication: root cause is not “record button never fired” and not basic ARA source access; the break is after import, in extraction completion visibility and/or post-import UI flush.

- timestamp: 2026-04-15T10:16:40.7774058+08:00
  checked: `Source/Plugin/PluginEditor.cpp:804-918`, `Source/Plugin/PluginEditor.cpp:1343-1539`, `Source/Standalone/PluginEditor.cpp:828-1005`, `Source/Standalone/PluginEditor.cpp:2349-2617`, `Source/Standalone/UI/PianoRollComponent.cpp:1146-1421`, `Source/Standalone/UI/TransportBarComponent.cpp:830-855`, `Source/Standalone/UI/TransportBarComponent.cpp:1116-1118`
  found: VST3 record path definitely imports audio, syncs clip context, enqueues deferred post-process, and submits RMVPE extraction; but unlike Standalone, the VST3 shell no longer drives any PianoRoll visual tick after Phase 21, does not propagate render/progress state into PianoRoll, and relies on pending invalidation being drained solely by `PianoRollComponent`'s `VBlankAttachment`.
  implication: VST3 path currently lacks an independently verified post-import UI flush path; if REAPER does not deliver effective VBlank callbacks for the plugin peer, waveform and OriginalF0 visibility both stall even though import/extraction logic ran.

- timestamp: 2026-04-15T10:16:40.7774058+08:00
  checked: `git show 86011548d8c5509a3da05719b66723cb991c23b6 -- Source/Plugin/PluginEditor.cpp`, `.planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md`, `.planning/phases/21-单 VBlank 视觉循环/21-TEST-VERIFICATION.md`
  found: Phase 21 explicitly removed `pianoRoll_.onHeartbeatTick()` from the VST3 editor timer and declared success using component tests + static grep only; `L5` host verification was explicitly marked not applicable, and no REAPER/VST3 runtime regression covered the editor peer actually delivering VBlank-driven flushes.
  implication: the strongest regression candidate is Phase 21 itself: VST3 shell cadence was cut over based on component-level proof, but the real REAPER host path was never validated end-to-end.

- timestamp: 2026-04-15T10:33:08.2345012+08:00
  checked: `Source/Plugin/PluginEditor.h`, `Source/Plugin/PluginEditor.cpp`
  found: VST3 deferred post-process queue now carries OriginalF0 extraction context, record/import and ARA replace paths no longer call `requestOriginalF0ExtractionForImport(...)` immediately, and `syncClipToPianoRoll()` now issues an immediate repaint after clip clear/sync.
  implication: VST3 import flow is structurally aligned with Standalone's "post-process first, extract second" order and no longer relies on a later heartbeat/VBlank to publish the first imported clip frame.

- timestamp: 2026-04-15T10:33:08.2345012+08:00
  checked: `cmake --build build-debug-vs-fresh --config Debug --target OpenTune_VST3 OpenTuneTests`, `ctest --test-dir build-debug-vs-fresh -C Debug --output-on-failure -R OpenTuneCoreTests`, `cmake --build build-debug-vs-fresh --config Debug --target OpenTune_Standalone`
  found: modified VST3 target, existing core regression suite, and Standalone target all build successfully; `OpenTuneCoreTests` passes.
  implication: the fix is compile-valid, does not break the shared test target, and preserves Standalone build integrity before host-side manual verification.

- timestamp: 2026-04-15T10:42:09.7858963+08:00
  checked: user human-verification checkpoint response with `OpenTune2026-04-15_10-38-46.log`, `OpenTune2026-04-15_10-38-56.log`, `OpenTune2026-04-15_10-39-12.log`
  found: first-round fix restored imported clip waveform and the blocking extraction prompt lifecycle in REAPER VST3, but OriginalF0 is still invisible and the user still cannot find direct evidence that OriginalF0 was successfully published back to the UI.
  implication: the prior hypothesis only explained the missing first-frame visual publish/spinner path; the remaining bug is narrower and isolated to OriginalF0 generation, commit, sync retention, or PianoRoll display gating.

- timestamp: 2026-04-15T10:47:56.4700314+08:00
  checked: `Source/Plugin/PluginEditor.cpp:1369-1545`, `Source/Standalone/PluginEditor.cpp:2349-2589`, `Source/PluginProcessor.cpp:2878-2917`, `Source/Standalone/UI/PianoRollComponent.h:373-379`
  found: VST3 extraction callback returns immediately on `!result.success`, so downstream `setClipPitchCurveById`/`pianoRoll_.setPitchCurve(...)` 根本不会执行；与此同时，Standalone 导入提取会基于 `silentGaps` 分段提取并重建整条 F0，而 VST3 仍忽略 `silentGaps` 对整段音频做一次性提取。新增日志也证明 overlay 已恢复但仍无任何 publish 成功证据。
  implication: remaining failure is upstream of clip commit/UI paint, and the strongest concrete mechanism is VST3 generation path itself failing before publish because it diverged from the Standalone silent-gap-aware extraction contract.

- timestamp: 2026-04-15T10:53:38.6146486+08:00
  checked: `Source/Services/ImportedClipF0Extraction.h`, `Source/Plugin/PluginEditor.cpp`, `Source/Standalone/PluginEditor.cpp`, `cmake --build build-debug-vs-fresh --config Debug --target OpenTune_VST3 OpenTune_Standalone OpenTuneTests`, `ctest --test-dir build-debug-vs-fresh -C Debug --output-on-failure -R OpenTuneCoreTests`
  found: Standalone/VST3 现在共用同一条 silent-gap-aware 导入 F0 提取实现；VST3 新增了 extraction failed / ready-state commit failed / pitchCurve commit failed / extraction committed 的 `RecordTrace` 日志。更新后的 `OpenTune_VST3`、`OpenTune_Standalone`、`OpenTuneTests` 均成功编译，`OpenTuneCoreTests` 通过。
  implication: local code and regression evidence support the fix, and the remaining unknown is only REAPER host-side end-to-end behavior with the rebuilt VST3 bundle.

- timestamp: 2026-04-15T11:06:19.3622137+08:00
  checked: user-provided `OpenTune2026-04-15_10-57-51.log`, plus `Source/PluginProcessor.cpp:632-747`, `Source/PluginProcessor.h:576-579`, `Source/Plugin/PluginEditor.cpp`, `Source/Standalone/PluginEditor.cpp`
  found: REAPER 日志明确显示 `RecordTrace: OriginalF0 extraction failed ... reason=f0_service_unavailable`，证明上一轮 silent-gap-aware 修复虽已生效，但 editor 侧仍依赖 `initializeInferenceIfNeeded()` 与 `getF0Service()` 的双步契约；该契约在宿主真实运行中会让“已初始化”和“可取到 service 指针”脱钩。现已把导入 OriginalF0 提取收回到 processor 的单一 API `extractImportedClipOriginalF0(...)`，由 processor 内部统一完成 readiness 检查、service 获取和共享 helper 调用，并补加 processor 实例级失败日志。
  implication: 根因从“算法路径错误”进一步收敛为“processor 内部推理服务所有权被 editor 双步访问泄漏”；修复后的唯一剩余验证点是 REAPER 下 processor-owned 提取契约是否真正恢复 OriginalF0 commit。

## Resolution

root_cause: VST3 导入链路同时存在两层断点：其一是导入 OriginalF0 仍走旧的整段音频提取，未复用 `silentGaps`；其二也是 REAPER 实测真正打断发布的关键，是 editor 层把 processor 推理服务暴露成 `initializeInferenceIfNeeded()` 与 `getF0Service()` 双步访问，导致宿主真实运行中 readiness 与 service 所有权脱钩，最终在 commit 前直接报 `f0_service_unavailable`。
fix: 先新增共享 `ImportedClipF0Extraction` helper，让 Standalone/VST3 共用 silent-gap-aware 分段提取；随后再把“导入 clip 的 OriginalF0 提取”收回到 `OpenTuneAudioProcessor::extractImportedClipOriginalF0(...)`，由 processor 内部原子完成 readiness 检查、service 获取与 helper 调用，并为 VST3 增补 editor-level 与 processor-level 的 `RecordTrace` 诊断。
verification: `cmake --build build-debug-vs-fresh --config Debug --target OpenTune_VST3 OpenTune_Standalone OpenTuneTests` passed; `ctest --test-dir build-debug-vs-fresh -C Debug --output-on-failure -R OpenTuneCoreTests` passed. Remaining verification is REAPER manual reproduction with the rebuilt VST3 and the new processor-level `RecordTrace` diagnostics.
  files_changed: [Source/Services/ImportedClipF0Extraction.h, Source/PluginProcessor.h, Source/PluginProcessor.cpp, Source/Plugin/PluginEditor.cpp, Source/Standalone/PluginEditor.cpp]

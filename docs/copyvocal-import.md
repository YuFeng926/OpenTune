# 在 OpenTune 中导入 CopyVocal 结果（`.cvf`）

本页是面向使用者的快速上手说明。`.cvf` 格式本身由 CopyVocal 定义，权威文档见
CopyVocal 仓库的 `docs/copyvocal-bundle-v1.md`。

OpenTune 导入 `.cvf` 后得到的是一个**可继续编辑的工程**：学生原始音频、音符、
原始 F0、CopyVocal 的目标 F0（作为音高修正段）以及教师对齐得到的时间网格都会
进入 OpenTune 的编辑模型。它不是把 `student.corrected.wav` 当成普通音频导入。

---

## 1. 在 CopyVocal 侧生成 `.cvf`

### 方法 A：运行任务时顺便导出

```powershell
.\run.bat --teacher ".\Teacher.wav" --student ".\Student.wav" `
          --output ".\outputs\short" --export-cvf
```

产物：`.\outputs\short\copyvocal.cvf`。导出结果也会写入 `job.json` 的
`stages.opentune_bundle`，包含 note / handle / frame 数量。

### 方法 B：为已有输出补导出（不重新推理）

```powershell
D:\miniconda3\envs\vocal2midi\python.exe -m application.opentune_bundle ".\outputs\short"
```

输出目录需要已经包含 `job.json`、`student.analysis.json`、`alignment.json` 和
`target_pitch.npz`。默认写到 `<output>\copyvocal.cvf`，可用 `-o` 指定路径。

> `.cvf` 里记录的是学生参考音频的**绝对路径**，请保持 CopyVocal 输出目录不动，
> 或把整个输出目录一起迁移。

### 方法 C：在 CopyVocal GUI 中导出

- 运行前在 **设置 → 运行选项** 打开 **同时导出 CopyVocal 文件 (.cvf)**，任务完成后
  会随输出一起写出 `copyvocal.cvf`；
- 对已经完成的输出，在 **结果** 页点击 **导出 CopyVocal 文件** 即可补导出。

---

## 2. 在 OpenTune 侧导入

两种方式等价：

- 菜单 **File → Import CopyVocal Bundle...**，选择 `.cvf` 文件；
- 直接把 `.cvf` 拖进 OpenTune 窗口。

如果当前工程有未保存修改，OpenTune 会先询问保存 / 不保存 / 取消，与打开普通
工程的行为一致。导入在后台完成，完成后自动切到新工程。

导入后应能看到：

| 内容 | 来源 |
| --- | --- |
| 音符（音高、起止） | 教师 GAME notes（失败时回退学生 notes），音高取目标 F0 中位数 |
| 原始 F0 曲线 | `f0.original`，已标记为 Ready，不会重新推理 |
| 音高修正段（手绘） | `f0.target`，仅在原始 F0 有声的连续区间生成 |
| 时间网格 | `timeGrid.handles`，教师对齐锚点，可继续编辑 |
| 工程源音频 | 学生参考音频补零/裁剪到 CopyVocal 输出时长 |

---

## 3. 关于时间与音频长度

CopyVocal 完整修正后的输出长度等于教师长度，通常不等于学生录音长度；而 OpenTune
要求 clip 的源时长与输出时长一致。因此导入时会：

1. 把学生参考音频补零或裁剪到 `clocks.projectSourceSeconds`（= 输出时长）；
2. 补零区间的 F0 保持为 0，不产生虚假音符或修正；
3. `timeGrid` 保持 CopyVocal 的映射，导入后可继续拖动编辑。

这些操作只影响 OpenTune 内存中的工程媒体，不修改 CopyVocal 的任何原始文件。

---

## 4. 保存工程后的行为

导入后可以像普通工程一样保存为 `.otproj`、编辑音符和音高、导出渲染。

- 原始 F0 与目标修正段会随工程持久化；重新打开工程不需要 CopyVocal。
- v1 的 `words` / `phonemes` 只被解析，不在界面显示（OpenTune 当前没有歌词模型）。
- 如果之后在 OpenTune 里对同一段音频重新提取 F0，原始 F0 会被重算，但目标修正段
  仍保留在工程中。

---

## 5. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| 提示 `unsupported format version` | `.cvf` 的 `formatVersion` 高于当前 OpenTune 支持的版本，升级 OpenTune |
| 提示无法解码学生媒体 | `media.student.path` 指向的 WAV 被移动或删除；保持 CopyVocal 输出目录完整 |
| 提示时长不一致 | 学生参考音频与 `projectSourceSeconds` 偏差超过容差；重新导出 `.cvf` |
| 导入后没有音符 | CopyVocal 运行时加了 `--no-game` 且学生分析也没有可用音符；重新运行并保留音符提取 |
| 音高修正段为空 | 目标 F0 在原始 F0 有声区间内全为 0，通常是该段没有可修正内容 |

---

## 6. 时间映射的实现

导入的工程通常带非恒等 `timeGrid`。OpenTune 把该映射烘焙进 Stage 1 的 conditioning
与 F0，由声码器一次合成输出时间轴的音频，不做波形时间伸缩。实现细节与取舍见
[`time-grid-conditioning-warp.md`](time-grid-conditioning-warp.md)。


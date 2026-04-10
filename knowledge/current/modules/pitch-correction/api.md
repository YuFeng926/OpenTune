---
module: pitch-correction
type: api
version: 1.0
updated: 2026-04-10
sources:
  - Source/Utils/PitchCurve.h
  - Source/Utils/PitchCurve.cpp
  - Source/Utils/PitchUtils.h
  - Source/Utils/Note.h
  - Source/Utils/NoteGenerator.h
  - Source/Utils/NoteGenerator.cpp
  - Source/Utils/SimdPerceptualPitchEstimator.h
  - Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h
  - Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.cpp
---

# pitch-correction -- API Reference

> 本模块无 HTTP Controller，此文档记录音高修正算法层对外暴露的编程接口契约。
> 所有类型位于 `namespace OpenTune`。

---

## 1. PitchUtils (工具函数)

**文件**: `Source/Utils/PitchUtils.h`

| 函数签名 | 说明 |
|---|---|
| `float mixRetune(float shiftedF0, float targetF0, float retuneSpeed)` | 在 log2 空间中按 `retuneSpeed`(0.0--1.0）混合偏移后的原始 F0 与目标平直 F0。0 = 保留全部原始细节，1 = 完全平直 |
| `float freqToMidi(float freq)` | 频率 (Hz) -> MIDI 编号 (float)。公式: `69 + 12 * log2(freq / 440)` |
| `float midiToFreq(float midi)` | MIDI 编号 -> 频率 (Hz)。公式: `440 * 2^((midi - 69) / 12)` |

所有函数均为 `inline`，可安全在任意线程调用。对 `freq <= 0` 或 `midi <= 0` 输入返回 `0.0f`。

---

## 2. PitchCurveSnapshot (不可变快照 -- 只读)

**文件**: `Source/Utils/PitchCurve.h:37-109`

### 2.1 构造

```cpp
PitchCurveSnapshot(
    std::vector<float> originalF0,
    std::vector<float> originalEnergy,
    std::vector<CorrectedSegment> correctedSegments,
    int hopSize,
    double sampleRate,
    uint64_t renderGeneration = 0);
```

所有成员为 `const`，构造后不可变。

### 2.2 Getter 方法

| 方法 | 返回类型 | 说明 |
|---|---|---|
| `getOriginalF0()` | `const vector<float>&` | 原始 F0 数组 (Hz，100 fps) |
| `getOriginalEnergy()` | `const vector<float>&` | 原始逐帧 RMS 能量 |
| `getCorrectedSegments()` | `const vector<CorrectedSegment>&` | 已排序的修正段列表 |
| `getHopSize()` | `int` | hop 大小 (samples) |
| `getSampleRate()` | `double` | F0 提取采样率 |
| `getRenderGeneration()` | `uint64_t` | 单调递增渲染代数，驱动缓存失效 |

### 2.3 查询方法

| 方法 | 返回类型 | 说明 |
|---|---|---|
| `isEmpty()` | `bool` | originalF0 是否为空 |
| `size()` | `size_t` | originalF0 帧数 |
| `hasAnyCorrection()` | `bool` | 是否存在任何修正段 |
| `hasCorrectionInRange(int startFrame, int endFrame)` | `bool` | 指定帧范围是否有修正段重叠 |
| `hasRenderableCorrectedF0()` | `bool` | 同 `hasAnyCorrection()` |
| `hasCorrectedVisibleInRange(double startSec, double endSec)` | `bool` | 指定时间范围是否有可见修正 |
| `getCorrectedVisibleTimeBounds(double& outStart, double& outEnd)` | `bool` | 获取所有修正段的时间边界 |
| `getCorrectedVisibleOverlapInRange(double start, double end, double& outStart, double& outEnd)` | `bool` | 计算给定时间范围与修正区域的交集 |
| `shouldDrawCorrectedF0(bool& outUseMask)` | `bool` | UI 是否应绘制修正 F0（outUseMask 始终 false） |
| `getMemoryUsage()` | `size_t` | 快照内存占用估算 (bytes) |

### 2.4 渲染方法

```cpp
void renderF0Range(int startFrame, int endFrame,
                   std::function<void(int frameStart, const float* data, int length)> callback) const;
```

- 遍历 `[startFrame, endFrame)` 范围，在修正段存在时回调修正 F0，否则回调原始 F0
- 对 `LineAnchor` 类型且 `retuneSpeed >= 0` 的段，实时混合 `mixRetune(originalF0, targetF0, retuneSpeed)`
- **线程安全**: 快照本身不可变，可在任意线程安全调用

```cpp
void renderCorrectedOnlyRange(int startFrame, int endFrame,
                              std::function<void(int, const float*, int)> callback) const;
```

- 同 `renderF0Range`，但无修正区域回调 `0.0f` 填充而非原始 F0

---

## 3. PitchCurve (可变外壳 -- COW 发布者)

**文件**: `Source/Utils/PitchCurve.h:111-381`

### 3.1 快照读取

```cpp
std::shared_ptr<const PitchCurveSnapshot> getSnapshot() const;
```

- 通过 `std::atomic_load` 获取当前快照，**lock-free**，可安全在音频线程调用
- 便捷转发方法 (`isEmpty()`, `size()`, `renderF0Range()`, ...) 均委托到 `getSnapshot()`

### 3.2 原始数据设置

| 方法 | 说明 |
|---|---|
| `setOriginalF0(const vector<float>& f0)` | 设置完整原始 F0，自动对齐 energy 尺寸 |
| `setOriginalEnergy(const vector<float>& energy)` | 设置完整原始能量，自动截断/填充至 F0 尺寸 |
| `setOriginalF0Range(size_t startFrame, const vector<float>& fragment)` | 局部更新 F0（增量导入） |
| `setOriginalEnergyRange(size_t startFrame, const vector<float>& fragment)` | 局部更新能量（增量导入） |
| `setHopSize(int hopSize)` | 更新 hop size |
| `setSampleRate(double sampleRate)` | 更新采样率 |

所有 setter 均为 COW: 创建新快照并通过 `std::atomic_store` 原子发布。

### 3.3 修正操作

```cpp
void applyCorrectionToRange(
    const std::vector<Note>& notes,
    int startFrame, int endFrame,
    float retuneSpeed,
    float vibratoDepth = 0.0f,
    float vibratoRate = 7.5f,
    double audioSampleRate = 44100.0);
```

- **核心修正入口**。执行五阶段修正流程 (斜率旋转 -> 音高偏移 -> 颤音 LFO -> Retune 混合 -> 过渡平滑)
- 为每个音符独立创建 `CorrectedSegment`（`Source::NoteBased`）
- 自动添加 10 帧 (~100ms) 过渡段
- 增加 `renderGeneration`

```cpp
void setManualCorrectionRange(int startFrame, int endFrame,
                              const vector<float>& f0Data,
                              CorrectedSegment::Source source);

void setManualCorrectionRange(int startFrame, int endFrame,
                              const vector<float>& f0Data,
                              CorrectedSegment::Source source,
                              float retuneSpeed);
```

- 用于 HandDraw / LineAnchor 手动绘制修正
- 清除目标范围已有段，插入新段 + 过渡段

```cpp
void clearCorrectionRange(int startFrame, int endFrame);
```

- 清除指定范围修正段，保留范围外部分（自动裁剪边界段）

```cpp
void clearAllCorrections();
```

- 清除所有修正段，增加 `renderGeneration`

```cpp
void replaceCorrectedSegments(const vector<CorrectedSegment>& segments);
```

- 整体替换修正段列表（用于 undo/redo），自动排序

```cpp
void restoreCorrectedSegment(const CorrectedSegment& segment);
```

- 恢复单个修正段（undo），先清除重叠区域再插入

```cpp
void clear();
```

- 完全重置（清空 F0、energy、修正段），重置 hopSize=512, sampleRate=16000

---

## 4. Note / NoteSequence

**文件**: `Source/Utils/Note.h`

### 4.1 Note

| 方法 | 返回类型 | 说明 |
|---|---|---|
| `getDuration()` | `double` | `endTime - startTime` (秒) |
| `getAdjustedPitch()` | `float` | `pitch * 2^(pitchOffset / 12)` 含偏移的实际音高 (Hz) |
| `getMidiNote()` | `int` | 调整后音高的 MIDI 编号 (四舍五入) |
| `getBaseMidiNote()` | `int` | 基准 pitch 的 MIDI 编号 |
| `static midiToFrequency(int midiNote)` | `float` | MIDI -> Hz |
| `static frequencyToMidi(float freq)` | `int` | Hz -> MIDI (四舍五入) |

### 4.2 NoteSequence

| 方法 | 说明 |
|---|---|
| `insertNoteSorted(const Note&)` | 插入并自动排序 + 去重叠 |
| `setNotesSorted(const vector<Note>&)` | 批量设置并规范化 |
| `replaceRangeWithNotes(double start, double end, const vector<Note>&)` | 替换时间范围内的音符 |
| `clear()` | 清空 |
| `getNotes() const` | 获取只读引用 |
| `getNotes()` | 获取可变引用 |
| `size()`, `isEmpty()` | 基本查询 |
| `getNoteAtTime(double time)` | 按时间查找 (const / non-const 版本) |
| `findNoteAt(double time, float targetPitch, float tolerance=50)` | 按时间 + 音高查找 |
| `selectAll()`, `deselectAll()` | 全选 / 全取消 |
| `getSelectedNotes()` | 返回选中音符指针列表 |
| `deleteSelectedNotes()` | 删除所有选中音符 |
| `eraseRange(double start, double end)` | 擦除时间范围（自动裁剪跨边界音符） |
| `clearAllDirty()`, `hasDirtyNotes()`, `getDirtyRange()` | 脏标记管理 |

### 4.3 LineAnchor

```cpp
struct LineAnchor {
    int id;
    double time;    // 秒
    float freq;     // Hz
    bool selected;
};
```

---

## 5. NoteGenerator

**文件**: `Source/Utils/NoteGenerator.h`, `Source/Utils/NoteGenerator.cpp`

```cpp
static std::vector<Note> generate(
    const float* f0, int f0Count,
    const float* energy,
    int startFrame, int endFrameExclusive,
    int hopSize, double f0SampleRate, double hostSampleRate,
    const NoteGeneratorParams& params = {});

static std::vector<Note> generate(
    const vector<float>& f0, const vector<float>& energy,
    int hopSize, double f0SampleRate, double hostSampleRate,
    const NoteGeneratorParams& params = {});
```

- 从 F0 曲线自动分割生成有序、非重叠音符序列
- `params.policy` 控制分割行为 (见 data-model.md)

```cpp
static bool validate(const vector<Note>& notes);
```

- 验证音符序列：非零时长、已排序、无重叠

### 5.1 ScaleSnapConfig

```cpp
static const int* semitones(ScaleMode mode, int& outCount) noexcept;
float snapMidi(float midiNote) const noexcept;
```

- `snapMidi`: 将 MIDI 音高吸附到指定调式最近的音级

---

## 6. SimdPerceptualPitchEstimator

**文件**: `Source/Utils/SimdPerceptualPitchEstimator.h`

```cpp
static float estimatePIP(const float* f0, const float* energy,
                         int numSamples, float hopSizeTime);
```

- **PIP (Perceptual Intentional Pitch)**: 感知意图音高估算
- 算法: VNC (150ms 滑动均值) * SSA (Tukey 窗 15% 余弦边缘) * Energy 加权
- 回退: 当权重总和接近 0 时返回 F0 中位数
- 被 `NoteGenerator::representativePitch` 调用

---

## 7. PianoRollCorrectionWorker

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h/.cpp`

### 7.1 任务提交

```cpp
void enqueue(RequestPtr request);
```

- 提交异步修正请求。新请求覆盖旧的 pending 请求（最新优先）
- 被覆盖的请求标记为 `VersionMismatch` 并发布到 completed

```cpp
RequestPtr takeCompleted();
```

- 主线程拉取已完成的请求结果（成功或失败）

### 7.2 版本控制

| 方法 | 说明 |
|---|---|
| `setVersion(uint64_t)` | 设置当前版本 |
| `getVersion()` | 获取当前版本 |
| `incrementVersion()` | 原子递增并返回新版本 |

### 7.3 Clip 上下文

| 方法 | 说明 |
|---|---|
| `setClipContext(int trackId, uint64_t clipId)` | 设置当前处理的 track/clip |
| `getClipContext(int&, uint64_t&)` | 获取当前 track/clip |
| `setClipContextGeneration(uint64_t)` / `getClipContextGeneration()` | clip 上下文代数 |

### 7.4 执行

```cpp
static void executeRequest(AsyncCorrectionRequest& request);
```

- 静态方法，调用 `request.curve->applyCorrectionToRange(...)` 执行实际修正
- Worker loop 在后台线程以 4ms 轮询间隔运行

```cpp
void stop();
```

- 设置 `stopFlag_` 并 join 工作线程

---

## 8. 待确认 (Confirm)

### 边界行为

- [ ] `renderF0Range` 中 `endFrame > originalF0.size()` 时自动截断到 `maxFrame` -- 这是否为预期行为？是否应记录日志？
- [ ] `setOriginalF0Range` 可自动扩展 `originalF0` 尺寸 -- 是否存在并发读取时的安全隐患？（当前 setter 仅应从 message thread 调用）

### 线程模型

- [ ] `PianoRollCorrectionWorker::workerLoop` 使用 4ms sleep 轮询而非条件变量 -- 是否有性能 / 功耗优化计划？
- [ ] `enqueue` 覆盖旧请求时，旧请求被标记为 `VersionMismatch` 并立即发布到 completed -- 主线程是否总是检查 `errorKind`？

### API 完整性

- [ ] `SimdPerceptualPitchEstimator` 在 AGENTS.md 中提到 `getPerceptualOffset` 方法（Fletcher-Munson 校正），但源码中未出现 -- 该方法是否已移至其他文件或已移除？
- [ ] `ScaleSnapConfig::snapMidi` 已实现但未在 `NoteGenerator::generate` 中调用 -- 调式吸附是否在上层 (PianoRollCorrectionWorker / PianoRollComponent) 中调用？

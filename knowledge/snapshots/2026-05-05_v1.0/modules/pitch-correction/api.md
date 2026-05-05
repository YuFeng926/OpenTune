---
spec_version: 1.0.0
status: draft
module: pitch-correction
doc_type: api
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# pitch-correction -- API Contracts

本文档为 `pitch-correction` 模块内所有 public 类型与方法的契约视图（ArchDoc 视角）。所有符号位于 `namespace OpenTune`。

---

## 1. `PitchCurveSnapshot`（`Source/Utils/PitchCurve.h`）

不可变（immutable）音高曲线快照。所有字段均为 `const`。通过 `std::shared_ptr<const PitchCurveSnapshot>` 以 `std::atomic_store` / `atomic_load` 在线程间共享。

### 构造

```cpp
PitchCurveSnapshot(
    std::vector<float> originalF0,
    std::vector<float> originalEnergy,
    std::vector<CorrectedSegment> correctedSegments,
    int hopSize,
    double sampleRate,
    uint64_t renderGeneration = 0);
```

**契约**

- 参数通过 `std::move` 吸收，调用方所有权让渡。
- 不做尺寸一致性校验（`originalF0.size()` 与 `originalEnergy.size()` 不一致时两者独立存储）。
- `renderGeneration == 0` 表示空态/未标记；所有写操作路径应传入新生成的非零值。

### 读取方法（全部 `const`）

| 方法 | 返回 | 语义 |
|---|---|---|
| `getOriginalF0()` | `const std::vector<float>&` | 原始 F0（Hz），未修正；`<=0` 视为 unvoiced |
| `getOriginalEnergy()` | `const std::vector<float>&` | 原始能量（线性或 dB 由上游决定） |
| `getCorrectedSegments()` | `const std::vector<CorrectedSegment>&` | 按 `startFrame` 升序的修正段落 |
| `getHopSize()` | `int` | F0 帧步长（样本数） |
| `getSampleRate()` | `double` | F0 曲线对应的采样率（与 host 不同） |
| `getRenderGeneration()` | `uint64_t` | 单调递增版本号，每次写操作 +1 |
| `isEmpty()` | `bool` | `originalF0_.empty()` |
| `size()` | `size_t` | F0 帧数 |
| `hasAnyCorrection()` | `bool` | 是否存在任何修正段 |
| `hasCorrectionInRange(s, e)` | `bool` | 区间 `[s, e)` 是否与任一段有非空交集 |
| `hasRenderableCorrectedF0()` | `bool` | 与 `hasAnyCorrection()` 等价（语义上为渲染管线使用） |
| `getMemoryUsage()` | `size_t` | 三份 `vector` 的 `capacity()` 合计字节 |

### 渲染接口

```cpp
void renderF0Range(int startFrame, int endFrame,
                   std::function<void(int, const float*, int)> callback) const;
void renderCorrectedOnlyRange(int startFrame, int endFrame,
                              std::function<void(int, const float*, int)> callback) const;
```

**契约**

- 遍历 `[startFrame, endFrame)`，按原始段与修正段交替，以 `callback(frameStart, dataPtr, length)` 推送切片。
- 边界裁剪：`endFrame` 超过 `originalF0_.size()` 时自动截断；`startFrame >= maxFrame` 直接返回。
- **`renderF0Range`**：修正段内产出修正后 F0（含 `LineAnchor` 源额外叠加 `mixRetune`），段外产出原始 F0。
- **`renderCorrectedOnlyRange`**：修正段内产出同上，**段外产出零向量**（用于可视化差值）。
- `LineAnchor` 源 + `retuneSpeed >= 0.0f` 时，对段内每帧调用 `PitchUtils::mixRetune(originalF0, targetF0, retuneSpeed)`；`originalF0 <= 0 || targetF0 <= 0` 时直接输出 `targetF0`。
- callback 的 `dataPtr` 可能指向临时缓冲（LineAnchor 路径），**不允许在回调返回后继续持有**。

---

## 2. `PitchCurve`（COW 外壳）

### 构造 / 克隆

```cpp
PitchCurve();                               // 空快照，hop=512, sr=16000
std::shared_ptr<PitchCurve> clone() const;  // 深拷贝快照内容到新对象
```

### 快照访问

```cpp
std::shared_ptr<const PitchCurveSnapshot> getSnapshot() const;  // atomic_load
```

**契约**：每次调用都是一次独立的原子 load。跨字段一致性时调用方必须一次性捕获快照：

```cpp
auto snap = curve->getSnapshot();
auto& f0 = snap->getOriginalF0();
auto hop = snap->getHopSize();  // 两者属于同一个快照
```

### Forwarding 便捷方法（均委托快照）

`isEmpty()` / `size()` / `getHopSize()` / `getSampleRate()` / `hasCorrectionInRange` / `hasAnyCorrection()` / `hasRenderableCorrectedF0()` / `getMemoryUsage()` / `renderCorrectedOnlyRange()` / `renderF0Range()`。

**警告**：连续调用两个便捷方法会读取两个不同快照，若写入方并发更新，可能观察到不一致状态。

### 写入（COW）

所有写方法**均走 `std::atomic_store(&snapshot_, newSnapshot)`**，不使用 mutex。

| 方法 | 契约 |
|---|---|
| `setOriginalF0(const std::vector<float>&)` | 替换 F0；若 energy 尺寸不等则重置为全 0（尺寸 = f0.size()） |
| `setOriginalEnergy(const std::vector<float>&)` | 替换 energy；不足补 0、超过截断（以 f0.size() 为准） |
| `setOriginalF0Range(size_t startFrame, const std::vector<float>&)` | 在指定位置 splice；不足自动 `resize(endFrame, 0.0f)`；同步 resize energy |
| `setOriginalEnergyRange(size_t startFrame, const std::vector<float>&)` | 类似上；同步 resize f0 |
| `applyCorrectionToRange(notes, start, end, retuneSpeed, vibratoDepth=0, vibratoRate=7.5, audioSR=44100)` | 五阶段修正；见 business.md。**新 segment `source = NoteBased`** |
| `setManualCorrectionRange(s, e, f0Data, source)` | 写入手绘/锚点段；自动附加两侧 smoothstep 过渡段 |
| `setManualCorrectionRange(s, e, f0Data, source, retuneSpeed)` | 同上 + 携带 per-segment retune |
| `clearCorrectionRange(s, e)` | 删除区间内段（两侧段被裁切保留） |
| `clearAllCorrections()` | 清空所有修正段，递增 generation |
| `replaceCorrectedSegments(const std::vector<CorrectedSegment>&)` | 排序后整体替换 |
| `restoreCorrectedSegment(const CorrectedSegment&)` | 删除与之重叠的段后插入，排序 |
| `clear()` | 全重置（F0/energy/segments/hop=512/sr=16000），递增 generation |
| `setHopSize(int)` / `setSampleRate(double)` | 仅替换标量；**不** 递增 generation（当前实现复用旧值）|

**线程安全契约**

- 多读一写：安全（原子 load/store 保证）。
- 多写：**未加锁**，同时调用多个 setter 可能丢失中间写入。当前约定写入者为单一线程（UI 或 Worker 其一），跨线程需调用方协商。
- 读者持有 `shared_ptr<const Snapshot>` 期间，Snapshot 内容不会被 mutate。

---

## 3. `CorrectedSegment`（数据载体）

| 字段 | 类型 | 语义 |
|---|---|---|
| `startFrame` / `endFrame` | `int` | 半开区间 `[start, end)`，基于 F0 帧索引 |
| `f0Data` | `std::vector<float>` | 修正后 F0（Hz），长度 = `endFrame - startFrame` |
| `source` | `Source`（枚举） | `None` / `NoteBased` / `HandDraw` / `LineAnchor` |
| `retuneSpeed` | `float` | `-1` 表示使用 request 默认；`[0, 1]` 表示固定 retune |
| `vibratoDepth` / `vibratoRate` | `float` | 同上，`-1` = fallback |

构造：`CorrectedSegment(int start, int end, const std::vector<float>& data, Source src = Source::None)`。

---

## 4. `Note` / `NoteSequence` / `LineAnchor`（`Source/Utils/Note.h`）

### `struct Note`

| 字段 | 类型 | 默认值 | 语义 |
|---|---|---|---|
| `startTime` / `endTime` | `double` | 0 | 秒，host 时间轴 |
| `pitch` | `float` | 0 | 量化后基准 (Hz) |
| `originalPitch` | `float` | 0 | 未量化原始检测值 (Hz) |
| `pitchOffset` | `float` | 0 | 半音偏移（用户拖动产生） |
| `retuneSpeed` / `vibratoDepth` / `vibratoRate` | `float` | -1 | -1 表示使用请求级别默认值 |
| `velocity` | `float` | 1 | 力度 |
| `isVoiced` | `bool` | true | 是否有声段 |
| `selected` | `bool` | false | UI 选中态 |
| `dirty` | `bool` | false | 增量渲染脏标记 |

**方法**

- `getDuration() const` → `endTime - startTime`
- `getAdjustedPitch() const` → `pitch * 2^(pitchOffset/12)`；pitch≤0 返回 0
- `getMidiNote() const` → round(adjustedPitch 对应 MIDI)
- `getBaseMidiNote() const` → round(pitch 对应 MIDI)
- 静态 `midiToFrequency(int)` / `frequencyToMidi(float)`

### `class NoteSequence`

| 方法 | 语义 |
|---|---|
| `insertNoteSorted(const Note&)` | 插入并调用 `normalizeNonOverlapping`（后者截断前者 endTime） |
| `setNotesSorted(const std::vector<Note>&)` | 全替换并归一化 |
| `replaceRangeWithNotes(s, e, replacements)` | `eraseRange` + 依次插入 |
| `eraseRange(double startTime, double endTime)` | 裁切重叠段；**被分割的两侧 `dirty=true`，`selected=false`** |
| `clear()` | 清空 |
| `getNotes()` const/非 const | 直接访问底层 vector |
| `getNoteAtTime(double)` | 返回 `time ∈ [start, end)` 的首个命中 |
| `findNoteAt(double time, float targetPitch, float pitchTolerance=50Hz)` | 额外按 adjustedPitch 容差过滤 |
| `selectAll` / `deselectAll` / `getSelectedNotes` / `deleteSelectedNotes` | UI 选择态操作 |
| `clearAllDirty()` / `hasDirtyNotes()` / `getDirtyRange()` | 增量渲染辅助；`getDirtyRange` 无 dirty 返回 `{-1, -1}` |

### `struct LineAnchor`

`{ int id; double time; float freq; bool selected; }` — 手绘 F0 的锚点。

---

## 5. `NoteGenerator`（`Source/Utils/NoteGenerator.h`）

静态工具类，构造已 `= delete`。

### 配置类型

```cpp
enum class ScaleMode {
    Chromatic, Major, Minor, HarmonicMinor,
    Dorian, Mixolydian, PentatonicMajor, PentatonicMinor
};

struct ScaleSnapConfig {
    RootNote  root = 0;                // MIDI 半音数（0-11 取模有效）
    ScaleMode mode = ScaleMode::Chromatic;

    static const int* semitones(ScaleMode mode, int& outCount) noexcept;
    float snapMidi(float midiNote) const noexcept;
};

struct NoteSegmentationPolicy {
    float transitionThresholdCents = 80.0f;  // 相对平均音高偏离 80 cents 切段
    float gapBridgeMs              = 10.0f;  // 连续 unvoiced ≤ 10ms 桥接同一 note
    float minDurationMs            = 100.0f; // 小于 100ms 的 note 丢弃
    float tailExtendMs             = 15.0f;  // 尾部向右延伸 15ms
};

struct NoteGeneratorParams {
    NoteSegmentationPolicy policy;
    float retuneSpeed  = -1.0f;
    float vibratoDepth = -1.0f;
    float vibratoRate  = -1.0f;
    std::optional<ScaleSnapConfig> scaleSnap;
};
```

### 核心 API

```cpp
static std::vector<Note> generate(
    const float* f0, int f0Count,
    const float* energy,                  // 可空，空则不启用 PIP
    int startFrame, int endFrameExclusive,
    int hopSize, double f0SampleRate, double hostSampleRate,
    const NoteGeneratorParams& params = {});

static std::vector<Note> generate(
    const std::vector<float>& f0,
    const std::vector<float>& energy,     // 尺寸必须等于 f0 才启用 PIP，否则视作无
    int hopSize, double f0SampleRate, double hostSampleRate,
    const NoteGeneratorParams& params = {});

static bool validate(const std::vector<Note>& notes);  // 检查持续时间/排序/重叠
```

**契约**

- 输入校验失败（`f0 == nullptr || f0Count ≤ 0 || hopSize ≤ 0 || f0SampleRate ≤ 0`）→ 返回空向量。
- `startFrame` 负值取 0；`endFrameExclusive` 超过 `f0Count` 截断到 `f0Count`。
- 使用 `ScaleSnapConfig::snapMidi` 对代表音高 MIDI 值取近似后 `round` 得到 `pitch`；`originalPitch` 保留未量化的代表音高（PIP 或中位数）。
- `hostSampleRate` **当前未在算法中使用**，作为保留参数。
- 代表音高计算：若有 `energy` 则走 `SimdPerceptualPitchEstimator::estimatePIP`，否则取 voiced 帧中位数。
- 输出序列已排序且非重叠（后者 startTime 截断前者 endTime）。
- `validate` 打印 DBG 日志但不抛异常，仅返回 `bool`。

---

## 6. `SimdPerceptualPitchEstimator`（`Source/Utils/SimdPerceptualPitchEstimator.h`）

### 核心 API

```cpp
static float estimatePIP(const float* f0,
                         const float* energy,
                         int numSamples,
                         float hopSizeTime);   // 单位：秒/帧
```

**契约**

- `numSamples <= 0 || f0/energy == nullptr || hopSizeTime <= 0` → 返回 `0.0f`（不抛异常，用于实时上下文）。
- VNC 移动平均窗长度 = `max(1, floor(0.150 / hopSizeTime))`（覆盖 ≥150ms 的颤音周期）。
- SSA 使用 Tukey 边缘 cosine 斜率：`edgePercent = 0.15`，两端各 ramp 长度衰减。
- 权重 = SSA × Energy，分子 = Σ VNC×Weight，分母 = Σ Weight。
- `totalWeight < 1e-7f` → 回退为原始 F0 的中位数（静音 fallback）。
- 权重被强制非负（负值截断为 0）。
- **内部 O(N·Win) 实现**，注释说明"N 小（50-500 帧），可接受"。

**内部 helpers（私有）**

- `computeMovingAverage(src, dest, n, win)` — 中心化移动平均（边界 clamp 索引）。
- `prepareWeightBuffer(weights, energy, n, ramp)` — Tukey 边缘 + 非负化。

---

## 7. `PitchUtils`（`Source/Utils/PitchUtils.h`）

全部 inline 纯函数，`namespace OpenTune::PitchUtils`：

| 函数 | 契约 |
|---|---|
| `mixRetune(shiftedF0, targetF0, retuneSpeed)` | `shifted<=0` → targetF0；`target<=0` → shiftedF0；否则在 log2 空间按 `(1-speed)` 缩放偏差后回指数。`retuneSpeed` clamp 到 `[0,1]` |
| `freqToMidi(freq)` | `freq<=0` → 0；否则 `69 + 12*log2(freq/440)` |
| `midiToFreq(midi)` | `midi<=0` → 0；否则 `440 * 2^((midi-69)/12)` |

---

## 8. `PianoRollCorrectionWorker`（`Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h`）

### 数据结构

```cpp
struct AsyncCorrectionRequest {
    enum class Kind { ApplyNoteRange, AutoTuneGenerate };
    enum class ErrorKind { None, InvalidRange, VersionMismatch, ExecutionError };

    Kind kind = Kind::ApplyNoteRange;
    std::shared_ptr<PitchCurve> curve;     // ApplyNoteRange 必填
    std::vector<Note> notes;               // ApplyNoteRange 输入；AutoTuneGenerate 输出
    int startFrame = 0, endFrameExclusive = 0;
    float retuneSpeed = 1.0f;
    float vibratoDepth = 0.0f, vibratoRate = 5.0f;
    double audioSampleRate = 44100.0;
    uint64_t version = 0;                  // 由 worker 在 enqueue 时递增填入

    // AutoTuneGenerate 专用
    int    autoHopSize        = 160;
    double autoF0SampleRate   = 16000.0;
    int    autoStartFrame = 0, autoEndFrame = 0;   // inclusive endFrame
    NoteGeneratorParams autoGenParams;
    std::vector<float>  autoOriginalF0Full;

    uint64_t materializationEpochSnapshot = 0;
    uint64_t materializationIdSnapshot    = 0;

    bool success = false;
    std::string errorMessage;
    ErrorKind errorKind = ErrorKind::None;
};

using RequestPtr = std::shared_ptr<AsyncCorrectionRequest>;
```

### 方法

| 方法 | 契约 |
|---|---|
| 构造函数 | 立即启动 `workerThread_`，进入 `workerLoop` |
| 析构函数 | 调用 `stop()` |
| `void enqueue(RequestPtr request)` | 拒绝 null / `ApplyNoteRange` 无 curve 的请求；**递增 version 并写入唯一 pending 槽**；旧 pending 立即标记 `VersionMismatch` 转入 completed |
| `RequestPtr takeCompleted()` | 非阻塞，原子取走最近完成的请求（可能是成功或失败）；无完成则返回 null |
| `void stop()` | 置位 `stopFlag_` + notify_one + join；可在析构外主动调用 |
| `static void executeRequest(AsyncCorrectionRequest&)` | 同步分发：`ApplyNoteRange → PitchCurve::applyCorrectionToRange`；`AutoTuneGenerate → NoteGenerator::generate`（注意 `autoEndFrame + 1` 作为 exclusive 上界） |

**执行前校验**（workerLoop）

1. Kind-aware 范围检查：`AutoTuneGenerate` 检查 `autoEndFrame > autoStartFrame`；`ApplyNoteRange` 检查 `endFrameExclusive > startFrame`。失败 → `ErrorKind::InvalidRange`。
2. 版本检查：`request.version != getVersion()` → `ErrorKind::VersionMismatch`。
3. 执行体包裹 `try { executeRequest } catch (std::exception) catch (...)` → `ErrorKind::ExecutionError`。

### 线程模型

- 单 worker 线程，`std::condition_variable` 唤醒（非轮询）。
- pending 单槽：后到新请求覆盖旧请求，旧请求立刻报错回传（**最新优先策略**）。
- 完成结果存于 `completedRequest_` 单槽，由 UI 线程 `takeCompleted()` 拉取；**若 UI 不及时拉取，后续 completed 会覆盖**。

### 约束

- 拒绝规则（当前实现）：`enqueue` 仅拒 null + `ApplyNoteRange` 无 curve；其余入队后在 workerLoop 再校验。
- `incrementVersion()` 使用 `memory_order_acq_rel`，版本号跨线程可见。
- 析构时若 workerThread 正在执行 `executeRequest`，会等待其完成（join）。

---

## ⚠️ 待确认

1. `PitchCurve::setHopSize` / `setSampleRate` **未递增 `renderGeneration`**，下游若以 generation 判断缓存是否失效，可能忽略这两类写入。是否需要补齐？
2. `ScaleSnapConfig::snapMidi` 当前在代码中仅被 `NoteGenerator::quantisePitch` 内部调用；UI 层 / 用户交互路径是否存在二次调用，是否需要导出为业务 API？
3. `PianoRollCorrectionWorker::takeCompleted` 在多次写入覆盖下丢弃中间态 —— 这是刻意"最新胜出"还是需要队列化？
4. `AsyncCorrectionRequest::materializationEpochSnapshot` / `materializationIdSnapshot` 的上游填入者与消费者在本模块未出现；是否由 `core-processor` 材料化路径负责？
5. 旧文档提到的 `getPerceptualOffset` 方法未在当前 `SimdPerceptualPitchEstimator` 中出现，是否已被 `estimatePIP` 完全替代？
6. `PitchCurveSnapshot::renderF0Range` 在 `endFrame > maxFrame` 时仅截断而不回调"越界段全零"，下游消费方（声码器）是否需要区别对待？
7. `NoteGenerator::generate(std::vector<float>& f0, std::vector<float>& energy, ...)` 当 `energy.size() != f0.size()` 时会静默放弃 PIP 回退中位数；是否需要显式日志？

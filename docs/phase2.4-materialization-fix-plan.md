# Phase 2.4 Placement MaterializationId 修复完整方案

## 背景
StandaloneArrangement::Placement 已改用 ContentKey，但 31 处代码仍访问不存在的 placement.materializationId

## 修复清单

### PluginProcessor.cpp

#### 已修复 (3处)
- Line 142: hasRemainingPlacementForContent - 改用 ContentKey 参数 ✓
- Line 573: renderPlacementForExport - 检查 contentKey.isValid() ✓
- Line 580: renderPlacementForExport - 改用 getStandalonePlaybackReadSource ✓
- Line 2402: processBlock - 改用 getStandalonePlaybackReadSource ✓
- Line 2440-2454: processBlock - 简化 readRequest.source 赋值 ✓
- Line 2788: 序列化 - 改用 placement.contentKey.objectId ✓
- Line 2899-2901: 序列化写 - 写入 ContentKey 三个字段 ✓

#### 待修复 (11处)
- Line 3130: 反序列化读 - 读取 ContentKey 三个字段
- Line 3143: 反序列化检查 - 检查 contentKey.isValid()
- Line 3551: deletePlacementById - 通过 contentKey 查询
- Line 3556: deletePlacementById - retireClip(contentKey)
- Line 3563: deletePlacementById - outcome 返回 contentKey
- Line 3853: exportToFile - 检查音频可用性
- Line 3869: exportToFile - PlaybackPlacement 构造
- Line 3908: exportMix - 检查音频可用性
- Line 3929: exportMix - 检查音频可用性
- Line 3935: exportMix - PlaybackPlacement 构造
- Line 3962: exportSeparateTracks - 检查音频可用性
- Line 3987: exportSeparateTracks - 检查音频可用性

### PluginEditor.cpp (3处)
- Line 60: placement 构造
- Line 682: 比较 activeMaterializationId
- Line 704: 比较 activeMaterializationId

### PianoRollComponent.cpp (10处)
- Line 1763-3095: UI 层访问

## 关键替换模式

### 模式 1: 检查有效性
\\\cpp
// 旧代码
if (placement.materializationId == 0)

// 新代码
if (!placement.contentKey.isValid())
\\\

### 模式 2: 序列化
\\\cpp
// 旧代码 (写)
output.writeInt64(placement.materializationId);

// 新代码 (写)
output.writeInt(static_cast<int>(placement.contentKey.domainKind));
output.writeInt64(placement.contentKey.objectId);
output.writeInt64(placement.contentKey.sourceWindowDiscriminator);

// 旧代码 (读)
placement.materializationId = input.readInt64();

// 新代码 (读)
placement.contentKey.domainKind = static_cast<DomainKind>(input.readInt());
placement.contentKey.objectId = input.readInt64();
placement.contentKey.sourceWindowDiscriminator = input.readInt64();
\\\

### 模式 3: Store 查询
\\\cpp
// 旧代码
materializationStore_->getPlaybackReadSource(placement.materializationId, source)
materializationStore_->containsMaterialization(placement.materializationId)
materializationStore_->retireMaterialization(placement.materializationId)

// 新代码
getStandalonePlaybackReadSource(placement.contentKey, source)
standaloneContentRepository_->findClip(placement.contentKey) != nullptr
standaloneContentRepository_->retireClip(placement.contentKey)
\\\

### 模式 4: PlaybackPlacement 构造
\\\cpp
// 旧代码
StandaloneArrangement::PlaybackPlacement{
    placement.materializationId,
    placement.timelineStartSeconds,
    ...
}

// 新代码
StandaloneArrangement::PlaybackPlacement{
    placement.contentKey,
    placement.timelineStartSeconds,
    ...
}
\\\

### 模式 5: Outcome 返回值
\\\cpp
// 旧代码
outcome.materializationId = placement.materializationId;

// 新代码
outcome.contentKey = placement.contentKey;
\\\

## 执行策略
1. 按文件分组修复
2. 每个文件修复后检查编译错误
3. 最后整体编译验证


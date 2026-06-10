# Phase 2.4 placement.materializationId 修复方案

## 问题
StandaloneArrangement::Placement 和 PlaybackPlacement 已使用 ContentKey，但 31 处代码仍访问不存在的 placement.materializationId

## 修复策略
所有 placement.materializationId → placement.contentKey
所有 materializationId == 0 → !contentKey.isValid()
所有通过 materializationId 查询 store → 通过 contentKey 查询 StandaloneContentRepository

## 需要修复的文件和位置

### PluginProcessor.cpp (18处)
- Line 142: hasRemainingPlacementForContent 参数改 ContentKey ✓
- Line 573: renderPlacementForExport 检查改 contentKey.isValid() ✓
- Line 580: 改用 getStandalonePlaybackReadSource(contentKey) ✓
- Line 2413: processBlock 渲染路径
- Line 2815: appendUniqueMaterializationId → appendUniqueContentKey
- Line 2924: 序列化写 contentKey
- Line 3153: 反序列化读 contentKey
- Line 3166: 检查 contentKey 有效性
- Line 3551: deletePlacementById sourceId 查询
- Line 3556: retirePlacement 调用
- Line 3563: outcome 返回值
- Line 3853-3987: 导出函数多处 (7处)

### PluginEditor.cpp (3处)
- Line 60: placement 构造
- Line 682: 比较 activeMaterializationId
- Line 704: 比较 activeMaterializationId

### PianoRollComponent.cpp (10处)
- Line 1763-3095: UI 层访问

## 关键 API 变更
- processor.getMaterializationStore()->getPlaybackReadSource(id) 
  → processor.getStandalonePlaybackReadSource(contentKey)
- materializationStore_->retireMaterialization(id)
  → standaloneContentRepository_->retireClip(contentKey)
- materializationStore_->containsMaterialization(id)
  → standaloneContentRepository_->findClip(contentKey) != nullptr


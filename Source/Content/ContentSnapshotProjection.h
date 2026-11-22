#pragma once
#include "ContentState.h"
#include "EditableContentSnapshot.h"

namespace OpenTune {

/// ContentState → EditableContentSnapshot 纯数据投影，只复制两者的共同字段。
/// EditableContentSnapshot 与 ContentState 字段完全对齐，无需域侧补写。
EditableContentSnapshot makeContentSnapshot(const ContentState& state);

/// EditableContentSnapshot → ContentState 纯反向投影，只复制两者的共同字段。
/// 产物是新 ContentState：运行时 contentRevision 从默认 1 开始，不继承 snapshot 值。
ContentState contentStateFromSnapshot(const EditableContentSnapshot& snapshot);

} // namespace OpenTune

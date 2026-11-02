#pragma once

#include <juce_core/juce_core.h>

namespace OpenTune::DemoProject {

/**
 * 首次运行的演示工程。
 *
 * 音频随应用打包在 Demo/Demo.wav（不复制）；工程文件在首次运行时生成到
 * Documents/Projects/Demo.otproj，其 relativeMediaPath 写成从工程目录指向应用包内
 * 音频的相对路径。应用更新后包路径可能变化，因此每次启动都修复失效的引用；
 * 用户保存过的工程（媒体已复制进 .Demo_Media）保持不动。
 */

/** 生成（或修复）演示工程并返回其路径；资源缺失时返回空 File。 */
juce::File install();

/** 若演示工程已生成，修复其中失效的包内音频引用。 */
void repairIfInstalled();

} // namespace OpenTune::DemoProject

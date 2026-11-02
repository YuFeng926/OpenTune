#pragma once

namespace OpenTune {

/**
 * iPad 首次运行演示导览在“正在打开工程”这一步的判定。
 *
 * 打开是异步的。LoadingProject 刚被设上时，轨道上还没有片段，而且
 * projectOperationBusy_ 也还没被置上。这个窗口不是打开失败：失败只发生在
 * 打开请求已经发出并且已经结束、工程里仍然没有音频的时候。
 * 把窗口误判成失败会取消导览、不写首次运行标记，但工程仍然会被打开，
 * 于是每次启动都重新打开 demo，第二段引导永远不出现。
 */
enum class DemoTourLoadPhase
{
    WaitingForOpen,
    OpenFailed,
    TourReady
};

struct DemoTourLoadInput
{
    bool openRequested = false;
    bool openInProgress = false;
    bool hasPlacement = false;
};

inline DemoTourLoadPhase resolveDemoTourLoad(DemoTourLoadInput input) noexcept
{
    if (!input.openRequested || input.openInProgress)
        return DemoTourLoadPhase::WaitingForOpen;
    if (!input.hasPlacement)
        return DemoTourLoadPhase::OpenFailed;
    return DemoTourLoadPhase::TourReady;
}

} // namespace OpenTune

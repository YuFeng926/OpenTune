#include "../PianoRollComponent.h"
#include "../UIColors.h"
#include "../TimelineViewportPolicy.h"

namespace OpenTune {

PianoRollOverlayComponent::PianoRollOverlayComponent(PianoRollComponent& owner)
    : owner_(owner)
{
}

void PianoRollOverlayComponent::paint(juce::Graphics& g)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    // 1. 播放头
    owner_.drawPlayheadOverlay(g);

    // 1b. 播放头经过音符高亮
    owner_.drawPlayheadNoteHighlight(g);

    // 2. 交互 Overlay（选中高亮、框选、绘制预览、ghost 把手等）
    owner_.drawTransientOverlay(g);

    // 3. TimeGrid 把手
    owner_.drawTimeGridHandles(g);

    // 4. 选中音符高亮
    owner_.drawSelectedNoteHighlights(g);

    // 5. F0 曲线选中高亮
    owner_.drawF0SelectionHighlight(g);

    // 6. Ghost content (已在内容表面绘制，此处跳过)

    // 7. 按下琴键高亮
    owner_.drawPianoKeysPressed(g);

    owner_.recordRenderProbe(PianoRollComponent::RenderProbePoint::OverlayPresent,
                             juce::Time::getMillisecondCounterHiRes() - t0);
}

} // namespace OpenTune

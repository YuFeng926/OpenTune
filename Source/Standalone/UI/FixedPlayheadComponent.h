#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

/**
 * FixedPlayheadComponent - 固定屏幕空间播放头
 * 
 * 播放头固定在 screen-space，不接收 timeline time。
 * Transport tick 只更新 camera，播放头不因 tick repaint。
 * 只响应 layout/style/visibility 改变。
 */
class FixedPlayheadComponent : public juce::Component {
public:
    FixedPlayheadComponent();
    ~FixedPlayheadComponent() override;
    
    void setAnchorBounds(int anchorX, int height);
    void setColour(juce::Colour colour);
    void setVisible(bool visible);
    
    void paint(juce::Graphics& g) override;
    
private:
    int anchorX_ = 0;
    int height_ = 0;
    juce::Colour colour_ = juce::Colours::red;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FixedPlayheadComponent)
};

} // namespace OpenTune

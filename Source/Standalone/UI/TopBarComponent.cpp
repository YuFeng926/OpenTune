#include "TopBarComponent.h"
#include "ToolbarIcons.h"

namespace OpenTune {

TopBarComponent::TopBarComponent(MenuBarComponent& menuBar, TransportBarComponent& transportBar)
    : menuBar_(menuBar)
    , transportBar_(transportBar)
{
    transportBar_.setEmbeddedInTopBar(true);
    addAndMakeVisible(menuBar_);
    addAndMakeVisible(transportBar_);

    // 侧边栏开关按钮 - 使用箭头图标表示收起/展开功能
    trackPanelToggleButton_.setIcon(ToolbarIcons::getPanelRightIcon());
    trackPanelToggleButton_.setClickingTogglesState(true);
    trackPanelToggleButton_.setToggleState(true, juce::dontSendNotification);
    trackPanelToggleButton_.setTooltip(LOC(kTooltipTrackPanel));
    trackPanelToggleButton_.onClick = [this]() {
        if (onToggleTrackPanel) onToggleTrackPanel();
    };
    addAndMakeVisible(trackPanelToggleButton_);

    parameterPanelToggleButton_.setIcon(ToolbarIcons::getPanelLeftIcon());
    parameterPanelToggleButton_.setClickingTogglesState(true);
    parameterPanelToggleButton_.setToggleState(true, juce::dontSendNotification);
    parameterPanelToggleButton_.setTooltip(LOC(kTooltipParameterPanel));
    parameterPanelToggleButton_.onClick = [this]() {
        if (onToggleParameterPanel) onToggleParameterPanel();
    };
    addAndMakeVisible(parameterPanelToggleButton_);
}

void TopBarComponent::applyTheme()
{
    transportBar_.applyTheme();

    // 按钮颜色由 UnifiedToolbarButton 内部处理，这里触发重绘即可
    repaint();
}

void TopBarComponent::setSidePanelsVisible(bool trackPanelVisible, bool parameterPanelVisible)
{
    trackPanelToggleButton_.setToggleState(trackPanelVisible, juce::dontSendNotification);
    parameterPanelToggleButton_.setToggleState(parameterPanelVisible, juce::dontSendNotification);
    repaint();
}

void TopBarComponent::setTrackPanelToggleVisible(bool visible)
{
    trackPanelToggleVisible_ = visible;
    trackPanelToggleButton_.setVisible(visible);
    resized();
    repaint();
}

void TopBarComponent::refreshLocalizedText()
{
    // 更新按钮文本和 tooltip
    trackPanelToggleButton_.setButtonText(LOC(kTracks));
    trackPanelToggleButton_.setTooltip(LOC(kTooltipTrackPanel));
    
    parameterPanelToggleButton_.setButtonText(LOC(kProps));
    parameterPanelToggleButton_.setTooltip(LOC(kTooltipParameterPanel));
    
    // 刷新运输栏
    transportBar_.refreshLocalizedText();
    
    repaint();
}

void TopBarComponent::paint(juce::Graphics& g)
{
    const auto& style = UIColors::currentThemeStyle();
    // 阴影边距：背景在 reduced(12) 区域内绘制，阴影在边距内渲染
    const float shadowMargin = 12.0f;
    auto bounds = getLocalBounds().toFloat().reduced(shadowMargin);

    // 顶部条属于"悬浮层级"，使用更明显但仍柔和的 L2 阴影
    if (UIColors::currentThemeId() == ThemeId::Aurora)
    {
        // Aurora Theme: No rounded corners, soft bottom edge
        UIColors::drawShadow(g, bounds, UIColors::ShadowLevel::Float);
        
        // Background - Dark gradient
        juce::ColourGradient bgGrad(UIColors::backgroundDark, 0.0f, 0.0f,
                                    UIColors::backgroundMedium, 0.0f, bounds.getHeight(), false);
        g.setGradientFill(bgGrad);
        g.fillRect(bounds);

        // Bottom Edge - Gradient Blur (Dilute boundary)
        juce::ColourGradient bottomBlur(juce::Colours::transparentBlack, 0.0f, bounds.getBottom() - 4.0f,
                                        juce::Colour(Aurora::Colors::BorderGlow).withAlpha(0.2f), 0.0f, bounds.getBottom(), false);
        g.setGradientFill(bottomBlur);
        g.fillRect(bounds.getX(), bounds.getBottom() - 4.0f, bounds.getWidth(), 4.0f);
    }
    else
    {
        UIColors::drawShadow(g, bounds, UIColors::ShadowLevel::Float);
        UIColors::fillPanelBackground(g, bounds, style.panelRadius);
        UIColors::drawPanelFrame(g, bounds, style.panelRadius);
    }
}

void TopBarComponent::resized()
{
    // 阴影边距：内容区域在 reduced(12) 范围内布局
    const int shadowMargin = 12;
    auto bounds = getLocalBounds().reduced(shadowMargin);

    // 顶部菜单条

    if (menuBar_.isVisible())
        menuBar_.setBounds(bounds.removeFromTop(25));
    else
        menuBar_.setBounds({});

    // Transport 行：左/右留给侧边栏开关按钮
    const int pad = 6;
    const int toggleW = 50; // 统一宽度 (50px) - Scaled 1.25x
    const int toggleH = 40; // 统一高度 (40px) - Scaled 1.25x

    auto row = bounds.reduced(pad, pad);

    if (trackPanelToggleVisible_) {
        auto leftArea = row.removeFromLeft(toggleW);
        trackPanelToggleButton_.setBounds(leftArea.withSizeKeepingCentre(toggleW, toggleH));
        row.removeFromLeft(pad);
    } else {
        trackPanelToggleButton_.setBounds({});
    }

    auto rightArea = row.removeFromRight(toggleW);
    parameterPanelToggleButton_.setBounds(rightArea.withSizeKeepingCentre(toggleW, toggleH));

    row.removeFromRight(pad);
    transportBar_.setBounds(row);
}

} // namespace OpenTune

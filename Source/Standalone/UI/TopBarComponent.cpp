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
    const float shadowMargin = UIColors::currentThemeId() == ThemeId::Aurora ? 10.0f : 12.0f;
    auto bounds = getLocalBounds().toFloat().reduced(shadowMargin);

    // 顶部条属于"悬浮层级"，使用更明显但仍柔和的 L2 阴影
    if (UIColors::currentThemeId() == ThemeId::Overdose)
    {
        UIColors::drawShadow(g, bounds, UIColors::ShadowLevel::Float);

        juce::Path tray;
        tray.addRoundedRectangle(bounds, style.panelRadius);

        // 扁平面板：纯色填充 + 柔和边框 + 顶线高光
        g.setColour(juce::Colour(Overdose::Colors::PanelOpaqueTop).withAlpha(0.96f));
        g.fillPath(tray);

        g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.48f));
        g.strokePath(tray, juce::PathStrokeType(1.0f));

        g.setColour(juce::Colour(Overdose::Colors::PanelHighlight).withAlpha(0.38f));
        g.drawLine(bounds.getX() + style.panelRadius,
                   bounds.getY() + 1.0f,
                   bounds.getRight() - style.panelRadius,
                   bounds.getY() + 1.0f,
                   1.0f);
    }
    else if (UIColors::currentThemeId() == ThemeId::Aurora)
    {
        UIColors::drawShadow(g, bounds, UIColors::ShadowLevel::Float);

        UIColors::fillAuroraGlass(g, bounds, 7.0f);
        UIColors::drawAuroraGlassFrame(g, bounds, 7.0f, false);

        juce::ColourGradient bottomClosure(juce::Colours::transparentBlack,
                                           bounds.getX(),
                                           bounds.getBottom() - 3.0f,
                                           UIColors::backgroundDark.withAlpha(0.20f),
                                           bounds.getX(),
                                           bounds.getBottom(),
                                           false);
        g.setGradientFill(bottomClosure);
        g.fillRect(bounds.getX() + 6.0f, bounds.getBottom() - 3.0f, bounds.getWidth() - 12.0f, 3.0f);
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

#include "TopBarComponent.h"
#include "ToolbarIcons.h"

namespace OpenTune {

namespace {

constexpr float kAuroraTopBarSurfaceIntensity = 0.62f;
constexpr float kAuroraTopBarFrameIntensity = 0.12f;

} // namespace

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
    // 阴影边距：Overdose 托盘贴上沿（参考图），仅底部保留阴影空间
    auto fullBounds = getLocalBounds().toFloat();
    juce::Rectangle<float> bounds;
    if (UIColors::currentThemeId() == ThemeId::Overdose)
    {
        bounds = fullBounds.reduced(8.0f, 0.0f);
        bounds = bounds.withBottom(fullBounds.getBottom() - 10.0f);
    }
    else if (UIColors::currentThemeId() == ThemeId::Aurora)
    {
        bounds = fullBounds.reduced(10.0f);
    }
    else
    {
        bounds = fullBounds.reduced(12.0f);
    }

    // 顶部条属于"悬浮层级"，使用更明显但仍柔和的 L2 阴影
    if (UIColors::currentThemeId() == ThemeId::Overdose)
    {
        UIColors::drawShadow(g, bounds, UIColors::ShadowLevel::Float);

        juce::Path tray;
        tray.addRoundedRectangle(bounds, style.panelRadius);

        // 玻璃拟态托盘：横向渐变（参考图：左 #BDB6DA 右 #C6C0E2，顶部亮、底部阴影深）
        juce::ColourGradient panelGrad(
            juce::Colour(0xFFC7C1E1).withAlpha(0.98f),
            bounds.getX(), bounds.getCentreY(),
            juce::Colour(0xFFCFCAE9).withAlpha(0.98f),
            bounds.getRight(), bounds.getCentreY(), false);
        panelGrad.addColour(0.5f, juce::Colour(0xFFC2BBDD).withAlpha(0.96f));
        g.setGradientFill(panelGrad);
        g.fillPath(tray);

        g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.48f));
        g.strokePath(tray, juce::PathStrokeType(1.0f));

        // 顶部高光（参考图：约 6px 白色亮上沿 #EFEBF3）
        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(tray);
            auto topBand = bounds.withHeight(6.0f);
            juce::ColourGradient topGlow(juce::Colour(0xFFFFFFFF).withAlpha(0.70f),
                                         topBand.getCentreX(), topBand.getY(),
                                         juce::Colour(0xFFEFEBF3).withAlpha(0.10f),
                                         topBand.getCentreX(), topBand.getBottom(), false);
            g.setGradientFill(topGlow);
            g.fillRect(topBand);
        }

        // 底部紫灰压暗（参考图：底部阴影 #746AA2 渐隐，贴边更明显）
        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(tray);
            auto bottomBand = bounds.withTrimmedTop(bounds.getHeight() * 0.50f);
            juce::ColourGradient bs(juce::Colours::transparentBlack,
                                    bottomBand.getCentreX(), bottomBand.getY(),
                                    juce::Colour(0xFF746AA2).withAlpha(0.42f),
                                    bottomBand.getCentreX(), bottomBand.getBottom(), false);
            g.setGradientFill(bs);
            g.fillRect(bottomBand);
        }
    }
    else if (UIColors::currentThemeId() == ThemeId::Aurora)
    {
        UIColors::drawShadow(g, bounds, UIColors::ShadowLevel::Ambient);

        UIColors::fillAuroraGlass(g, bounds, 7.0f, kAuroraTopBarSurfaceIntensity);
        UIColors::drawAuroraGlassFrame(g, bounds, 7.0f, false, kAuroraTopBarFrameIntensity);

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
    // 阴影边距：与 paint 保持一致（Overdose 顶部贴边、底部 10px 阴影空间）
    auto bounds = getLocalBounds();
    if (UIColors::currentThemeId() == ThemeId::Overdose)
    {
        bounds = bounds.reduced(8, 0);
        bounds.setBottom(getLocalBounds().getBottom() - 10);
    }
    else
    {
        bounds = bounds.reduced(12);
    }

    // 顶部菜单条

    if (menuBar_.isVisible())
        menuBar_.setBounds(bounds.removeFromTop(25));
    else
        menuBar_.setBounds({});

    // Transport 行：左/右留给侧边栏开关按钮
    const int pad = 3;
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

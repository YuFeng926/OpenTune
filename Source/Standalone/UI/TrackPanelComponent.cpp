#include "TrackPanelComponent.h"
#include "UIColors.h"

namespace OpenTune {

// MuteSoloIconButton implementation
void MuteSoloIconButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    bool isToggled = getToggleState();
    const auto themeId = UIColors::currentThemeId();
    
    // Custom colors requested by user: Mute = Deep Red, Solo = Orange
    juce::Colour activeBase;
    if (iconType_ == IconType::Mute)
        activeBase = juce::Colour(0xFF8B0000); // Deep Red
    else
        activeBase = juce::Colour(0xFFE67E22); // Orange (Standard Studio Color)

    if (themeId == ThemeId::Overdose)
    {
        // Overdose 主题：使用粉色系
        activeBase = juce::Colour(Overdose::Colors::PrimaryPink);
        
        UIColors::fillOverdoseButtonShell(g, bounds, UIColors::currentThemeStyle().controlRadius);

        if (isToggled || shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
        {
            const auto outline = (isToggled || shouldDrawButtonAsDown)
                ? activeBase.brighter(0.18f).withAlpha(0.92f)
                : juce::Colour(Overdose::Colors::PanelHighlight).withAlpha(0.42f);
            g.setColour(outline);
            g.drawRoundedRectangle(bounds.reduced(0.75f),
                                   UIColors::currentThemeStyle().controlRadius,
                                   (isToggled || shouldDrawButtonAsDown) ? 1.45f : 1.0f);
        }
    }
    else if (themeId == ThemeId::Aurora)
    {
        UIColors::drawAuroraButtonChrome(g,
                                         bounds,
                                         UIColors::currentThemeStyle().controlRadius,
                                         shouldDrawButtonAsHighlighted,
                                         shouldDrawButtonAsDown,
                                         isToggled,
                                         isToggled ? activeBase : UIColors::panelGlow,
                                         isToggled ? activeBase.brighter(0.25f) : UIColors::glassEdge);
    }
    else
    {
        auto base = isToggled ? activeBase : UIColors::buttonNormal;
        getLookAndFeel().drawButtonBackground(g, *this, base, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }

    // Icon text
    if (themeId == ThemeId::Overdose)
    {
        g.setColour(isToggled
                        ? juce::Colour(0xFFFFFFFF)  // 激活时白色
                        : juce::Colour(Overdose::Colors::TextPrimary).withAlpha(0.85f));  // 普通时深粉紫
    }
    else
    {
        g.setColour(isToggled ? activeBase.brighter(0.42f) : UIColors::textPrimary);
    }
    g.setFont(UIColors::getUIFont(14.0f));

    juce::String iconText = (iconType_ == IconType::Mute) ? "M" : "S";
    g.drawText(iconText, getLocalBounds().toFloat(), juce::Justification::centred);
}

TrackPanelComponent::TrackPanelComponent()
{
    // 初始化所有12条轨道 - 紧凑模式：只保留S/M按钮和音量旋钮（带环形LED电平表）
    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        auto& track = tracks_[i];

        // Mute button
        track.muteButton.setClickingTogglesState(true);
        track.muteButton.onClick = [this, i] { onMuteToggled(i); };
        addAndMakeVisible(track.muteButton);

        // Solo button
        track.soloButton.setClickingTogglesState(true);
        track.soloButton.onClick = [this, i] { onSoloToggled(i); };
        addAndMakeVisible(track.soloButton);

        // Volume slider (Knob)
        track.volumeSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        track.volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        track.volumeSlider.setLookAndFeel(&knobLnF_); // Apply custom LookAndFeel
        track.volumeSlider.onValueChange = [this, i] { onVolumeChanged(i); };
        addAndMakeVisible(track.volumeSlider);

        // 环形LED电平表 - 先添加使其在旋钮后面（z-order）
        addAndMakeVisible(track.levelMeter);
        track.levelMeter.toBack();  // 确保在旋钮后面
        
        // 默认只显示前 visibleTrackCount_ 条轨道的控件
        bool visible = (i < visibleTrackCount_);
        track.muteButton.setVisible(visible);
        track.soloButton.setVisible(visible);
        track.volumeSlider.setVisible(visible);
        track.levelMeter.setVisible(visible);
    }

    // 初始化+号按钮
    addTrackButton_.onClick = [this]() { showMoreTracks(); };
    addAndMakeVisible(addTrackButton_);

    // Initialize track colors with default palette
    {
        auto& random = juce::Random::getSystemRandom();
        for (int i = 0; i < MAX_TRACKS; ++i)
        {
            float hue = static_cast<float>(i) * 0.3f + random.nextFloat() * 0.15f;
            float saturation = 0.5f + random.nextFloat() * 0.3f;
            float brightness = 0.7f + random.nextFloat() * 0.3f;
            trackColors_[i] = juce::Colour::fromHSV(hue, saturation, brightness, 1.0f);
        }
    }

    // Set track 0 as active by default
    setActiveTrack(0);
    applyTheme();
}

TrackPanelComponent::~TrackPanelComponent()
{
    // Clear LookAndFeel to avoid dangling pointers
    for (auto& track : tracks_)
    {
        track.volumeSlider.setLookAndFeel(nullptr);
    }
}

void TrackPanelComponent::applyTheme()
{
    for (auto& track : tracks_)
    {
        // 旋钮颜色
        track.volumeSlider.setColour(juce::Slider::rotarySliderFillColourId, UIColors::accent);
        track.volumeSlider.setColour(juce::Slider::rotarySliderOutlineColourId, UIColors::backgroundLight);
        track.volumeSlider.setColour(juce::Slider::thumbColourId, UIColors::accent);
    }

    repaint();
}

void TrackPanelComponent::paint(juce::Graphics& g)
{
    const auto& style = UIColors::currentThemeStyle();
    const auto themeId = UIColors::currentThemeId();
    // 阴影边距：背景在 reduced(12) 区域内绘制，阴影在边距内渲染
    const float shadowMargin = 12.0f;
    auto bounds = getLocalBounds().toFloat().reduced(shadowMargin);

    // Fill background (shadow is drawn internally by fillPanelBackground if needed, or we add it)
    if (themeId == ThemeId::Overdose)
    {
        UIColors::fillOverdosePanelBackground(g, bounds, style.panelRadius);
    }
    else if (themeId == ThemeId::Aurora)
    {
        UIColors::drawShadow(g, bounds);
        UIColors::fillTrackPanelBackground(g, bounds, style.panelRadius);
    }
    else if (themeId == ThemeId::DarkBlueGrey) {
        // Draw Soft Shadow for the whole panel
        UIColors::drawShadow(g, bounds);
        
        // Create rounded path for background and clipping
        juce::Path backgroundPath;
        backgroundPath.addRoundedRectangle(bounds, style.panelRadius);
        g.reduceClipRegion(backgroundPath);

        UIColors::fillPanelBackground(g, bounds, style.panelRadius);
        UIColors::drawPanelFrame(g, bounds, style.panelRadius);
    } else {
        UIColors::drawShadow(g, bounds);
        
        // Create rounded path for background and clipping
        juce::Path backgroundPath;
        backgroundPath.addRoundedRectangle(bounds, style.panelRadius);
        g.reduceClipRegion(backgroundPath);

        UIColors::fillPanelBackground(g, bounds, style.panelRadius);
        UIColors::drawPanelFrame(g, bounds, style.panelRadius);
    }

    // 使用动态轨道高度（与ArrangementView同步）
    const int dynamicTrackHeight = trackHeight_;
    
    // 轨道卡片的内边距 - 确保圆角不被截断
    const float trackCardMarginX = static_cast<float>(kTrackPanelCardInsetX);
    const float trackCardMarginY = static_cast<float>(kTrackPanelCardInsetY);

    // 左侧彩色竖线/色条的上下内缩量（统一语义，小于 controlRadius 以更贴边）
    constexpr float trackAccentVerticalInset = 3.0f;

    // Draw track backgrounds and separators
    for (int i = 0; i < visibleTrackCount_; ++i)
    {
        // 使用kTrackLaneTopOffset与ArrangementView对齐（默认30，匹配rulerHeight_）
        // Y坐标计算：kTrackLaneTopOffset + trackId * h - verticalScrollOffset_
        int y = kTrackLaneTopOffset + i * dynamicTrackHeight - verticalScrollOffset_;
        
        // 跳过完全不可见的轨道（性能优化）
        if (y + dynamicTrackHeight < 0 || y > getHeight())
            continue;
            
        juce::Rectangle<float> trackBounds(shadowMargin, static_cast<float>(y), 
                                           bounds.getWidth(), static_cast<float>(dynamicTrackHeight));
        
        if (themeId == ThemeId::Aurora)
        {
            const auto trackColor = trackColors_[i];
            const auto laneBounds = trackBounds.reduced(static_cast<float>(kTrackPanelCardInsetX), static_cast<float>(kTrackPanelCardInsetY));
            const auto tintBounds = laneBounds.withX(bounds.getX()).withRight(bounds.getRight());
            const auto active = tracks_[i].isActive;
            const bool isOverdose = (themeId == ThemeId::Overdose);
            // Overdose：轨道色本身即主题粉，tint 大幅降淡避免透过卡片形成高饱和粉色
            const float tintActiveA = isOverdose ? 0.10f : 0.34f;
            const float tintActiveB = isOverdose ? 0.06f : 0.24f;
            const float tintIdleA = isOverdose ? 0.08f : 0.20f;
            const float tintIdleB = isOverdose ? 0.04f : 0.13f;

            g.setColour((i % 2 == 0 ? UIColors::pianoRollLane : UIColors::glassSurface).withAlpha(active ? 0.058f : 0.036f));
            g.fillRect(tintBounds);

            if (active)
            {
                juce::ColourGradient rowGlow(trackColor.withAlpha(isOverdose ? 0.06f : 0.16f),
                                             tintBounds.getX(), tintBounds.getCentreY(),
                                             juce::Colours::transparentBlack,
                                             tintBounds.getRight(), tintBounds.getCentreY(), false);
                rowGlow.addColour(0.24, trackColor.withAlpha(isOverdose ? 0.04f : 0.10f));
                rowGlow.addColour(1.0, juce::Colours::transparentBlack);
                g.setGradientFill(rowGlow);
                g.fillRect(tintBounds);

                juce::ColourGradient edgeGlow(trackColor.withAlpha(isOverdose ? 0.05f : 0.12f),
                                              tintBounds.getX(), tintBounds.getY(),
                                              juce::Colours::transparentBlack,
                                              tintBounds.getX(), tintBounds.getBottom(), false);
                edgeGlow.addColour(0.50, trackColor.withAlpha(isOverdose ? 0.03f : 0.07f));
                edgeGlow.addColour(1.0, juce::Colours::transparentBlack);
                g.setGradientFill(edgeGlow);
                g.fillRect(tintBounds);
            }

            juce::ColourGradient tint(trackColor.withAlpha(active ? tintActiveA : tintIdleA),
                                      tintBounds.getX(), tintBounds.getCentreY(),
                                      juce::Colours::transparentBlack,
                                      tintBounds.getRight(), tintBounds.getCentreY(), false);
            tint.addColour(0.15, trackColor.withAlpha(active ? tintActiveB : tintIdleB));
            tint.addColour(0.48, UIColors::sidebarTrackFade.withAlpha(active ? 0.08f : 0.04f));
            tint.addColour(1.0, juce::Colours::transparentBlack);
            g.setGradientFill(tint);
            g.fillRect(tintBounds);

            const auto fadeRadius = juce::jmax(tintBounds.getWidth(), tintBounds.getHeight());
            juce::ColourGradient radial(trackColor.withAlpha(active ? (isOverdose ? 0.08f : 0.22f) : 0.10f),
                                        tintBounds.getX() + tintBounds.getWidth() * 0.16f,
                                        tintBounds.getCentreY(),
                                        juce::Colours::transparentBlack,
                                        tintBounds.getX() + fadeRadius,
                                        tintBounds.getBottom(),
                                        true);
            g.setGradientFill(radial);
            g.fillRect(tintBounds);

            const auto stripWidth = active ? 5.0f : 3.0f;
            auto stripBounds = tintBounds.withWidth(stripWidth).reduced(0.0f, trackAccentVerticalInset);
            g.setColour(trackColor.withAlpha(active ? 0.95f : 0.62f));
            g.fillRoundedRectangle(stripBounds, stripWidth * 0.5f);

            if (active)
            {
                g.setColour(trackColor.withAlpha(0.30f));
                g.drawLine(tintBounds.getX() + stripWidth + 4.0f, tintBounds.getY() + 1.0f,
                           tintBounds.getRight() - 8.0f, tintBounds.getY() + 1.0f, 1.0f);
            }

            g.setColour(UIColors::gridLine.withAlpha(active ? 0.075f : 0.040f));
            g.fillRect(trackBounds.getX(), trackBounds.getBottom() - 1.0f, trackBounds.getWidth(), 1.0f);
            
            continue; // Skip standard drawing
        }
        
        // 轨道卡片区域 - 增加内边距确保圆角完整显示
        auto cardBounds = trackBounds.reduced(trackCardMarginX, trackCardMarginY);

        if (themeId == ThemeId::BlueBreeze)
        {
            const auto trackTint = trackColors_[i].withAlpha(0.10f);
            const auto active = tracks_[i].isActive;
            UIColors::fillBlueBreezeTrackCard(g, cardBounds, style.controlRadius, active, trackTint);

            if (tracks_[i].isActive)
            {
                g.setColour(UIColors::accent.withAlpha(0.045f));
                g.fillRoundedRectangle(cardBounds.reduced(1.0f), juce::jmax(0.0f, style.controlRadius - 1.0f));

                const float x = cardBounds.getX() + 2.0f;
                const float y0 = cardBounds.getY() + trackAccentVerticalInset;
                const float y1 = cardBounds.getBottom() - trackAccentVerticalInset;
                g.setColour(UIColors::accent.withAlpha(0.66f));
                g.drawLine(x, y0, x, y1, 2.5f);
            }
        }
        else if (themeId == ThemeId::Overdose)
        {
            const auto active = tracks_[i].isActive;
            
            if (active)
            {
                // 激活轨道：极浅粉底（参考图：几乎白色带粉 tint）
                juce::Path cardShape;
                cardShape.addRoundedRectangle(cardBounds, style.controlRadius);

                // 柔和粉光晕
                juce::DropShadow cardGlow(
                    juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.22f),
                    12, { 0, 2 });
                cardGlow.drawForPath(g, cardShape);

                // 极浅粉底（更白更亮）
                g.setColour(juce::Colour(0xFFFFF5FA));  // 极浅粉白
                g.fillPath(cardShape);

                // 顶部高光
                auto highlightRect = cardBounds.reduced(1.5f).withHeight(cardBounds.getHeight() * 0.30f);
                juce::ColourGradient hl(
                    juce::Colour(0xFFFFFFFF).withAlpha(0.55f),
                    highlightRect.getX(), highlightRect.getY(),
                    juce::Colour(0xFFFFFFFF).withAlpha(0.0f),
                    highlightRect.getX(), highlightRect.getBottom(), false);
                g.setGradientFill(hl);
                g.fillRoundedRectangle(highlightRect, style.controlRadius - 1.5f);

                // 粉色边框（更淡更精致）
                g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.60f));
                g.drawRoundedRectangle(cardBounds.reduced(0.5f), style.controlRadius, 1.0f);

                // 左侧粉色竖线
                const float x = cardBounds.getX() + 2.0f;
                const float y0 = cardBounds.getY() + trackAccentVerticalInset;
                const float y1 = cardBounds.getBottom() - trackAccentVerticalInset;
                g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.75f));
                g.drawLine(x, y0, x, y1, 2.0f);
            }
            else
            {
                // 非激活轨道：淡薰衣草色（参考图：更淡更柔和）
                g.setColour(juce::Colour(0xFFF0EBF8));  // 淡薰衣草
                g.fillRoundedRectangle(cardBounds, style.controlRadius);
                
                // 微弱边框
                g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.32f));
                g.drawRoundedRectangle(cardBounds.reduced(0.5f), style.controlRadius, 0.8f);
            }
        }
        else if (themeId == ThemeId::DarkBlueGrey)
        {
            // 深蓝灰：清爽线条 + 轻洗色强调（避免厚重黑边）
            auto activeBg = UIColors::backgroundLight.brighter(0.03f);
            auto inactiveBg = UIColors::backgroundLight.darker(0.04f);
            g.setColour(tracks_[i].isActive ? activeBg : inactiveBg);
            g.fillRoundedRectangle(cardBounds, style.controlRadius);

            // Track color accent overlay
            if (tracks_[i].isActive || trackColorMode_ != TrackColorMode::Random)
            {
                g.setColour(trackColors_[i].withAlpha(tracks_[i].isActive ? 0.08f : 0.04f));
                g.fillRoundedRectangle(cardBounds, style.controlRadius);
            }

            if (tracks_[i].isActive)
            {
                // 轻洗色（强调色非常克制）
                g.setColour(UIColors::accent.withAlpha(0.10f));
                g.fillRoundedRectangle(cardBounds.reduced(1.0f), juce::jmax(0.0f, style.controlRadius - 1.0f));

                // 左侧亮线（现代软件常用的"激活"表达）
                const float x = cardBounds.getX() + 2.0f;
                const float y0 = cardBounds.getY() + trackAccentVerticalInset;
                const float y1 = cardBounds.getBottom() - trackAccentVerticalInset;
                g.setColour(UIColors::accent.withAlpha(0.90f));
                g.drawLine(x, y0, x, y1, 2.0f);
            }

            // 细边框
            g.setColour(UIColors::panelBorder.withAlpha(0.55f));
            g.drawRoundedRectangle(cardBounds, style.controlRadius, 1.0f);
        }
    }
}

void TrackPanelComponent::resized()
{
    // 阴影边距：内容区域在 reduced(12) 范围内布局
    const int shadowMargin = 12;
    auto contentBounds = getLocalBounds().reduced(shadowMargin);
    
    // 使用动态轨道高度（与ArrangementView同步）
    const int dynamicTrackHeight = trackHeight_;
    const int addButtonHeight = 60;
    
    // 轨道卡片内边距
    const int trackCardMarginX = kTrackPanelCardInsetX;
    const int trackCardMarginY = kTrackPanelCardInsetY;

    // 布局可见轨道
    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        auto& track = tracks_[i];
        bool visible = (i < visibleTrackCount_);
        
        // 更新控件可见性（紧凑模式：只保留S/M按钮和音量旋钮+电平表）
        track.muteButton.setVisible(visible);
        track.soloButton.setVisible(visible);
        track.volumeSlider.setVisible(visible);
        track.levelMeter.setVisible(visible);
        
        if (!visible)
            continue;
        
        // 轨道区域 - 使用kTrackLaneTopOffset与ArrangementView对齐
        // Y坐标计算：kTrackLaneTopOffset + trackId * h - verticalScrollOffset_
        int trackY = kTrackLaneTopOffset + i * dynamicTrackHeight - verticalScrollOffset_;
        auto trackBounds = juce::Rectangle<int>(contentBounds.getX(), trackY, 
                                                 contentBounds.getWidth(), dynamicTrackHeight);
        
        // 轨道卡片内部区域（与paint()中cardBounds对应）
        auto cardBounds = trackBounds.reduced(trackCardMarginX, trackCardMarginY);
        
        // 控件尺寸固定不变
        const int btnSize = 28;      // M/S按钮尺寸
        const int meterSize = 52;    // 电平表外环直径
        const int knobSize = 40;     // 旋钮直径
        const int gap = 6;           // 控件间距
        
        // 计算总宽度：S按钮 + 间距 + M按钮 + 间距 + 电平表
        int totalWidth = btnSize + gap + btnSize + gap + meterSize;
        int startX = cardBounds.getX() + (cardBounds.getWidth() - totalWidth) / 2;
        int centerY = cardBounds.getCentreY();
        
        // 环形电平表（最左）
        int meterX = startX;
        int meterY = centerY - meterSize / 2;
        track.levelMeter.setBounds(meterX, meterY, meterSize, meterSize);

        // 旋钮在电平表中心
        int knobOffset = (meterSize - knobSize) / 2;
        track.volumeSlider.setBounds(meterX + knobOffset, meterY + knobOffset, knobSize, knobSize);

        // Mute按钮（中间）
        int muteX = startX + meterSize + gap;
        track.muteButton.setBounds(muteX, centerY - btnSize / 2, btnSize, btnSize);

        // Solo按钮（最右）
        int soloX = muteX + btnSize + gap;
        track.soloButton.setBounds(soloX, centerY - btnSize / 2, btnSize, btnSize);
    }
    
    // +号按钮布局 - 放在最后一个可见轨道下方
    int addButtonY = kTrackLaneTopOffset + visibleTrackCount_ * dynamicTrackHeight - verticalScrollOffset_;
    auto addButtonBounds = juce::Rectangle<int>(contentBounds.getX(), addButtonY, 
                                                 contentBounds.getWidth(), addButtonHeight).reduced(4, 4);
    addTrackButton_.setBounds(addButtonBounds);
    addTrackButton_.setVisible(visibleTrackCount_ < MAX_TRACKS);
}

void TrackPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    // Right-click is handled in mouseUp; don't select track on right-click
    if (event.mods.isRightButtonDown())
        return;

    // 使用动态轨道高度和滚动偏移计算点击的轨道
    // 坐标转换：(event.y - kTrackLaneTopOffset + verticalScrollOffset_) / trackHeight_
    const int dynamicTrackHeight = trackHeight_;
    int clickedTrack = (event.y - kTrackLaneTopOffset + verticalScrollOffset_) / dynamicTrackHeight;

    // 只响应可见轨道的点击
    if (clickedTrack >= 0 && clickedTrack < visibleTrackCount_)
    {
        onTrackSelected(clickedTrack);
    }
}

void TrackPanelComponent::mouseUp(const juce::MouseEvent& event)
{
    if (!event.mods.isPopupMenu()) {
        juce::Component::mouseUp(event);
        return;
    }

    const int trackIndex = (event.y - kTrackLaneTopOffset + verticalScrollOffset_) / trackHeight_;
    if (trackIndex < 0 || trackIndex >= visibleTrackCount_)
        return;

    juce::PopupMenu menu;
    menu.addItem(1, TRANS("Add Track"));
    menu.addItem(2, TRANS("Duplicate Track"));
    menu.addSeparator();

    juce::PopupMenu colorSub;
    colorSub.addItem(3, TRANS("Custom Color..."));
    colorSub.addItem(4, TRANS("Random Color"));
    menu.addSubMenu(TRANS("Track Color"), colorSub);

    menu.addSeparator();
    menu.addItem(5, TRANS("Delete Track"));

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [this, trackIndex](int result) {
            switch (result) {
                case 1: listeners_.call([](Listener& l) { l.trackAddRequested(); }); break;
                case 2: listeners_.call([trackIndex](Listener& l) { l.trackDuplicateRequested(trackIndex); }); break;
                case 3: listeners_.call([trackIndex](Listener& l) { l.trackColorChangeRequested(trackIndex); }); break;
                case 4: listeners_.call([trackIndex](Listener& l) { l.trackColorRandomizeRequested(trackIndex); }); break;
                case 5: listeners_.call([trackIndex](Listener& l) { l.trackDeleteRequested(trackIndex); }); break;
                default: break;
            }
        });
}

// Shift + 鼠标滚轮：Y轴缩放（与ArrangementView同步）
// 普通滚轮：垂直滚动（与ArrangementView同步）
void TrackPanelComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    // Shift + 滚轮 = Y轴缩放
    if (event.mods.isShiftDown() && wheel.deltaY != 0.0f)
    {
        // 计算新高度
        int change = (wheel.deltaY > 0.0f) ? 50 : -50;  // 垂直缩放步长50px
        int newHeight = juce::jlimit(MIN_TRACK_HEIGHT, MAX_TRACK_HEIGHT, trackHeight_ + change);
        
        if (newHeight != trackHeight_)
        {
            setTrackHeight(newHeight);
            // 通知监听器高度变化（PluginEditor会同步到ArrangementView）
            listeners_.call([newHeight](Listener& l) { l.trackHeightChanged(newHeight); });
        }
        return;
    }
    
    // 普通滚轮 = 垂直滚动（与ArrangementView同步）
    if (wheel.deltaY != 0.0f)
    {
        // 计算总内容高度（可见轨道数量）
        const int totalContentHeight = visibleTrackCount_ * trackHeight_ + kTrackLaneTopOffset;
        // 可见区域高度
        const int visibleHeight = getHeight();
        
        // 如果内容不足以填满可见区域，不允许滚动
        if (totalContentHeight <= visibleHeight)
        {
            return;
        }
        
        // 计算最大滚动偏移（内容高度 - 可见高度）
        const int maxScrollOffset = totalContentHeight - visibleHeight;
        
        // 滚动速度：每次滚动移动100像素
        int scrollChange = static_cast<int>(-wheel.deltaY * 100.0f);
        int newOffset = verticalScrollOffset_ + scrollChange;
        
        // 限制滚动范围 [0, maxScrollOffset]
        newOffset = juce::jlimit(0, maxScrollOffset, newOffset);
        
        if (newOffset != verticalScrollOffset_)
        {
            verticalScrollOffset_ = newOffset;
            // 重新布局控件以跟随滚动
            resized();
            // 通知监听器滚动偏移变化（PluginEditor会同步到ArrangementView）
            listeners_.call([newOffset](Listener& l) { l.verticalScrollChanged(newOffset); });
            repaint();
        }
        return;
    }
    
    // 其他情况传递给父类
    juce::Component::mouseWheelMove(event, wheel);
}

// 设置轨道高度（由外部调用以保持同步）
void TrackPanelComponent::setTrackHeight(int height)
{
    int clampedHeight = juce::jlimit(MIN_TRACK_HEIGHT, MAX_TRACK_HEIGHT, height);
    if (clampedHeight != trackHeight_)
    {
        trackHeight_ = clampedHeight;
        resized();
        repaint();
    }
}

// 设置垂直滚动偏移（与ArrangementView同步）
void TrackPanelComponent::setVerticalScrollOffset(int offset)
{
    // 计算最大滚动偏移（可见轨道数量）
    const int totalContentHeight = visibleTrackCount_ * trackHeight_ + kTrackLaneTopOffset;
    const int visibleHeight = getHeight();
    const int maxScrollOffset = juce::jmax(0, totalContentHeight - visibleHeight);
    
    // 限制滚动范围 [0, maxScrollOffset]
    int newOffset = juce::jlimit(0, maxScrollOffset, offset);
    
    if (newOffset != verticalScrollOffset_)
    {
        verticalScrollOffset_ = newOffset;
        resized();
        repaint();
    }
}

void TrackPanelComponent::setTrackColour(int trackId, juce::Colour colour)
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        trackColors_[trackId] = colour;
        repaint();
    }
}

juce::Colour TrackPanelComponent::getTrackColour(int trackId) const
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
        return trackColors_[trackId];
    return juce::Colours::transparentBlack;
}

void TrackPanelComponent::setTrackColorMode(TrackColorMode mode)
{
    if (trackColorMode_ != mode)
    {
        trackColorMode_ = mode;
        repaint();
    }
}

void TrackPanelComponent::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void TrackPanelComponent::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void TrackPanelComponent::setActiveTrack(int trackId)
{
    if (trackId < 0 || trackId >= MAX_TRACKS)
        return;

    activeTrackId_ = trackId;

    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        tracks_[i].isActive = (i == trackId);
        updateTrackAppearance(i);
    }
}

void TrackPanelComponent::setTrackMuted(int trackId, bool muted)
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        tracks_[trackId].muteButton.setToggleState(muted, juce::dontSendNotification);
    }
}

bool TrackPanelComponent::isTrackMuted(int trackId) const
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        return tracks_[trackId].muteButton.getToggleState();
    }
    return false;
}

void TrackPanelComponent::setTrackSolo(int trackId, bool solo)
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        tracks_[trackId].soloButton.setToggleState(solo, juce::dontSendNotification);
    }
}

bool TrackPanelComponent::isTrackSolo(int trackId) const
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        return tracks_[trackId].soloButton.getToggleState();
    }
    return false;
}

void TrackPanelComponent::setTrackVolume(int trackId, float volume)
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        tracks_[trackId].volumeSlider.setValue(volume, juce::dontSendNotification);
    }
}

float TrackPanelComponent::getTrackVolume(int trackId) const
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        return static_cast<float>(tracks_[trackId].volumeSlider.getValue());
    }
    return 1.0f;
}

void TrackPanelComponent::onTrackSelected(int trackId)
{
    setActiveTrack(trackId);
    listeners_.call([trackId](Listener& l) { l.trackSelected(trackId); });
}

void TrackPanelComponent::onMuteToggled(int trackId)
{
    bool muted = tracks_[trackId].muteButton.getToggleState();
    listeners_.call([trackId, muted](Listener& l) { l.trackMuteToggled(trackId, muted); });
}

void TrackPanelComponent::onSoloToggled(int trackId)
{
    bool solo = tracks_[trackId].soloButton.getToggleState();
    listeners_.call([trackId, solo](Listener& l) { l.trackSoloToggled(trackId, solo); });
}

void TrackPanelComponent::onVolumeChanged(int trackId)
{
    float volume = getTrackVolume(trackId);
    listeners_.call([trackId, volume](Listener& l) { l.trackVolumeChanged(trackId, volume); });
}

void TrackPanelComponent::updateTrackAppearance(int trackId)
{
    juce::ignoreUnused(trackId);
    // Trigger repaint to update background highlight
    repaint();
}

void TrackPanelComponent::setTrackLevel(int trackId, float levelDB)
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        tracks_[trackId].levelMeter.setLevel(levelDB);
    }
}

void TrackPanelComponent::setTrackClipping(int trackId, bool isClipping)
{
    if (trackId >= 0 && trackId < MAX_TRACKS)
    {
        tracks_[trackId].levelMeter.setClipping(isClipping);
    }
}

void TrackPanelComponent::setInferenceActive(bool active)
{
    for (auto& track : tracks_)
        track.levelMeter.setInferenceActive(active);

    addTrackButton_.setInferenceActive(active);
}

// 设置可见轨道数量
void TrackPanelComponent::setVisibleTrackCount(int count)
{
    int clamped = juce::jlimit(1, MAX_TRACKS, count);
    if (clamped == visibleTrackCount_)
        return;
    visibleTrackCount_ = clamped;
    // Re-clamp scroll offset for new track count
    setVerticalScrollOffset(verticalScrollOffset_);
    resized();
    repaint();
    listeners_.call([this](Listener& l) { l.visibleTrackCountChanged(visibleTrackCount_); });
}

// 增加可见轨道数量（点击+号按钮时调用）
void TrackPanelComponent::showMoreTracks()
{
    if (visibleTrackCount_ < MAX_TRACKS)
    {
        // 每次增加1条轨道
        setVisibleTrackCount(visibleTrackCount_ + 1);
    }
}

} // namespace OpenTune

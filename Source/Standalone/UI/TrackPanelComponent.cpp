#include "TrackPanelComponent.h"
#include "UIColors.h"

namespace OpenTune {

// MuteSoloIconButton implementation
void MuteSoloIconButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    bool isToggled = getToggleState();

    juce::ignoreUnused(bounds);
    
    // Custom colors requested by user: Mute = Deep Red, Solo = Orange
    juce::Colour activeBase;
    if (iconType_ == IconType::Mute)
        activeBase = juce::Colour(0xFF8B0000); // Deep Red
    else
        activeBase = juce::Colour(0xFFE67E22); // Orange (Standard Studio Color)

    auto base = isToggled ? activeBase : UIColors::buttonNormal;
    getLookAndFeel().drawButtonBackground(g, *this, base, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

    // Icon text
    g.setColour(UIColors::textPrimary);
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
    const auto& style = Theme::getActiveStyle();
    const auto themeId = Theme::getActiveTheme();
    // 阴影边距：背景在 reduced(12) 区域内绘制，阴影在边距内渲染
    const float shadowMargin = 12.0f;
    auto bounds = getLocalBounds().toFloat().reduced(shadowMargin);

    // Fill background (shadow is drawn internally by fillPanelBackground if needed, or we add it)
    if (themeId == ThemeId::Aurora)
    {
        // Aurora: Flat background, no frame, colorful tracks
        UIColors::drawShadow(g, bounds);
        
        // Background
        g.setColour(UIColors::backgroundMedium);
        g.fillRect(bounds);
        
        // No Panel Frame for Aurora to "dilute boundary"
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
    const float trackCardMarginX = 8.0f;
    const float trackCardMarginY = 6.0f;

    // Draw track backgrounds and separators
    for (int i = 0; i < visibleTrackCount_; ++i)
    {
        // 使用trackStartYOffset_与ArrangementView对齐（默认30，匹配rulerHeight_）
        // Y坐标计算：trackStartYOffset_ + trackId * h - verticalScrollOffset_
        int y = trackStartYOffset_ + i * dynamicTrackHeight - verticalScrollOffset_;
        
        // 跳过完全不可见的轨道（性能优化）
        if (y + dynamicTrackHeight < 0 || y > getHeight())
            continue;
            
        juce::Rectangle<float> trackBounds(shadowMargin, static_cast<float>(y), 
                                           bounds.getWidth(), static_cast<float>(dynamicTrackHeight));
        
        if (themeId == ThemeId::Aurora)
        {
            // Aurora 主题：霓虹风格，锐利边缘配合彩色轨道
            // 使用颜色循环支持12条轨道
            juce::Colour trackColor;
            switch(i % 6) {
                case 0: trackColor = juce::Colour(Aurora::Colors::Cyan); break;
                case 1: trackColor = juce::Colour(Aurora::Colors::Violet); break;
                case 2: trackColor = juce::Colour(Aurora::Colors::NeonGreen); break;
                case 3: trackColor = juce::Colour(Aurora::Colors::Magenta); break;
                case 4: trackColor = juce::Colour(Aurora::Colors::ElectricBlue); break;
                case 5: trackColor = juce::Colour(Aurora::Colors::Warning); break;
            }

            // 平时显示各轨道颜色，选中时背景变亮
            if (tracks_[i].isActive)
            {
                // Active: 背景整体变亮
                g.setColour(trackColor.withAlpha(0.25f));
                g.fillRect(trackBounds);

                // Left Strip (Neon Glow)
                g.setColour(trackColor);
                g.fillRect(trackBounds.getX(), trackBounds.getY(), 3.0f, trackBounds.getHeight());
                
                // Gradient Overlay - 更亮的渐变
                juce::ColourGradient highlight(trackColor.withAlpha(0.35f), trackBounds.getX(), trackBounds.getCentreY(),
                                               juce::Colours::transparentBlack, trackBounds.getX() + 80.0f, trackBounds.getCentreY(), false);
                g.setGradientFill(highlight);
                g.fillRect(trackBounds);
            }
            else
            {
                // Inactive: 显示各轨道颜色（平时状态）
                g.setColour(trackColor.withAlpha(0.12f));
                g.fillRect(trackBounds);

                // Left Strip (subtle)
                g.setColour(trackColor.withAlpha(0.6f));
                g.fillRect(trackBounds.getX(), trackBounds.getY(), 3.0f, trackBounds.getHeight());
            }

            // Separator (very subtle)
            g.setColour(UIColors::panelBorder.withAlpha(0.3f));
            g.fillRect(trackBounds.getX(), trackBounds.getBottom() - 1.0f, trackBounds.getWidth(), 1.0f);
            
            continue; // Skip standard drawing
        }
        
        // 轨道卡片区域 - 增加内边距确保圆角完整显示
        auto cardBounds = trackBounds.reduced(trackCardMarginX, trackCardMarginY);

        // BlueBreeze 主题：淡彩色背景 + 渐变
        if (themeId == ThemeId::BlueBreeze || themeId != ThemeId::DarkBlueGrey)
        {
            // 获取该轨道的淡彩色
            juce::Colour pastelColor(trackPastelColors[i % 12]);
            
            // 基础背景色
            auto baseBg = tracks_[i].isActive 
                ? UIColors::backgroundLight.brighter(0.12f)
                : UIColors::backgroundLight.darker(0.05f);
            
            // 混合淡彩色和基础背景
            auto mixedBg = baseBg.overlaidWith(pastelColor);
            
            // 绘制渐变背景（从上到下，淡彩色渐变到基础灰色）
            juce::ColourGradient cardGrad(
                mixedBg.brighter(0.05f), cardBounds.getX(), cardBounds.getY(),
                baseBg, cardBounds.getX(), cardBounds.getBottom(), false);
            g.setGradientFill(cardGrad);
            g.fillRoundedRectangle(cardBounds, style.controlRadius);
            
            if (tracks_[i].isActive)
            {
                // 选中时添加强调色覆盖层
                g.setColour(UIColors::accent.withAlpha(0.12f));
                g.fillRoundedRectangle(cardBounds.reduced(1.0f), juce::jmax(0.0f, style.controlRadius - 1.0f));
                
                // 左侧亮线（激活指示）
                const float x = cardBounds.getX() + 2.0f;
                const float y0 = cardBounds.getY() + style.controlRadius;
                const float y1 = cardBounds.getBottom() - style.controlRadius;
                g.setColour(UIColors::accent.withAlpha(0.90f));
                g.drawLine(x, y0, x, y1, 2.5f);
            }
            
            // 绘制边框
            g.setColour(UIColors::panelBorder.withAlpha(0.6f));
            g.drawRoundedRectangle(cardBounds, style.controlRadius, style.strokeThin);
        }
        else if (themeId == ThemeId::DarkBlueGrey)
        {
            // 深蓝灰：清爽线条 + 轻洗色强调（避免厚重黑边）
            auto activeBg = UIColors::backgroundLight.brighter(0.03f);
            auto inactiveBg = UIColors::backgroundLight.darker(0.04f);
            g.setColour(tracks_[i].isActive ? activeBg : inactiveBg);
            g.fillRoundedRectangle(cardBounds, style.controlRadius);

            if (tracks_[i].isActive)
            {
                // 轻洗色（强调色非常克制）
                g.setColour(UIColors::accent.withAlpha(0.10f));
                g.fillRoundedRectangle(cardBounds.reduced(1.0f), juce::jmax(0.0f, style.controlRadius - 1.0f));

                // 左侧亮线（现代软件常用的"激活"表达）
                const float x = cardBounds.getX() + 2.0f;
                const float y0 = cardBounds.getY() + style.controlRadius;
                const float y1 = cardBounds.getBottom() - style.controlRadius;
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
    const int trackCardMarginX = 8;
    const int trackCardMarginY = 6;

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
        
        // 轨道区域 - 使用trackStartYOffset_与ArrangementView对齐
        // Y坐标计算：trackStartYOffset_ + trackId * h - verticalScrollOffset_
        int trackY = trackStartYOffset_ + i * dynamicTrackHeight - verticalScrollOffset_;
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
    int addButtonY = trackStartYOffset_ + visibleTrackCount_ * dynamicTrackHeight - verticalScrollOffset_;
    auto addButtonBounds = juce::Rectangle<int>(contentBounds.getX(), addButtonY, 
                                                 contentBounds.getWidth(), addButtonHeight).reduced(4, 4);
    addTrackButton_.setBounds(addButtonBounds);
    addTrackButton_.setVisible(visibleTrackCount_ < MAX_TRACKS);
}

void TrackPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    // 使用动态轨道高度和滚动偏移计算点击的轨道
    // 坐标转换：(event.y - trackStartYOffset_ + verticalScrollOffset_) / trackHeight_
    const int dynamicTrackHeight = trackHeight_;
    int clickedTrack = (event.y - trackStartYOffset_ + verticalScrollOffset_) / dynamicTrackHeight;

    // 只响应可见轨道的点击
    if (clickedTrack >= 0 && clickedTrack < visibleTrackCount_)
    {
        onTrackSelected(clickedTrack);
    }
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
        // 计算总内容高度（12轨道）
        const int totalContentHeight = MAX_TRACKS * trackHeight_ + trackStartYOffset_;
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
    // 计算最大滚动偏移
    const int totalContentHeight = MAX_TRACKS * trackHeight_ + trackStartYOffset_;
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

// 设置轨道起始Y偏移（与ArrangementView的rulerHeight对齐）
void TrackPanelComponent::setTrackStartYOffset(int offset)
{
    if (offset != trackStartYOffset_)
    {
        trackStartYOffset_ = offset;
        resized();
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
    visibleTrackCount_ = juce::jlimit(1, MAX_TRACKS, count);
    resized();
    repaint();
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

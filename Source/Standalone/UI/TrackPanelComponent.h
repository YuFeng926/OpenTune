#pragma once

/**
 * 轨道面板组件
 * 
 * 显示和控制多轨道的侧边面板：
 * - 轨道选择
 * - Mute/Solo 控制
 * - 音量旋钮和电平表
 * - 添加轨道按钮
 * 
 * 支持动态轨道高度和滚动同步。
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include "UIColors.h"

namespace OpenTune {

// 轨道面板常量
static constexpr int TRACK_PANEL_WIDTH = 120; // 紧凑模式宽度
static constexpr int MAX_TRACKS = 12;           // 最大轨道数量
static constexpr int DEFAULT_VISIBLE_TRACKS = 2; // 默认显示轨道数量

    // 12种轨道淡彩色（柔和不抢主题，BlueBreeze 主题专用）
    static constexpr juce::uint32 trackPastelColors[12] = {
        0x18E8F5FF,  // 淡蓝
        0x18FFE8F0,  // 淡粉
        0x18E8FFE8,  // 淡绿
        0x18FFF5E8,  // 淡橙
        0x18F0E8FF,  // 淡紫
        0x18FFFFE8,  // 淡黄
        0x18E8FFFF,  // 淡青
        0x18FFE8E8,  // 淡红
        0x18F5FFE8,  // 淡柠檬
        0x18E8E8FF,  // 淡靛蓝
        0x18FFE8FF,  // 淡品红
        0x18E8FFF5,  // 淡薄荷
    };

    // 环形LED电平表 - 包裹在音量旋钮外圈
    class CircularLevelMeter : public juce::Component, private juce::Timer
    {
    public:
        CircularLevelMeter()
        {
            startTimerHz(30);  // 30Hz 刷新率
        }

        ~CircularLevelMeter() override
        {
            stopTimer();
        }

        // 设置电平值 (dB)
        void setLevel(float newLevelDB)
        {
            targetLevel_ = juce::jlimit(-60.0f, 6.0f, newLevelDB);
        }

        // 设置过载状态
        void setClipping(bool isClipping)
        {
            if (isClipping && !isClipping_)
            {
                isClipping_ = true;
                clipHoldTime_ = 60;  // 保持2秒 (30Hz)
            }
        }

        void setInferenceActive(bool active)
        {
            startTimerHz(active ? 12 : 30);
        }

        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            auto center = bounds.getCentre();
            
            // 外环半径和内环半径
            float outerRadius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;
            float innerRadius = outerRadius - 5.0f;  // LED环宽度 5px
            
            // 弧度范围 (与旋钮一致，约270度)
            const float startAngle = juce::MathConstants<float>::pi * 1.25f;   // 从左下开始
            const float endAngle = juce::MathConstants<float>::pi * 2.75f;     // 到右下结束
            const float totalAngle = endAngle - startAngle;
            
            // LED段数
            const int numSegments = 24;
            const float segmentGap = 0.03f;  // 段间隙
            const float segmentAngle = (totalAngle - segmentGap * numSegments) / numSegments;
            
            // 计算当前电平对应的段数
            float norm = juce::jmap(currentLevel_, -60.0f, 6.0f, 0.0f, 1.0f);
            norm = juce::jlimit(0.0f, 1.0f, norm);
            int litSegments = static_cast<int>(norm * numSegments);
            
            // 绘制每个LED段
            for (int i = 0; i < numSegments; ++i)
            {
                float segStart = startAngle + i * (segmentAngle + segmentGap);
                float segEnd = segStart + segmentAngle;
                
                // 获取该段的颜色
                juce::Colour segColor;
                float segPos = static_cast<float>(i) / numSegments;
                
                if (segPos < 0.6f)
                {
                    // 绿色区域 (0-60%)
                    segColor = juce::Colour(0xFF2ECC71);  // 绿色
                }
                else if (segPos < 0.8f)
                {
                    // 黄色区域 (60-80%)
                    segColor = juce::Colour(0xFFF39C12);  // 黄色
                }
                else if (segPos < 0.92f)
                {
                    // 橙色区域 (80-92%)
                    segColor = juce::Colour(0xFFE67E22);  // 橙色
                }
                else
                {
                    // 红色区域 (92-100%)
                    segColor = juce::Colour(0xFFE74C3C);  // 红色
                }
                
                // 判断是否点亮
                bool isLit = (i < litSegments);
                
                // 过载时最后几段闪烁
                if (isClipping_ && i >= numSegments - 2)
                {
                    isLit = true;
                    segColor = juce::Colour(0xFFFF4444);  // 过载红
                }
                
                // 绘制弧形段
                juce::Path arcPath;
                arcPath.addCentredArc(center.x, center.y, outerRadius, outerRadius,
                                      0.0f, segStart, segEnd, true);
                arcPath.addCentredArc(center.x, center.y, innerRadius, innerRadius,
                                      0.0f, segEnd, segStart, false);
                arcPath.closeSubPath();
                
                if (isLit)
                {
                    // 点亮状态 - 使用渐变增加立体感
                    g.setColour(segColor);
                    g.fillPath(arcPath);
                    
                    // 添加内发光效果
                    g.setColour(segColor.brighter(0.3f).withAlpha(0.5f));
                    g.strokePath(arcPath, juce::PathStrokeType(1.0f));
                }
                else
                {
                    // 熄灭状态 - 暗灰色
                    g.setColour(UIColors::backgroundLight.darker(0.3f));
                    g.fillPath(arcPath);
                }
            }
        }

    private:
        float targetLevel_ = -60.0f;
        float currentLevel_ = -60.0f;
        bool isClipping_ = false;
        int clipHoldTime_ = 0;

        void timerCallback() override
        {
            // 平滑衰减
            if (currentLevel_ < targetLevel_)
                currentLevel_ = targetLevel_;
            else
                currentLevel_ = currentLevel_ * 0.85f + targetLevel_ * 0.15f;

            // 过载保持逻辑
            if (isClipping_)
            {
                if (clipHoldTime_ > 0)
                    clipHoldTime_--;
                else
                    isClipping_ = false;
            }

            repaint();
        }
    };

    // Mute/Solo 控制的图标按钮
    class MuteSoloIconButton : public juce::Button
    {
    public:
        enum class IconType { Mute, Solo };

        MuteSoloIconButton(const juce::String& name, IconType type)
            : juce::Button(name), iconType_(type) {}

        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    private:
        IconType iconType_;
    };

    // 音量旋钮的自定义 LookAndFeel
    class VolumeKnobLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        VolumeKnobLookAndFeel()
        {
            setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::black);
            setColour(juce::Slider::thumbColourId, juce::Colours::white);
        }

        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
            const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider& slider) override
        {
            auto radius = (float)juce::jmin(width / 2, height / 2) - 2.0f;
            auto centreX = (float)x + (float)width * 0.5f;
            auto centreY = (float)y + (float)height * 0.5f;
            auto rx = centreX - radius;
            auto ry = centreY - radius;
            auto rw = radius * 2.0f;
            auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

            // 1. Black Background Circle
            g.setColour(juce::Colours::black);
            g.fillEllipse(rx, ry, rw, rw);

            // 2. White Pointer (Needle)
            juce::Path p;
            float pointerLength = radius * 0.7f;
            float pointerThickness = 3.0f;
            p.addRectangle(-pointerThickness * 0.5f, -radius * 0.8f, pointerThickness, pointerLength);
            p.applyTransform(juce::AffineTransform::rotation(angle).translated(centreX, centreY));

            g.setColour(juce::Colours::white);
            g.fillPath(p);

            // Border (Optional, for better visibility)
            g.setColour(juce::Colours::white.withAlpha(0.2f));
            g.drawEllipse(rx, ry, rw, rw, 1.0f);
        }
    };

    // 自定义音量旋钮（旋转式）
    class VolumeKnob : public juce::Slider
    {
    public:
        VolumeKnob() : juce::Slider()
        {
            setSliderStyle(juce::Slider::RotaryVerticalDrag);
            setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            
            // Gain range: 0.0 to 4.0 (approx +12dB)
            setRange(0.0, 4.0, 0.001);
            setSkewFactorFromMidPoint(1.0); // 1.0 (0dB) is at the center
            
            setValue(1.0, juce::dontSendNotification);
            setDoubleClickReturnValue(true, 1.0); // Double click resets to 0dB (1.0 gain)
        }

        bool hitTest(int x, int y) override
        {
            auto bounds = getLocalBounds().toFloat();
            float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
            float dist = bounds.getCentre().getDistanceFrom(juce::Point<float>(static_cast<float>(x), static_cast<float>(y)));
            return dist <= radius;
        }
    };

    // 带悬停效果的透明标签
    class TransparentLabel : public juce::Label
    {
    public:
        TransparentLabel()
        {
            setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        }

        void mouseEnter(const juce::MouseEvent& e) override
        {
            setColour(juce::Label::outlineColourId, UIColors::accent.withAlpha(0.5f));
            repaint();
            juce::Label::mouseEnter(e);
        }

        void mouseExit(const juce::MouseEvent& e) override
        {
            setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
            repaint();
            juce::Label::mouseExit(e);
        }
    };

// 添加轨道按钮 - 带触觉反馈的+号按钮
class AddTrackButton : public juce::Component,
                       public juce::TooltipClient,
                       private juce::Timer
{
public:
    AddTrackButton()
    {
        startTimerHz(60);
    }

    ~AddTrackButton() override
    {
        stopTimer();
    }

    std::function<void()> onClick;

    // TooltipClient 接口实现
    juce::String getTooltip() override { return tooltip_; }
    void setTooltipText(const juce::String& tooltip) { tooltip_ = tooltip; }

    void setInferenceActive(bool active)
    {
        startTimerHz(active ? 30 : 60);
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(4.0f);
        const auto& style = Theme::getActiveStyle();
        
        // 背景
        auto bgColor = UIColors::backgroundLight;
        if (isMouseOver_)
            bgColor = bgColor.brighter(0.1f);
        if (isMouseDown_)
            bgColor = bgColor.darker(0.1f);
        
        g.setColour(bgColor);
        g.fillRoundedRectangle(bounds, style.controlRadius);
        
        // 边框
        g.setColour(UIColors::panelBorder.withAlpha(isMouseOver_ ? 0.8f : 0.5f));
        g.drawRoundedRectangle(bounds, style.controlRadius, 1.0f);
        
        // +号图标
        auto iconColor = isMouseOver_ ? UIColors::accent : UIColors::textSecondary;
        g.setColour(iconColor);
        g.setFont(UIColors::getUIFont(24.0f));
        g.drawText("+", bounds, juce::Justification::centred);
        
        // Hover 发光效果
        if (glowAlpha_ > 0.0f)
        {
            g.setColour(UIColors::accent.withAlpha(glowAlpha_ * 0.3f));
            g.drawRoundedRectangle(bounds.expanded(2.0f), style.controlRadius + 2.0f, 2.0f);
        }
        
        // 点击涟漪效果
        for (const auto& ripple : ripples_)
        {
            float radius = ripple.radius;
            float alpha = ripple.alpha;
            g.setColour(UIColors::accent.withAlpha(alpha * 0.4f));
            g.drawEllipse(ripple.x - radius, ripple.y - radius, radius * 2, radius * 2, 2.0f);
        }
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        isMouseOver_ = true;
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        isMouseOver_ = false;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        isMouseDown_ = true;
        
        // 添加涟漪
        Ripple r;
        r.x = static_cast<float>(e.x);
        r.y = static_cast<float>(e.y);
        r.radius = 5.0f;
        r.alpha = 1.0f;
        ripples_.push_back(r);
        
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        isMouseDown_ = false;
        if (onClick && getLocalBounds().contains(e.x, e.y))
            onClick();
        repaint();
    }

private:
    struct Ripple
    {
        float x, y;
        float radius;
        float alpha;
    };

    bool isMouseOver_ = false;
    bool isMouseDown_ = false;
    float glowAlpha_ = 0.0f;
    std::vector<Ripple> ripples_;
    juce::String tooltip_;

    void timerCallback() override
    {
        bool needsRepaint = false;
        
        // 更新发光效果
        float targetGlow = isMouseOver_ ? 1.0f : 0.0f;
        if (std::abs(glowAlpha_ - targetGlow) > 0.01f)
        {
            glowAlpha_ += (targetGlow - glowAlpha_) * 0.2f;
            needsRepaint = true;
        }
        
        // 更新涟漪
        for (auto it = ripples_.begin(); it != ripples_.end();)
        {
            it->radius += 3.0f;
            it->alpha -= 0.05f;
            if (it->alpha <= 0.0f)
                it = ripples_.erase(it);
            else
                ++it;
            needsRepaint = true;
        }
        
        if (needsRepaint)
            repaint();
    }
};

class TrackPanelComponent : public juce::Component
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void trackSelected(int trackId) = 0;
        virtual void trackMuteToggled(int trackId, bool muted) = 0;
        virtual void trackSoloToggled(int trackId, bool solo) = 0;
        virtual void trackVolumeChanged(int trackId, float volume) = 0;
        // Y轴缩放回调 - 通知外部轨道高度变化
        virtual void trackHeightChanged(int newHeight) { juce::ignoreUnused(newHeight); }
        // 垂直滚动回调 - 通知外部滚动偏移变化（用于同步ArrangementView）
        virtual void verticalScrollChanged(int offset) { juce::ignoreUnused(offset); }
    };

    // 构造函数，高度由外部传入
    TrackPanelComponent();
    ~TrackPanelComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    void applyTheme();

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void setActiveTrack(int trackId);
    int getActiveTrack() const { return activeTrackId_; }

    void setTrackMuted(int trackId, bool muted);
    bool isTrackMuted(int trackId) const;

    void setTrackSolo(int trackId, bool solo);
    bool isTrackSolo(int trackId) const;

    void setTrackVolume(int trackId, float volume);
    float getTrackVolume(int trackId) const;

    void setTrackLevel(int trackId, float levelDB);
    void setTrackClipping(int trackId, bool isClipping);
    void setInferenceActive(bool active);

    // 可见轨道数量控制
    void setVisibleTrackCount(int count);
    int getVisibleTrackCount() const { return visibleTrackCount_; }
    void showMoreTracks();  // 增加可见轨道数量

    // 动态轨道高度控制 - 与ArrangementView同步
    void setTrackHeight(int height);
    int getTrackHeight() const { return trackHeight_; }
    
    // 垂直滚动偏移 - 与ArrangementView同步（Y轴滚动同步）
    void setVerticalScrollOffset(int offset);
    int getVerticalScrollOffset() const { return verticalScrollOffset_; }
    
    // 轨道起始Y偏移 - 与ArrangementView的rulerHeight对齐
    void setTrackStartYOffset(int offset);
    int getTrackStartYOffset() const { return trackStartYOffset_; }
    
    // 轨道高度限制常量
    static constexpr int MIN_TRACK_HEIGHT = 70;   // 最小高度：控件不重叠
    static constexpr int DEFAULT_TRACK_HEIGHT = 100;  // 默认高度
    static constexpr int MAX_TRACK_HEIGHT = 300;  // 最大高度：窗口1/3左右

private:
    struct TrackControl
    {
        // 只保留3个核心控件：Solo、Mute按钮和音量旋钮（带电平表）
        MuteSoloIconButton muteButton { "M", MuteSoloIconButton::IconType::Mute };
        MuteSoloIconButton soloButton { "S", MuteSoloIconButton::IconType::Solo };
        VolumeKnob volumeSlider;
        CircularLevelMeter levelMeter;  // 环形LED电平表（包裹旋钮）
        bool isActive{false};
    };

    std::array<TrackControl, MAX_TRACKS> tracks_;  // 12条轨道
    int activeTrackId_{0};
    int visibleTrackCount_{DEFAULT_VISIBLE_TRACKS};  // 当前可见轨道数量，默认2
    int trackHeight_{DEFAULT_TRACK_HEIGHT};  // 动态轨道高度
    int verticalScrollOffset_{0};  // 垂直滚动偏移（与ArrangementView同步）
    int trackStartYOffset_{30};    // 轨道起始Y偏移（默认30，与ArrangementView的rulerHeight_对齐）
    AddTrackButton addTrackButton_;  // +号按钮
    VolumeKnobLookAndFeel knobLnF_;  // Custom LookAndFeel for knobs

    juce::ListenerList<Listener> listeners_;

    void onTrackSelected(int trackId);
    void onMuteToggled(int trackId);
    void onSoloToggled(int trackId);
    void onVolumeChanged(int trackId);

    void updateTrackAppearance(int trackId);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackPanelComponent)
};

} // namespace OpenTune

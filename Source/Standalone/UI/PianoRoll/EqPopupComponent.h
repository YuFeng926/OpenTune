/**
 * EQ Popup Component — per-note EQ 编辑弹窗
 *
 * 动态滤波器列表：最多 kMaxFilters=10。
 * 浮动参数卡 + 双击创建/删除 + 滚轮调 Q + Minimize 行为随态变化。
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/NoteEqSettings.h"
#include "Utils/SpectrumDisplayData.h"
#include "EqGraphRenderer.h"
#include "EqBandInteraction.h"
#include <memory>

namespace OpenTune {

class LargeKnobLookAndFeel;

class EqPopupComponent : public juce::Component,
                          public juce::Timer {
public:
    EqPopupComponent();
    ~EqPopupComponent() override;

    void setEqSettings(const EqSettings& settings);
    void setPreviewMode(bool isPreview);
    void setRemoveConfirmationSuppressed(bool suppress);
    void setNoteColor(juce::Colour color);

    std::function<void(const EqSettings&)> onCommitSettings;
    std::function<void()> onRemoveEq;
    std::function<void()> onClose;
    std::function<void(bool)> onRemoveConfirmationSuppressed;
    std::function<void(SpectrumArray&, SpectrumArray&)> onReadSpectrum;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    void timerCallback() override;

    bool isPreviewMode() const { return isPreview_; }
    void toggleMaximize();

private:
    enum class ButtonId { None = -1, Maximize = 0, Bypass, Remove, Minimize };
    void paintButton(juce::Graphics& g, ButtonId id, juce::Rectangle<float> bounds, bool hovered) const;
    juce::Rectangle<float> buttonBounds(ButtonId id) const;
    ButtonId hitTestButton(juce::Point<float> pos) const;

    // 卡片内旁通/删除按钮（仅完整视图）
    enum class CardButton { None = -1, Bypass = 0, Remove = 1 };
    juce::Rectangle<float> cardButtonBounds(CardButton btn, const juce::Rectangle<float>& cardBounds) const;
    CardButton hitTestCardButton(juce::Point<float> pos) const;

    // 浮动参数卡（固定底部居中）
    juce::Rectangle<float> floatingCardBounds() const;
    void paintFloatingCard(juce::Graphics& g, int filterIndex) const;
    void updateCardState();

    // 卡片内旋钮控件
    void configureCardControls(int filterIndex);
    void hideCardControls();
    void layoutCardControls();
    void cardSliderChanged(int parameterIndex);

    // 卡片内滤波器类型按钮
    juce::Rectangle<float> cardFilterTypeButtonBounds(const juce::Rectangle<float>& cardBounds, int index) const;
    void paintFilterTypeButton(juce::Graphics& g, EqFilterType type, const juce::Rectangle<float>& bounds,
                               bool isActive, bool isHovered) const;
    int hitTestFilterTypeButton(juce::Point<float> pos) const;

    void dismissRemoveConfirmation();

    void paintBypassIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintRemoveIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintMaximizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintMinimizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;

    EqSettings settings_;
    EqGraphRenderer renderer_;
    EqBandInteraction interaction_;
    SpectrumArray spectrum_{};
    SpectrumArray spectrumPeaks_{};
    juce::Colour noteColor_;
    bool isPreview_ = true;
    bool isMaximized_ = false;
    int hoveredBand_ = -1;
    int hoveredButton_ = -1;
    int hoveredViewRange_ = -1;
    int pressedViewRange_ = -1;
    int selectedFilterIndex_ = -1;
    juce::Rectangle<int> savedPreviewBounds_;

    // 浮动参数卡状态
    int cardBand_ = -1;
    int hoveredTypeButton_ = -1;
    int hoveredCardButton_ = -1;

    // 卡片内旋钮控件
    std::unique_ptr<LargeKnobLookAndFeel> cardKnobLookAndFeel_;
    juce::Slider frequencySlider_;
    juce::Slider gainSlider_;
    juce::Slider qSlider_;
    bool updatingCardControls_ = false;

    std::vector<double> hoverBandAmounts_ = std::vector<double>(EqSettings::kMaxFilters, 0.0);
    static constexpr double kHoverFadeInTime = 0.50;
    static constexpr double kHoverFadeOutTime = 0.50;

    bool dragCommitted_ = false;
    bool wasDragging_ = false;
    bool draggingWindow_ = false;
    juce::Point<int> dragOffset_;
    int pendingDragBand_ = -1;
    juce::Point<float> pendingDragStartPos_;

    bool showingRemoveConfirmation_ = false;
    bool suppressRemoveConfirmation_ = false;
    juce::Point<float> activeMousePos_;

    static constexpr int kFullWidth = 600;
    static constexpr int kFullHeight = 450;  // 4:3 比例
    static constexpr float kTopBarHeight = 28.0f;
    static constexpr float kBtnSize = 20.0f;
    static constexpr float kBtnGap = 3.0f;
    static constexpr float kCardWidth = 208.0f;
    static constexpr float kCardTypeRowH = 22.0f;
    static constexpr float kCardTypeBtnSize = 20.0f;
    static constexpr float kCardTypeBtnGap = 2.0f;
    static constexpr float kCardColumnWidth = 68.0f;
    static constexpr float kCardSliderHeight = 68.0f;
    static constexpr float kCardLabelHeight = 12.0f;
    static constexpr float kCardPadding = 4.0f;

    juce::Rectangle<float> topBarBounds() const;
    void commitSettings();
    bool removeFilterAt(int index);
    void updateHoverBandFade(double dt);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqPopupComponent)
};

} // namespace OpenTune

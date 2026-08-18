/**
 * EQ Popup Component — per-note EQ 编辑弹窗
 *
 * 动态滤波器列表：最多 kMaxFilters=10。
 * 浮动参数卡 + 双击创建/删除 + 滚轮调 Q + Minimize 行为随态变化。
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/NoteEqSettings.h"
#include "EqGraphRenderer.h"
#include "EqBandInteraction.h"
#include <memory>

namespace OpenTune {

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
    std::function<void(std::array<float, 128>&, std::array<float, 128>&)> onReadSpectrum;

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

    // 浮动参数卡
    juce::Rectangle<float> floatingCardBounds(juce::Point<float> anchorPos) const;
    void paintFloatingCard(juce::Graphics& g, int filterIndex, juce::Point<float> anchorPos) const;
    void updateCardState();

    void showValueInputPopup(int filterIndex);
    void dismissValueInputPopup();
    void layoutValueInputOverlay();
    void dismissRemoveConfirmation();

    void paintBypassIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintRemoveIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintMaximizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintMinimizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;

    EqSettings settings_;
    EqGraphRenderer renderer_;
    EqBandInteraction interaction_;
    std::array<float, 128> spectrum_{};
    std::array<float, 128> spectrumPeaks_{};
    juce::Colour noteColor_;
    bool isPreview_ = true;
    bool isMaximized_ = false;
    int hoveredBand_ = -1;
    int hoveredButton_ = -1;
    int hoveredViewRange_ = -1;
    int pressedViewRange_ = -1;
    int selectedFilterIndex_ = -1;
    int pendingValueInputBand_ = -1;
    juce::Rectangle<int> savedPreviewBounds_;

    // 浮动参数卡状态
    int cardBand_ = -1;
    juce::Point<float> cardAnchorPos_;

    std::vector<double> hoverBandAmounts_ = std::vector<double>(EqSettings::kMaxFilters, 0.0);
    static constexpr double kHoverFadeInTime = 0.50;
    static constexpr double kHoverFadeOutTime = 0.50;

    bool dragCommitted_ = false;
    bool wasDragging_ = false;
    bool draggingWindow_ = false;
    juce::Point<int> dragOffset_;
    int pendingDragBand_ = -1;
    juce::Point<float> pendingDragStartPos_;

    bool showingValueInput_ = false;
    int valueInputBand_ = -1;
    std::unique_ptr<juce::TextEditor> freqEditor_;
    std::unique_ptr<juce::TextEditor> gainEditor_;
    std::unique_ptr<juce::Label> qLabel_;
    std::unique_ptr<juce::TextButton> valueInputOk_;
    std::unique_ptr<juce::TextButton> valueInputCancel_;

    bool showingRemoveConfirmation_ = false;
    bool suppressRemoveConfirmation_ = false;
    juce::Point<float> activeMousePos_;

    static constexpr int kFullWidth = 600;
    static constexpr int kFullHeight = 400;
    static constexpr float kTopBarHeight = 28.0f;
    static constexpr float kBtnSize = 20.0f;
    static constexpr float kBtnGap = 3.0f;
    static constexpr float kCardWidth = 220.0f;
    static constexpr float kCardRowHeight = 20.0f;
    static constexpr float kCardButtonHeight = 18.0f;

    juce::Rectangle<float> topBarBounds() const;
    void commitSettings();
    bool removeFilterAt(int index);
    void updateHoverBandFade(double dt);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqPopupComponent)
};

} // namespace OpenTune

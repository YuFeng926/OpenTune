/**
 * EQ Popup Component — per-note EQ 编辑弹窗
 *
 * 两态契约：
 * - 预览：180x80，四控制全部可见，动态曲线+anchors交互，隐藏轴/网格/图例/视图范围
 * - 完整：600x400，完整 UI（滤波器hover淡入淡出）
 *
 * 动态滤波器列表：最多 kMaxFilters=10，不假设固定 5 段。
 * 支持双击创建/删除 anchor，滚轮调 Q（Peak），Minimize 行为随态变化。
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

    // ── 固定公共 API ──
    void setEqSettings(const EqSettings& settings);
    void setPreviewMode(bool isPreview);
    void setRemoveConfirmationSuppressed(bool suppress);
    void setNoteColor(juce::Colour color);

    // ── 回调 ──
    std::function<void(const EqSettings&)> onCommitSettings;
    std::function<void()> onRemoveEq;
    std::function<void()> onClose;
    std::function<void(bool)> onRemoveConfirmationSuppressed;
    std::function<void(std::array<float, 128>&, std::array<float, 128>&)> onReadSpectrum;

    // ── Component ──
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    // ── Timer ──
    void timerCallback() override;

    bool isPreviewMode() const { return isPreview_; }
    void toggleMaximize();

private:
    // ── 按钮 ──
    enum class ButtonId { None = -1, Maximize = 0, Bypass, Remove, Minimize };
    void paintButton(juce::Graphics& g, ButtonId id, juce::Rectangle<float> bounds, bool hovered) const;
    juce::Rectangle<float> buttonBounds(ButtonId id) const;
    ButtonId hitTestButton(juce::Point<float> pos) const;

    // ── 数值输入弹窗 ──
    void showValueInputPopup(int filterIndex);
    void dismissValueInputPopup();
    void layoutValueInputOverlay();

    // ── Remove 确认弹窗 ──
    void dismissRemoveConfirmation();

    // ── 按钮图标 ──
    void paintBypassIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintRemoveIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintMaximizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintMinimizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;

    // ── 状态 ──
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
    int selectedFilterIndex_ = -1;  // mouseDown 命中 anchor 时记录
    bool doubleClickHandled_ = false;  // mouseDown 多击已处理 anchor 删除
    juce::Rectangle<int> savedPreviewBounds_;

    // ── 滤波器 hover 淡入淡出动画 ──
    std::vector<double> hoverBandAmounts_ = std::vector<double>(EqSettings::kMaxFilters, 0.0);
    static constexpr double kHoverFadeInTime = 0.50;
    static constexpr double kHoverFadeOutTime = 0.50;

    // 拖拽状态
    bool dragCommitted_ = false;
    bool wasDragging_ = false;

    // 窗口拖拽状态
    bool draggingWindow_ = false;
    juce::Point<int> dragOffset_;

    // 微拖拽 pending 语义
    int pendingDragBand_ = -1;
    juce::Point<float> pendingDragStartPos_;

    // 数值输入弹窗
    bool showingValueInput_ = false;
    int valueInputBand_ = -1;
    int pendingValueInputBand_ = -1;
    std::unique_ptr<juce::TextEditor> freqEditor_;
    std::unique_ptr<juce::TextEditor> gainEditor_;
    std::unique_ptr<juce::Label> qLabel_;
    std::unique_ptr<juce::TextButton> valueInputOk_;
    std::unique_ptr<juce::TextButton> valueInputCancel_;

    // Remove 确认
    bool showingRemoveConfirmation_ = false;
    bool suppressRemoveConfirmation_ = false;

    // 活动中的 mousePos
    juce::Point<float> activeMousePos_;

    // 布局常量
    static constexpr int kFullWidth = 600;
    static constexpr int kFullHeight = 400;
    static constexpr float kTopBarHeight = 28.0f;
    static constexpr float kBtnSize = 20.0f;
    static constexpr float kBtnGap = 3.0f;

    juce::Rectangle<float> topBarBounds() const;

    void commitSettings();
    bool removeFilterAt(int index);  // 返回 true 表示 onRemoveEq 触发，组件可能已销毁
    void updateHoverBandFade(double dt);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqPopupComponent)
};

} // namespace OpenTune

/**
 * EQ Popup Component — per-note EQ 编辑弹窗
 *
 * 两态契约：
 * - 预览：180×80，四控制全部可见（Maximize/Bypass/Remove/Close），5曲线+5 anchors交互，隐藏轴/网格/坐标动画/图例/视图范围
 * - 完整：600×400，完整 UI
 *
 * 尺寸自管理（无 parent 回调）：
 * - 首次从预览进入完整时保存当前 preview bounds
 * - 完整尺寸目标600×400，按 parent local bounds 等比例/夹紧
 * - Minimize 恢复保存的 preview bounds
 * - setPreviewMode 外部初始调用保持父级设置的 bounds，不误保存/跳变
 *
 * 固定公共 API（契约 §6）：
 * - setEqSettings: 零回调
 * - setNoteColor: 设置背景色
 * - setPreviewMode: 切换预览/完整状态（外部初始调用不改变 bounds）
 * - setRemoveConfirmationSuppressed: 设置删除确认抑制
 * - onCommitSettings: anchor mouseUp 每次只触发一次
 * - onRemoveEq: Remove 确认后触发
 * - onClose: Close 触发
 * - onRemoveConfirmationSuppressed: "不再提示"勾选同步
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
    void setNoteColor(juce::Colour color);
    void setPreviewMode(bool isPreview);
    void setRemoveConfirmationSuppressed(bool suppress);

    // ── 回调（由 PianoRollComponent 设置） ──
    std::function<void(const EqSettings&)> onCommitSettings;
    std::function<void()> onRemoveEq;
    std::function<void()> onClose;
    std::function<void(bool)> onRemoveConfirmationSuppressed;

    // ── Component ──
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;

    // ── Timer ──
    void timerCallback() override;

    bool isPreviewMode() const { return isPreview_; }
    void toggleMaximize();

private:
    // ── 按钮（None = 未命中任何按钮，图区域不得切换模式） ──
    enum class ButtonId { None = -1, Maximize = 0, Bypass, Remove, Close };
    void paintButton(juce::Graphics& g, ButtonId id, juce::Rectangle<float> bounds, bool hovered) const;
    juce::Rectangle<float> buttonBounds(ButtonId id) const;
    ButtonId hitTestButton(juce::Point<float> pos) const;

    // ── 数值输入弹窗 ──
    void showValueInputPopup(int bandIndex);
    void dismissValueInputPopup();
    void layoutValueInputOverlay();

    // ── Remove 确认弹窗 ──
    void dismissRemoveConfirmation();

    // ── 绘制按钮图标（图形而非文字） ──
    void paintPowerSymbol(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintRemoveIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintMaximizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;
    void paintCloseIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const;

    // ── 状态 ──
    EqSettings settings_;
    EqGraphRenderer renderer_;
    EqBandInteraction interaction_;
    bool isPreview_ = true;
    bool isMaximized_ = false;
    juce::Colour noteColor_ = juce::Colour::fromRGB(100, 100, 100);
    int hoveredBand_ = -1;
    int hoveredButton_ = -1;
    int hoveredLegend_ = -1;
    int hoveredViewRange_ = -1;
    int pressedViewRange_ = -1;  // -1=无, 0=Decrease(+), 1=Increase(-); mouseDown 记录, mouseUp 提交
    juce::Rectangle<int> savedPreviewBounds_;  // 保存的预览 bounds，用于 Minimize 恢复

    // 拖拽状态
    bool dragCommitted_ = false;
    bool wasDragging_ = false;

    // 微拖拽 pending 语义（mouseDown 只记录，mouseDrag 超阈值才 startDrag）
    int pendingDragBand_ = -1;
    juce::Point<float> pendingDragStartPos_;

    // 数值输入弹窗（overlay 状态，单一管理）
    bool showingValueInput_ = false;
    int valueInputBand_ = -1;
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

    // ── 坐标反馈 SRC 节奏动画状态 ──
    enum class CoordAnimState { Idle, FadeIn, Active, FadeOut };
    CoordAnimState coordAnimState_ = CoordAnimState::Idle;
    double coordAnimElapsed_ = 0.0;   // 当前阶段累计时间（秒）
    float coordAnimOpacity_ = 0.0f;   // 0..1 渲染透明度
    bool mouseInGraph_ = false;       // 鼠标是否在图区域内

    static constexpr double kCoordFadeInTime  = 0.120;  // 120ms fade in
    static constexpr double kCoordActiveEntry = 0.140;   // 140ms entry
    static constexpr double kCoordActiveHold  = 2.360;   // 2360ms hold
    static constexpr double kCoordFadeOutTime = 0.500;    // 500ms fade out

    // 布局常量
    static constexpr int kPreviewWidth = 180;
    static constexpr int kPreviewHeight = 80;
    static constexpr int kFullWidth = 600;
    static constexpr int kFullHeight = 400;
    static constexpr float kTopBarHeight = 28.0f;
    static constexpr float kBtnSize = 20.0f;
    static constexpr float kBtnGap = 3.0f;

    // 按钮区域计算
    juce::Rectangle<float> topBarBounds() const;
    juce::Rectangle<float> graphAreaBounds() const;

    void commitSettings();
    void updateCoordAnimation(double dt);
    bool graphBoundsContains(juce::Point<float> pos) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqPopupComponent)
};

} // namespace OpenTune

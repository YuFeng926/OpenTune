/**
 * EQ Popup Component — per-note EQ 编辑弹窗（实现）
 */

#include "EqPopupComponent.h"
#include "../UIColors.h"
#include <cmath>

namespace OpenTune {

EqPopupComponent::EqPopupComponent()
{
    setRepaintsOnMouseActivity(true);
    startTimerHz(30);
    interaction_.setRenderer(&renderer_);
}

EqPopupComponent::~EqPopupComponent()
{
    stopTimer();
    dismissValueInputPopup();
    dismissRemoveConfirmation();
}

// ============================================================================
// 固定公共 API
// ============================================================================

void EqPopupComponent::setEqSettings(const EqSettings& settings)
{
    settings_ = settings;
    renderer_.setSettings(settings);
    dragCommitted_ = false;
    repaint();
}

void EqPopupComponent::setNoteColor(juce::Colour color)
{
    noteColor_ = color;
    repaint();
}

void EqPopupComponent::setPreviewMode(bool isPreview)
{
    isPreview_ = isPreview;
    isMaximized_ = !isPreview;
    // 预览模式设置50-20kHz频率范围，完整模式恢复默认
    if (isPreview)
        renderer_.setPreviewFreqRange(50.0, 20000.0);
    else
        renderer_.clearPreviewFreqRange();
    // 外部初始调用保持父级设置的 bounds，不误保存/跳变
    resized();
    repaint();
}

void EqPopupComponent::setRemoveConfirmationSuppressed(bool suppress)
{
    suppressRemoveConfirmation_ = suppress;
}

// ============================================================================
// 布局
// ============================================================================

juce::Rectangle<float> EqPopupComponent::topBarBounds() const
{
    return getLocalBounds().toFloat().removeFromTop(kTopBarHeight);
}

juce::Rectangle<float> EqPopupComponent::graphAreaBounds() const
{
    auto bounds = getLocalBounds().toFloat();
    if (!isPreview_)
        bounds.removeFromTop(kTopBarHeight);
    if (isPreview_)
        return bounds.reduced(2.0f);
    return bounds.reduced(4.0f, 3.0f);
}

void EqPopupComponent::resized()
{
    if (isPreview_)
    {
        renderer_.setGraphBounds(graphAreaBounds());
    }
    else
    {
        // SRC 同职责：为右侧与底部轴标签预留空间
        constexpr float leftMargin = 54.0f;   // SRC leftAxisWidthPx + leftAxisGraphGapPx
        constexpr float rightMargin = 44.0f;  // SRC rightAxisWidthPx
        constexpr float bottomMargin = 42.0f; // SRC graphBottomInsetPx
        constexpr float topMargin = 28.0f;    // SRC graphTopInsetPx
        auto area = graphAreaBounds();
        const float ix = area.getX() + leftMargin;
        const float iy = area.getY() + topMargin;
        const float iw = area.getWidth() - leftMargin - rightMargin;
        const float ih = area.getHeight() - topMargin - bottomMargin;
        renderer_.setGraphBounds({ ix, iy, iw, ih });
    }
    // 布局数值输入弹窗控件
    if (showingValueInput_)
        layoutValueInputOverlay();
}

// ============================================================================
// 绘制
// ============================================================================

void EqPopupComponent::paint(juce::Graphics& g)
{
    // 背景
    const auto bgBase = noteColor_.withSaturation(noteColor_.getSaturation() * 0.3f);
    const auto bgColor = bgBase.interpolatedWith(EqGraphRenderer::backgroundColor(), 0.7f);
    g.fillAll(bgColor);

    // ── 曲线 + anchors 绘制（两态都画，bypass 由 renderer 降透明度） ──
    if (isPreview_)
    {
        renderer_.drawPreview(g);
    }
    else
    {
        renderer_.drawFull(g, hoveredBand_, activeMousePos_, interaction_.isDragging(),
                           hoveredLegend_, hoveredViewRange_, pressedViewRange_);
    }

    // ── 四控制按钮：预览模式和完整模式都画 ──
    {
        const auto topBar = topBarBounds();
        const float btnY = (kTopBarHeight - kBtnSize) * 0.5f;

        // Maximize/Minimize
        paintButton(g, ButtonId::Maximize,
                    { topBar.getX() + 4.0f, btnY, kBtnSize, kBtnSize },
                    hoveredButton_ == static_cast<int>(ButtonId::Maximize));

        // Bypass
        paintButton(g, ButtonId::Bypass,
                    { topBar.getX() + 4.0f + kBtnSize + kBtnGap, btnY, kBtnSize, kBtnSize },
                    hoveredButton_ == static_cast<int>(ButtonId::Bypass));

        // Remove
        paintButton(g, ButtonId::Remove,
                    { topBar.getX() + 4.0f + (kBtnSize + kBtnGap) * 2, btnY, kBtnSize, kBtnSize },
                    hoveredButton_ == static_cast<int>(ButtonId::Remove));

        // Close
        paintButton(g, ButtonId::Close,
                    { topBar.getRight() - 4.0f - kBtnSize, btnY, kBtnSize, kBtnSize },
                    hoveredButton_ == static_cast<int>(ButtonId::Close));
    }

    // ── 坐标反馈（完整模式，SRC 节奏动画 opacity 驱动） ──
    if (!isPreview_ && !interaction_.isDragging()
        && graphBoundsContains(activeMousePos_) && coordAnimOpacity_ > 0.01f)
    {
        renderer_.drawCoordReadout(g, activeMousePos_, true, coordAnimOpacity_);
    }

    // ── 按钮 tooltip ──
    if (hoveredButton_ >= 0)
    {
        const auto id = static_cast<ButtonId>(hoveredButton_);
        juce::String tooltipText;
        switch (id)
        {
        case ButtonId::Maximize:
            tooltipText = isMaximized_ ? juce::String::fromUTF8(u8"收起为预览")
                                       : juce::String::fromUTF8(u8"展开EQ编辑器");
            break;
        case ButtonId::Bypass:
            tooltipText = settings_.active ? juce::String::fromUTF8(u8"旁通EQ")
                                           : juce::String::fromUTF8(u8"启用EQ");
            break;
        case ButtonId::Remove:
            tooltipText = juce::String::fromUTF8(u8"删除EQ处理");
            break;
        case ButtonId::Close:
            tooltipText = juce::String::fromUTF8(u8"关闭");
            break;
        default: break;
        }

        if (tooltipText.isNotEmpty())
        {
            const auto btnRect = buttonBounds(id);
            const float tooltipH = 16.0f;
            const float tooltipY = btnRect.getBottom() + 3.0f;
            // 确保 tooltip 不超出组件底部
            const float maxY = static_cast<float>(getHeight()) - tooltipH - 2.0f;
            const float finalY = juce::jmin(tooltipY, maxY);

            g.setColour(EqGraphRenderer::hudBgColor().withAlpha(0.92f));
            const auto font = juce::Font(juce::FontOptions(10.0f));
            const auto textW = font.getStringWidthFloat(tooltipText) + 8.0f;
            const float tooltipX = juce::jmax(2.0f, juce::jmin(btnRect.getX(),
                                                                static_cast<float>(getWidth()) - textW - 2.0f));
            g.fillRoundedRectangle(tooltipX, finalY, textW, tooltipH, 3.0f);
            g.setColour(EqGraphRenderer::hudTextColor());
            g.setFont(font);
            g.drawText(tooltipText, juce::Rectangle<float>(tooltipX, finalY, textW, tooltipH),
                       juce::Justification::centred, false);
        }
    }

    // ── Remove 确认弹窗 ──
    if (showingRemoveConfirmation_)
    {
        const auto area = getLocalBounds().toFloat();
        const float dialogW = 200.0f;
        const float dialogH = 70.0f;
        const float dialogX = (area.getWidth() - dialogW) * 0.5f;
        const float dialogY = (area.getHeight() - dialogH) * 0.5f;

        g.setColour(juce::Colour::fromRGBA(0, 0, 0, 160));
        g.fillRect(area);

        g.setColour(EqGraphRenderer::hudBgColor());
        g.fillRoundedRectangle(dialogX, dialogY, dialogW, dialogH, 4.0f);
        g.setColour(EqGraphRenderer::hudTextColor());
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::String::fromUTF8(u8"确认移除 EQ?"),
                   juce::Rectangle<float>(dialogX, dialogY + 6.0f, dialogW, 18.0f),
                   juce::Justification::centred, false);

        const auto checkRect = juce::Rectangle<float>(dialogX + 10.0f, dialogY + 28.0f, dialogW - 20.0f, 14.0f);
        g.setColour(suppressRemoveConfirmation_ ? EqGraphRenderer::combinedCurveColor()
                                                : EqGraphRenderer::axisLabelColor());
        g.drawRect(checkRect.getX(), checkRect.getY() + 2.0f, 10.0f, 10.0f, 1.0f);
        if (suppressRemoveConfirmation_)
        {
            g.setColour(EqGraphRenderer::combinedCurveColor());
            g.drawLine(checkRect.getX() + 2.0f, checkRect.getY() + 7.0f,
                       checkRect.getX() + 8.0f, checkRect.getY() + 12.0f, 1.5f);
            g.drawLine(checkRect.getX() + 8.0f, checkRect.getY() + 12.0f,
                       checkRect.getX() + 14.0f, checkRect.getY() + 4.0f, 1.5f);
        }
        g.setColour(EqGraphRenderer::axisLabelColor());
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(juce::String::fromUTF8(u8"不再提示"), checkRect.translated(16.0f, 0.0f),
                   juce::Justification::centredLeft, false);

        const auto okRect = juce::Rectangle<float>(dialogX + 10.0f, dialogY + dialogH - 24.0f, 80.0f, 18.0f);
        g.setColour(EqGraphRenderer::combinedCurveColor().withAlpha(0.6f));
        g.fillRoundedRectangle(okRect, 3.0f);
        g.setColour(juce::Colours::black);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(juce::String::fromUTF8(u8"确认"), okRect, juce::Justification::centred, false);

        const auto cancelRect = juce::Rectangle<float>(dialogX + dialogW - 90.0f, dialogY + dialogH - 24.0f, 80.0f, 18.0f);
        g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.4f));
        g.fillRoundedRectangle(cancelRect, 3.0f);
        g.setColour(EqGraphRenderer::hudTextColor());
        g.drawText(juce::String::fromUTF8(u8"取消"), cancelRect, juce::Justification::centred, false);
    }

    // ── 数值输入弹窗遮罩 ──
    if (showingValueInput_)
    {
        // 全区域半透明遮罩
        g.setColour(juce::Colour::fromRGBA(0, 0, 0, 120));
        g.fillRect(getLocalBounds().toFloat());
    }
}

// ============================================================================
// 按钮绘制 — 图形化图标
// ============================================================================

void EqPopupComponent::paintButton(juce::Graphics& g, ButtonId id, juce::Rectangle<float> bounds, bool hovered) const
{
    auto color = EqGraphRenderer::axisLabelColor().withAlpha(hovered ? 0.8f : 0.4f);
    if (id == ButtonId::Close && hovered)
        color = juce::Colour::fromRGB(220, 60, 60);

    g.setColour(color);
    g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

    const auto iconBounds = bounds.reduced(4.0f);
    switch (id)
    {
    case ButtonId::Maximize: paintMaximizeIcon(g, iconBounds, color); break;
    case ButtonId::Bypass:   paintPowerSymbol(g, iconBounds, color); break;
    case ButtonId::Remove:   paintRemoveIcon(g, iconBounds, color); break;
    case ButtonId::Close:    paintCloseIcon(g, iconBounds, color); break;
    case ButtonId::None: break;
    }
}

void EqPopupComponent::paintPowerSymbol(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    // 打勾的复选框：外框 + 对勾
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float s = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.40f;

    // 复选框外框
    g.drawRect(cx - s, cy - s, s * 2.0f, s * 2.0f, 1.5f);

    // 对勾（✓）
    if (settings_.active)
    {
        g.drawLine(cx - s * 0.5f, cy, cx - s * 0.1f, cy + s * 0.4f, 1.8f);
        g.drawLine(cx - s * 0.1f, cy + s * 0.4f, cx + s * 0.5f, cy - s * 0.4f, 1.8f);
    }
}

void EqPopupComponent::paintRemoveIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    // 垃圾桶图标：盖子 + 桶身 + 竖线
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float w = bounds.getWidth() * 0.55f;
    const float h = bounds.getHeight() * 0.60f;
    const float lidH = h * 0.25f;
    const float bodyTop = cy - h * 0.5f + lidH + 1.0f;
    const float bodyBot = cy + h * 0.5f;

    // 桶盖
    g.drawLine(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy - h * 0.5f, 1.5f);
    // 盖子提手
    g.drawLine(cx - w * 0.2f, cy - h * 0.5f - lidH, cx + w * 0.2f, cy - h * 0.5f - lidH, 1.5f);
    g.drawLine(cx - w * 0.2f, cy - h * 0.5f, cx - w * 0.2f, cy - h * 0.5f - lidH, 1.5f);
    g.drawLine(cx + w * 0.2f, cy - h * 0.5f, cx + w * 0.2f, cy - h * 0.5f - lidH, 1.5f);
    // 桶身
    g.drawLine(cx - w * 0.4f, bodyTop, cx - w * 0.35f, bodyBot, 1.3f);
    g.drawLine(cx + w * 0.4f, bodyTop, cx + w * 0.35f, bodyBot, 1.3f);
    g.drawLine(cx - w * 0.4f, bodyTop, cx + w * 0.4f, bodyTop, 1.3f);
    g.drawLine(cx - w * 0.35f, bodyBot, cx + w * 0.35f, bodyBot, 1.3f);
    // 桶身竖线
    g.drawLine(cx - w * 0.15f, bodyTop + 1.0f, cx - w * 0.15f, bodyBot - 1.0f, 1.0f);
    g.drawLine(cx + w * 0.15f, bodyTop + 1.0f, cx + w * 0.15f, bodyBot - 1.0f, 1.0f);
}

void EqPopupComponent::paintMaximizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    // 最大化：向上双箭头；最小化：向下双箭头
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float s = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.28f;

    if (isMaximized_)
    {
        // 向下双箭头（还原）
        g.drawLine(cx, cy - s, cx - s, cy, 1.3f);
        g.drawLine(cx, cy - s, cx + s, cy, 1.3f);
        g.drawLine(cx, cy + 1.0f, cx - s, cy + s + 1.0f, 1.3f);
        g.drawLine(cx, cy + 1.0f, cx + s, cy + s + 1.0f, 1.3f);
    }
    else
    {
        // 向上双箭头（最大化）
        g.drawLine(cx, cy + s, cx - s, cy, 1.3f);
        g.drawLine(cx, cy + s, cx + s, cy, 1.3f);
        g.drawLine(cx, cy - 1.0f, cx - s, cy - s - 1.0f, 1.3f);
        g.drawLine(cx, cy - 1.0f, cx + s, cy - s - 1.0f, 1.3f);
    }
}

void EqPopupComponent::paintCloseIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    // × 关闭符号
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float s = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.30f;
    g.drawLine(cx - s, cy - s, cx + s, cy + s, 1.5f);
    g.drawLine(cx + s, cy - s, cx - s, cy + s, 1.5f);
}

juce::Rectangle<float> EqPopupComponent::buttonBounds(ButtonId id) const
{
    const auto topBar = topBarBounds();
    const float btnY = (kTopBarHeight - kBtnSize) * 0.5f;
    switch (id)
    {
    case ButtonId::Maximize: return { topBar.getX() + 4.0f, btnY, kBtnSize, kBtnSize };
    case ButtonId::Bypass:   return { topBar.getX() + 4.0f + kBtnSize + kBtnGap, btnY, kBtnSize, kBtnSize };
    case ButtonId::Remove:   return { topBar.getX() + 4.0f + (kBtnSize + kBtnGap) * 2, btnY, kBtnSize, kBtnSize };
    case ButtonId::Close:    return { topBar.getRight() - 4.0f - kBtnSize, btnY, kBtnSize, kBtnSize };
    case ButtonId::None:     return {};
    }
    return {};
}

EqPopupComponent::ButtonId EqPopupComponent::hitTestButton(juce::Point<float> pos) const
{
    // 两态都检查四个按钮
    for (auto id : { ButtonId::Maximize, ButtonId::Bypass, ButtonId::Remove, ButtonId::Close })
        if (buttonBounds(id).contains(pos))
            return id;
    // 图区域不切换模式：明确返回 None
    return ButtonId::None;
}

// ============================================================================
// 鼠标交互
// ============================================================================

void EqPopupComponent::mouseMove(const juce::MouseEvent& event)
{
    activeMousePos_ = event.position;
    const auto pos = event.position;

    // 按钮悬停（两态都检查四个按钮）
    hoveredButton_ = -1;
    for (int i = 0; i < 4; ++i)
    {
        const auto id = static_cast<ButtonId>(i);
        if (buttonBounds(id).contains(pos))
        {
            hoveredButton_ = i;
            break;
        }
    }

    // 锚点悬停
    hoveredBand_ = renderer_.hitTestAnchor(pos, 10.0f);

    // 视图范围按钮悬停 — 0=Decrease(+), 1=Increase(-), -1=无
    if (!isPreview_)
    {
        const auto vr = renderer_.hitTestViewRangeButton(pos);
        if (vr == EqGraphRenderer::ViewRangeButton::Decrease)
            hoveredViewRange_ = 0;
        else if (vr == EqGraphRenderer::ViewRangeButton::Increase)
            hoveredViewRange_ = 1;
        else
            hoveredViewRange_ = -1;
    }
    else
    {
        hoveredViewRange_ = -1;
    }

    // 图例悬停
    hoveredLegend_ = -1;
    if (!isPreview_)
    {
        const auto legendItems = renderer_.buildLegendItems();
        hoveredLegend_ = renderer_.hitTestLegend(pos, legendItems);
    }

    // 坐标动画：跟踪鼠标是否在图区域内
    mouseInGraph_ = renderer_.graphBounds().contains(pos);

    setMouseCursor(hoveredBand_ >= 0 ? juce::MouseCursor::PointingHandCursor
                   : (hoveredButton_ >= 0 ? juce::MouseCursor::PointingHandCursor
                                          : juce::MouseCursor::NormalCursor));

    repaint();
}

void EqPopupComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
        return;

    const auto pos = event.position;

    // Remove 确认弹窗交互
    if (showingRemoveConfirmation_)
    {
        const auto area = getLocalBounds().toFloat();
        const float dialogW = 200.0f;
        const float dialogH = 70.0f;
        const float dialogX = (area.getWidth() - dialogW) * 0.5f;
        const float dialogY = (area.getHeight() - dialogH) * 0.5f;

        const auto checkRect = juce::Rectangle<float>(dialogX + 10.0f, dialogY + 28.0f, dialogW - 20.0f, 14.0f);
        if (checkRect.contains(pos))
        {
            suppressRemoveConfirmation_ = !suppressRemoveConfirmation_;
            if (onRemoveConfirmationSuppressed)
                onRemoveConfirmationSuppressed(suppressRemoveConfirmation_);
            repaint();
            return;
        }

        const auto okRect = juce::Rectangle<float>(dialogX + 10.0f, dialogY + dialogH - 24.0f, 80.0f, 18.0f);
        if (okRect.contains(pos))
        {
            dismissRemoveConfirmation();
            if (onRemoveEq)
                onRemoveEq();
            return;
        }

        const auto cancelRect = juce::Rectangle<float>(dialogX + dialogW - 90.0f, dialogY + dialogH - 24.0f, 80.0f, 18.0f);
        if (cancelRect.contains(pos))
        {
            dismissRemoveConfirmation();
            return;
        }

        return;
    }

    // 数值输入弹窗：遮罩拦截
    if (showingValueInput_)
        return;

    // 按钮点击（两态统一：None 则不处理，自然落入图例/视图范围/anchor 路径）
    {
        const auto btn = hitTestButton(pos);
        if (btn != ButtonId::None)
        {
            switch (btn)
            {
            case ButtonId::Maximize:
                toggleMaximize();
                return;
            case ButtonId::Bypass:
                settings_.active = !settings_.active;
                renderer_.setSettings(settings_);
                commitSettings();
                repaint();
                return;
            case ButtonId::Remove:
                if (suppressRemoveConfirmation_)
                {
                    if (onRemoveEq) onRemoveEq();
                }
                else
                {
                    showingRemoveConfirmation_ = true;
                    repaint();
                }
                return;
            case ButtonId::Close:
                if (onClose) onClose();
                return;
            }
        }
    }

    // 图例点击
    if (!isPreview_)
    {
        const auto legendItems = renderer_.buildLegendItems();
        const int legendIdx = renderer_.hitTestLegend(pos, legendItems);
        if (legendIdx >= 0 && legendIdx < 6)
        {
            const auto cid = legendItems[static_cast<size_t>(legendIdx)].id;
            renderer_.setCurveVisible(cid, !renderer_.isCurveVisible(cid));
            repaint();
            return;
        }

        // 视图范围按钮 — SRC press-release：mouseDown 只记录 pressed，不改范围
        const auto vr = renderer_.hitTestViewRangeButton(pos);
        if (vr != EqGraphRenderer::ViewRangeButton::None)
        {
            if (vr == EqGraphRenderer::ViewRangeButton::Decrease)
                pressedViewRange_ = 0;
            else
                pressedViewRange_ = 1;
            repaint();
            return;
        }
    }

    // 锚点交互 — 微拖拽 pending 语义：mouseDown 只记录 pending
    {
        const int bandIdx = renderer_.hitTestAnchor(pos, 10.0f);
        if (bandIdx >= 0)
        {
            pendingDragBand_ = bandIdx;
            pendingDragStartPos_ = pos;
            hoveredBand_ = bandIdx;
            setMouseCursor(juce::MouseCursor::NoCursor);
            repaint();
            return;
        }
    }
}

void EqPopupComponent::mouseDrag(const juce::MouseEvent& event)
{
    activeMousePos_ = event.position;

    // 未启动 drag 时：检查 pending band 是否超过阈值
    if (!interaction_.isDragging())
    {
        if (pendingDragBand_ < 0)
            return;
        if (!interaction_.hasDragThreshold(pendingDragStartPos_, event.position))
            return;
        // 超过阈值：启动 drag
        interaction_.startDrag(pendingDragBand_, pendingDragStartPos_, settings_);
        dragCommitted_ = false;
        wasDragging_ = true;
    }

    settings_ = interaction_.updateDrag(event.position, settings_);
    renderer_.setSettings(settings_);
    repaint();
}

void EqPopupComponent::mouseUp(const juce::MouseEvent& event)
{
    if (interaction_.isDragging())
    {
        // 启动了 drag：提交一次
        interaction_.endDrag();
        pendingDragBand_ = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);

        if (wasDragging_ && !dragCommitted_)
        {
            commitSettings();
            dragCommitted_ = true;
        }

        wasDragging_ = false;
        repaint();
        return;
    }

    // 未启动 drag：用记录的 band 打开数值输入
    if (pendingDragBand_ >= 0)
    {
        const int band = pendingDragBand_;
        pendingDragBand_ = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        showValueInputPopup(band);
        repaint();
    }

    // 视图范围按钮 — SRC press-release：mouseUp 释放仍命中同一按钮时执行切换
    if (pressedViewRange_ >= 0)
    {
        const auto vr = renderer_.hitTestViewRangeButton(event.position);
        const bool sameButton = (vr == EqGraphRenderer::ViewRangeButton::Decrease && pressedViewRange_ == 0)
                             || (vr == EqGraphRenderer::ViewRangeButton::Increase && pressedViewRange_ == 1);
        if (sameButton)
        {
            const double currentRange = renderer_.viewGainRangeDb();
            double newRange = currentRange;
            if (pressedViewRange_ == 0)  // Decrease
                newRange = currentRange >= 30.0 ? 12.0 : (currentRange >= 12.0 ? 6.0 : 6.0);
            else  // Increase
                newRange = currentRange <= 6.0 ? 12.0 : (currentRange <= 12.0 ? 30.0 : 30.0);
            if (newRange != currentRange)
                renderer_.setViewGainRangeDb(newRange);
        }
        pressedViewRange_ = -1;
        repaint();
    }
}

void EqPopupComponent::mouseExit(const juce::MouseEvent&)
{
    // 只清 hover + pressed，不破坏 pending band
    hoveredBand_ = -1;
    hoveredButton_ = -1;
    hoveredLegend_ = -1;
    hoveredViewRange_ = -1;
    pressedViewRange_ = -1;
    mouseInGraph_ = false;
    repaint();
}

// ============================================================================
// Timer — 驱动坐标反馈 SRC 节奏动画
// ============================================================================

void EqPopupComponent::timerCallback()
{
    const double dt = 1.0 / 30.0;  // 30Hz timer

    // 坐标动画
    updateCoordAnimation(dt);

    // 拖拽时刷新
    if (interaction_.isDragging())
        repaint();

    // 坐标动画活跃时也刷新
    if (coordAnimState_ != CoordAnimState::Idle)
        repaint();
}

void EqPopupComponent::updateCoordAnimation(double dt)
{
    if (isPreview_ || interaction_.isDragging())
    {
        coordAnimState_ = CoordAnimState::Idle;
        coordAnimOpacity_ = 0.0f;
        coordAnimElapsed_ = 0.0;
        return;
    }

    if (mouseInGraph_ && renderer_.graphBounds().contains(activeMousePos_))
    {
        switch (coordAnimState_)
        {
        case CoordAnimState::Idle:
            coordAnimState_ = CoordAnimState::FadeIn;
            coordAnimElapsed_ = 0.0;
            break;
        case CoordAnimState::FadeIn:
            coordAnimElapsed_ += dt;
            coordAnimOpacity_ = static_cast<float>(coordAnimElapsed_ / kCoordFadeInTime);
            if (coordAnimElapsed_ >= kCoordFadeInTime)
            {
                coordAnimState_ = CoordAnimState::Active;
                coordAnimElapsed_ = 0.0;
                coordAnimOpacity_ = 1.0f;
            }
            break;
        case CoordAnimState::Active:
            coordAnimElapsed_ += dt;
            coordAnimOpacity_ = 1.0f;
            if (coordAnimElapsed_ >= kCoordActiveEntry + kCoordActiveHold)
            {
                coordAnimState_ = CoordAnimState::FadeOut;
                coordAnimElapsed_ = 0.0;
            }
            break;
        case CoordAnimState::FadeOut:
            coordAnimElapsed_ += dt;
            coordAnimOpacity_ = 1.0f - static_cast<float>(coordAnimElapsed_ / kCoordFadeOutTime);
            if (coordAnimElapsed_ >= kCoordFadeOutTime)
            {
                coordAnimState_ = CoordAnimState::Idle;
                coordAnimOpacity_ = 0.0f;
                coordAnimElapsed_ = 0.0;
            }
            break;
        }
    }
    else
    {
        // 鼠标离开图区域：启动 fade out
        if (coordAnimState_ != CoordAnimState::Idle && coordAnimState_ != CoordAnimState::FadeOut)
        {
            coordAnimState_ = CoordAnimState::FadeOut;
            coordAnimElapsed_ = 0.0;
        }
        else if (coordAnimState_ == CoordAnimState::FadeOut)
        {
            coordAnimElapsed_ += dt;
            coordAnimOpacity_ = 1.0f - static_cast<float>(coordAnimElapsed_ / kCoordFadeOutTime);
            if (coordAnimElapsed_ >= kCoordFadeOutTime)
            {
                coordAnimState_ = CoordAnimState::Idle;
                coordAnimOpacity_ = 0.0f;
                coordAnimElapsed_ = 0.0;
            }
        }
        else
        {
            coordAnimState_ = CoordAnimState::Idle;
            coordAnimOpacity_ = 0.0f;
            coordAnimElapsed_ = 0.0;
        }
    }
}

// ============================================================================
// 状态操作
// ============================================================================

void EqPopupComponent::toggleMaximize()
{
    if (isMaximized_)
    {
        // 完整→预览：恢复保存的 preview bounds，设置预览频率范围
        isMaximized_ = false;
        isPreview_ = true;
        renderer_.setPreviewFreqRange(50.0, 20000.0);
        if (!savedPreviewBounds_.isEmpty())
            setBounds(savedPreviewBounds_);
        resized();
        repaint();
    }
    else
    {
        // 预览→完整：保存当前 preview bounds，计算 full bounds，恢复完整频率范围
        isMaximized_ = true;
        isPreview_ = false;
        renderer_.clearPreviewFreqRange();
        savedPreviewBounds_ = getBounds();  // 保存当前预览 bounds

        // 按 parent local bounds 等比例/夹紧
        const int targetW = kFullWidth;
        const int targetH = kFullHeight;
        int w = targetW;
        int h = targetH;

        if (auto* parent = getParentComponent())
        {
            const auto parentBounds = parent->getLocalBounds();
            // 等比例缩放以适配父级
            if (w > parentBounds.getWidth() || h > parentBounds.getHeight())
            {
                const double scaleX = static_cast<double>(parentBounds.getWidth()) / w;
                const double scaleY = static_cast<double>(parentBounds.getHeight()) / h;
                const double scale = juce::jmin(scaleX, scaleY);
                w = static_cast<int>(w * scale);
                h = static_cast<int>(h * scale);
            }
            // 在父级内定位：保持锚点位置附近
            const int x = juce::jlimit(parentBounds.getX(),
                                       juce::jmax(parentBounds.getX(), parentBounds.getRight() - w),
                                       getX());
            const int y = juce::jlimit(parentBounds.getY(),
                                       juce::jmax(parentBounds.getY(), parentBounds.getBottom() - h),
                                       getY());
            setBounds(x, y, w, h);
        }
        else
        {
            setBounds(getX(), getY(), w, h);
        }
        resized();
        repaint();
    }
}

void EqPopupComponent::commitSettings()
{
    if (onCommitSettings)
        onCommitSettings(settings_);
}

// ============================================================================
// 数值输入弹窗 — unique_ptr + 完整布局
// ============================================================================

void EqPopupComponent::showValueInputPopup(int bandIndex)
{
    showingValueInput_ = true;
    valueInputBand_ = bandIndex;

    double freq = 0.0;
    double gain = 0.0;
    switch (bandIndex)
    {
    case 0: freq = settings_.lowCutFrequencyHz; break;
    case 1: freq = settings_.lowShelfFrequencyHz; gain = settings_.lowShelfGainDb; break;
    case 2: freq = settings_.peakFrequencyHz; gain = settings_.peakGainDb; break;
    case 3: freq = settings_.highShelfFrequencyHz; gain = settings_.highShelfGainDb; break;
    case 4: freq = settings_.highCutFrequencyHz; break;
    }

    const auto type = EqBandInteraction::bandType(bandIndex);
    const bool hasGain = (type == EqBandInteraction::BandType::LowShelf
                       || type == EqBandInteraction::BandType::Peak
                       || type == EqBandInteraction::BandType::HighShelf);

    // 创建控件（unique_ptr）
    freqEditor_ = std::make_unique<juce::TextEditor>();
    freqEditor_->setText(juce::String(freq, 0));
    freqEditor_->setFont(juce::Font(juce::FontOptions(11.0f)));
    freqEditor_->setColour(juce::TextEditor::backgroundColourId, EqGraphRenderer::hudBgColor());
    freqEditor_->setColour(juce::TextEditor::textColourId, EqGraphRenderer::hudTextColor());
    freqEditor_->setColour(juce::TextEditor::outlineColourId, EqGraphRenderer::axisLabelColor());
    addAndMakeVisible(freqEditor_.get());

    if (hasGain)
    {
        gainEditor_ = std::make_unique<juce::TextEditor>();
        gainEditor_->setText(juce::String(gain, 1));
        gainEditor_->setFont(juce::Font(juce::FontOptions(11.0f)));
        gainEditor_->setColour(juce::TextEditor::backgroundColourId, EqGraphRenderer::hudBgColor());
        gainEditor_->setColour(juce::TextEditor::textColourId, EqGraphRenderer::hudTextColor());
        gainEditor_->setColour(juce::TextEditor::outlineColourId, EqGraphRenderer::axisLabelColor());
        addAndMakeVisible(gainEditor_.get());

        qLabel_ = std::make_unique<juce::Label>(juce::String(), "Q: 2.0");
        qLabel_->setFont(juce::Font(juce::FontOptions(10.0f)));
        qLabel_->setColour(juce::Label::textColourId, EqGraphRenderer::axisLabelColor());
        addAndMakeVisible(qLabel_.get());
    }

    valueInputOk_ = std::make_unique<juce::TextButton>("OK");
    valueInputOk_->setColour(juce::TextButton::buttonColourId, EqGraphRenderer::combinedCurveColor());
    valueInputOk_->setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    valueInputOk_->onClick = [this] {
        if (freqEditor_ && freqEditor_->getText().isNotEmpty())
        {
            const double newFreq = freqEditor_->getText().getDoubleValue();
            switch (valueInputBand_)
            {
            case 0: settings_.lowCutFrequencyHz = std::clamp(static_cast<float>(newFreq), 20.0f, 20000.0f); break;
            case 1: settings_.lowShelfFrequencyHz = std::clamp(static_cast<float>(newFreq), 20.0f, 20000.0f); break;
            case 2: settings_.peakFrequencyHz = std::clamp(static_cast<float>(newFreq), 500.0f, 12000.0f); break;
            case 3: settings_.highShelfFrequencyHz = std::clamp(static_cast<float>(newFreq), 20.0f, 20000.0f); break;
            case 4: settings_.highCutFrequencyHz = std::clamp(static_cast<float>(newFreq), 20.0f, 20000.0f); break;
            }
        }
        if (gainEditor_ && gainEditor_->getText().isNotEmpty())
        {
            const double newGain = gainEditor_->getText().getDoubleValue();
            switch (valueInputBand_)
            {
            case 1: settings_.lowShelfGainDb = std::clamp(static_cast<float>(newGain), -12.0f, 12.0f); break;
            case 2: settings_.peakGainDb = std::clamp(static_cast<float>(newGain), -12.0f, 12.0f); break;
            case 3: settings_.highShelfGainDb = std::clamp(static_cast<float>(newGain), -12.0f, 12.0f); break;
            }
        }
        renderer_.setSettings(settings_);
        commitSettings();
        dismissValueInputPopup();
        repaint();
    };
    addAndMakeVisible(valueInputOk_.get());

    valueInputCancel_ = std::make_unique<juce::TextButton>(juce::String::fromUTF8(u8"取消"));
    valueInputCancel_->setColour(juce::TextButton::buttonColourId, EqGraphRenderer::axisLabelColor().withAlpha(0.4f));
    valueInputCancel_->setColour(juce::TextButton::textColourOffId, EqGraphRenderer::hudTextColor());
    valueInputCancel_->onClick = [this] { dismissValueInputPopup(); repaint(); };
    addAndMakeVisible(valueInputCancel_.get());

    layoutValueInputOverlay();
    repaint();
}

void EqPopupComponent::dismissValueInputPopup()
{
    showingValueInput_ = false;
    valueInputBand_ = -1;
    freqEditor_.reset();
    gainEditor_.reset();
    qLabel_.reset();
    valueInputOk_.reset();
    valueInputCancel_.reset();
}

void EqPopupComponent::layoutValueInputOverlay()
{
    const auto area = getLocalBounds().toFloat();
    const float panelW = 220.0f;
    const float panelH = 80.0f;
    const float panelX = (area.getWidth() - panelW) * 0.5f;
    const float panelY = (area.getHeight() - panelH) * 0.5f;

    const float rowH = 20.0f;
    const float fieldW = 80.0f;
    const float gap = 6.0f;

    float y = panelY + 6.0f;
    float x = panelX + 8.0f;

    if (freqEditor_)
        freqEditor_->setBounds(static_cast<int>(x), static_cast<int>(y),
                               static_cast<int>(fieldW), static_cast<int>(rowH));

    const auto type = EqBandInteraction::bandType(valueInputBand_);
    const bool hasGain = (type == EqBandInteraction::BandType::LowShelf
                       || type == EqBandInteraction::BandType::Peak
                       || type == EqBandInteraction::BandType::HighShelf);

    if (hasGain)
    {
        x += fieldW + gap;
        if (gainEditor_)
            gainEditor_->setBounds(static_cast<int>(x), static_cast<int>(y),
                                   static_cast<int>(fieldW), static_cast<int>(rowH));
        x += fieldW + gap;
        if (qLabel_)
            qLabel_->setBounds(static_cast<int>(x), static_cast<int>(y),
                               static_cast<int>(60.0f), static_cast<int>(rowH));
    }

    y += rowH + gap;
    const float btnW = 60.0f;
    x = panelX + (panelW - btnW * 2 - gap) * 0.5f;

    if (valueInputOk_)
        valueInputOk_->setBounds(static_cast<int>(x), static_cast<int>(y),
                                 static_cast<int>(btnW), static_cast<int>(rowH));
    x += btnW + gap;
    if (valueInputCancel_)
        valueInputCancel_->setBounds(static_cast<int>(x), static_cast<int>(y),
                                     static_cast<int>(btnW), static_cast<int>(rowH));
}

// ============================================================================
// Remove 确认弹窗
// ============================================================================

void EqPopupComponent::dismissRemoveConfirmation()
{
    showingRemoveConfirmation_ = false;
    repaint();
}

// ============================================================================
// 辅助
// ============================================================================

bool EqPopupComponent::graphBoundsContains(juce::Point<float> pos) const
{
    return renderer_.graphBounds().contains(pos);
}

} // namespace OpenTune

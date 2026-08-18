/**
 * EQ Popup Component — per-note EQ 编辑弹窗（实现）
 *
 * 动态滤波器列表，最多 kMaxFilters=10。
 * 双击创建/删除 anchor，滚轮调 Q（Peak），Minimize 行为随态变化。
 */

#include "EqPopupComponent.h"
#include "../UIColors.h"
#include <cmath>

namespace OpenTune {

EqPopupComponent::EqPopupComponent()
{
    setRepaintsOnMouseActivity(true);
    setOpaque(false);
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
// 浮动参数卡
// ============================================================================

juce::Rectangle<float> EqPopupComponent::floatingCardBounds(juce::Point<float> anchorPos) const
{
    float cardH = kCardRowHeight * 3.0f + kCardButtonHeight + 14.0f;
    if (showingValueInput_)
        cardH += kCardButtonHeight + 6.0f; // OK/Cancel row + gap
    float x = anchorPos.x + 14.0f;
    float y = anchorPos.y - cardH * 0.5f;
    if (x + kCardWidth > static_cast<float>(getWidth()) - 2.0f)
        x = anchorPos.x - kCardWidth - 14.0f;
    const float minY = kTopBarHeight + 2.0f;
    const float maxY = juce::jmax(minY, static_cast<float>(getHeight()) - cardH - 2.0f);
    y = juce::jlimit(minY, maxY, y);
    x = juce::jmax(2.0f, x);
    return { x, y, kCardWidth, cardH };
}

void EqPopupComponent::paintFloatingCard(juce::Graphics& g, int filterIndex,
                                          juce::Point<float> anchorPos) const
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return;

    const auto& f = settings_.filters[filterIndex];
    const auto cardBounds = floatingCardBounds(anchorPos);
    const auto color = renderer_.bandColor(filterIndex);

    // 卡片背景
    g.setColour(EqGraphRenderer::hudBgColor().withAlpha(0.93f));
    g.fillRoundedRectangle(cardBounds, 6.0f);
    g.setColour(color.withAlpha(0.45f));
    g.drawRoundedRectangle(cardBounds, 6.0f, 1.0f);

    // 指向锚点的三角形连线
    {
        juce::Path arrow;
        const float tipX = anchorPos.x;
        const float tipY = anchorPos.y;
        const bool fromLeft = tipX < cardBounds.getX();
        const float baseX = fromLeft ? cardBounds.getX() : cardBounds.getRight();
        const float baseY = juce::jlimit(cardBounds.getY() + 8.0f, cardBounds.getBottom() - 8.0f, tipY);
        arrow.startNewSubPath(tipX, tipY);
        arrow.lineTo(baseX, baseY - 5.0f);
        arrow.lineTo(baseX, baseY + 5.0f);
        arrow.closeSubPath();
        g.setColour(EqGraphRenderer::hudBgColor().withAlpha(0.93f));
        g.fillPath(arrow);
        g.setColour(color.withAlpha(0.45f));
        g.strokePath(arrow, juce::PathStrokeType(1.0f));
    }

    const float cx = cardBounds.getX() + 6.0f;
    float cy = cardBounds.getY() + 5.0f;

    // 滤波器选择按钮行
    g.setFont(juce::FontOptions(9.0f));
    const int n = static_cast<int>(settings_.filters.size());
    float bx = cx;
    for (int i = 0; i < n; ++i)
    {
        const float bw = 18.0f;
        const juce::Rectangle<float> btnRect(bx, cy, bw, kCardButtonHeight);
        const bool isActive = (i == filterIndex);
        const auto btnColor = renderer_.bandColor(i);
        g.setColour(isActive ? btnColor.withAlpha(0.75f) : btnColor.withAlpha(0.25f));
        g.fillRoundedRectangle(btnRect, 3.0f);
        g.setColour(isActive ? juce::Colours::white.withAlpha(0.9f)
                             : juce::Colours::white.withAlpha(0.4f));
        g.drawText(juce::String(i + 1), btnRect, juce::Justification::centred, false);
        bx += bw + 2.0f;
    }
    cy += kCardButtonHeight + 4.0f;

    // Frequency
    g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.6f));
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("Freq", juce::Rectangle<float>(cx, cy, 32.0f, kCardRowHeight),
               juce::Justification::centredLeft, false);
    g.setColour(EqGraphRenderer::hudTextColor());
    g.setFont(juce::FontOptions(10.0f));
    juce::String freqStr;
    if (f.frequencyHz >= 1000.0f)
        freqStr = juce::String(f.frequencyHz / 1000.0f, 1) + " kHz";
    else
        freqStr = juce::String(static_cast<int>(f.frequencyHz)) + " Hz";
    g.drawText(freqStr, juce::Rectangle<float>(cx + 34.0f, cy, kCardWidth - 40.0f, kCardRowHeight),
               juce::Justification::centredLeft, false);
    cy += kCardRowHeight;

    // Gain
    const bool hasGain = (f.type == EqFilterType::LowShelf
                       || f.type == EqFilterType::Peak
                       || f.type == EqFilterType::HighShelf);
    if (hasGain)
    {
        g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.6f));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("Gain", juce::Rectangle<float>(cx, cy, 32.0f, kCardRowHeight),
                   juce::Justification::centredLeft, false);
        g.setColour(EqGraphRenderer::hudTextColor());
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(juce::String(f.gainDb, 1) + " dB",
                   juce::Rectangle<float>(cx + 34.0f, cy, kCardWidth - 40.0f, kCardRowHeight),
                   juce::Justification::centredLeft, false);
        cy += kCardRowHeight;
    }

    // Q（Peak 时显示）
    if (f.type == EqFilterType::Peak)
    {
        g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.6f));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("Q", juce::Rectangle<float>(cx, cy, 32.0f, kCardRowHeight),
                   juce::Justification::centredLeft, false);
        g.setColour(EqGraphRenderer::hudTextColor());
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(juce::String(f.q, 2),
                   juce::Rectangle<float>(cx + 34.0f, cy, kCardWidth - 40.0f, kCardRowHeight),
                   juce::Justification::centredLeft, false);
    }
}

void EqPopupComponent::updateCardState()
{
    if (isPreview_ || showingRemoveConfirmation_)
    {
        cardBand_ = -1;
        return;
    }
    if (showingValueInput_ && valueInputBand_ >= 0
        && valueInputBand_ < static_cast<int>(settings_.filters.size()))
    {
        cardBand_ = valueInputBand_;
        cardAnchorPos_ = renderer_.anchorPosition(valueInputBand_);
        return;
    }
    if (hoveredBand_ >= 0 && hoveredBand_ < static_cast<int>(settings_.filters.size()))
    {
        cardBand_ = hoveredBand_;
        cardAnchorPos_ = renderer_.anchorPosition(hoveredBand_);
    }
    else if (selectedFilterIndex_ >= 0 && selectedFilterIndex_ < static_cast<int>(settings_.filters.size()))
    {
        cardBand_ = selectedFilterIndex_;
        cardAnchorPos_ = renderer_.anchorPosition(selectedFilterIndex_);
    }
    else
    {
        cardBand_ = -1;
    }
}

// ============================================================================
// 固定公共 API
// ============================================================================

void EqPopupComponent::setEqSettings(const EqSettings& settings)
{
    dismissValueInputPopup();
    settings_ = settings;
    renderer_.setSettings(settings_);
    dragCommitted_ = false;
    cardBand_ = -1;
    for (auto& a : hoverBandAmounts_)
        a = 0.0;
    repaint();
}

void EqPopupComponent::setNoteColor(juce::Colour color)
{
    noteColor_ = color;
    repaint();
}

void EqPopupComponent::setPreviewMode(bool isPreview)
{
    dismissValueInputPopup();
    isPreview_ = isPreview;
    isMaximized_ = !isPreview;
    cardBand_ = -1;
    if (isPreview)
        renderer_.setPreviewFreqRange(50.0, 20000.0);
    else
        renderer_.clearPreviewFreqRange();
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

void EqPopupComponent::resized()
{
    renderer_.setGraphBounds(getLocalBounds().toFloat().reduced(3.0f));
    if (showingValueInput_)
        layoutValueInputOverlay();
}

// ============================================================================
// 绘制
// ============================================================================

void EqPopupComponent::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float cornerRadius = 8.0f;

    const auto bgBase = noteColor_.withSaturation(noteColor_.getSaturation() * 0.3f);
    const auto bgColor = bgBase.interpolatedWith(EqGraphRenderer::backgroundColor(), 0.7f);
    g.setColour(bgColor.withAlpha(0.75f));
    g.fillRoundedRectangle(bounds, cornerRadius);

    g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.25f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);

    renderer_.drawSpectrumBackground(g, spectrum_, spectrumPeaks_);

    if (isPreview_)
        renderer_.drawPreview(g);
    else
        renderer_.drawFull(g, hoveredBand_, activeMousePos_, interaction_.isDragging(),
                           hoveredViewRange_, pressedViewRange_, hoverBandAmounts_,
                           cardBand_);

    // 四控制按钮
    paintButton(g, ButtonId::Maximize, buttonBounds(ButtonId::Maximize),
                hoveredButton_ == static_cast<int>(ButtonId::Maximize));
    paintButton(g, ButtonId::Bypass, buttonBounds(ButtonId::Bypass),
                hoveredButton_ == static_cast<int>(ButtonId::Bypass));
    paintButton(g, ButtonId::Remove, buttonBounds(ButtonId::Remove),
                hoveredButton_ == static_cast<int>(ButtonId::Remove));
    paintButton(g, ButtonId::Minimize, buttonBounds(ButtonId::Minimize),
                hoveredButton_ == static_cast<int>(ButtonId::Minimize));

    // 浮动参数卡（在按钮之上绘制）
    if (!isPreview_ && cardBand_ >= 0 && !showingRemoveConfirmation_)
    {
        paintFloatingCard(g, cardBand_, cardAnchorPos_);
    }

    // 按钮 tooltip
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
        case ButtonId::Minimize:
            tooltipText = isPreview_ ? juce::String::fromUTF8(u8"隐藏EQ预览")
                                     : juce::String::fromUTF8(u8"最小化为预览");
            break;
        default: break;
        }

        if (tooltipText.isNotEmpty())
        {
            const auto btnRect = buttonBounds(id);
            const float tooltipH = 16.0f;
            const float tooltipY = btnRect.getBottom() + 3.0f;
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

    // Remove 确认弹窗
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

} // end of paint()

// ============================================================================
// 按钮绘制
// ============================================================================

void EqPopupComponent::paintButton(juce::Graphics& g, ButtonId id, juce::Rectangle<float> bounds, bool hovered) const
{
    auto color = EqGraphRenderer::axisLabelColor().withAlpha(hovered ? 0.8f : 0.4f);
    if (id == ButtonId::Minimize && hovered)
        color = juce::Colour::fromRGB(220, 60, 60);

    g.setColour(color);
    g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

    const auto iconBounds = bounds.reduced(4.0f);
    switch (id)
    {
    case ButtonId::Maximize: paintMaximizeIcon(g, iconBounds, color); break;
    case ButtonId::Bypass:   paintBypassIcon(g, iconBounds, color); break;
    case ButtonId::Remove:   paintRemoveIcon(g, iconBounds, color); break;
    case ButtonId::Minimize: paintMinimizeIcon(g, iconBounds, color); break;
    case ButtonId::None: break;
    }
}

void EqPopupComponent::paintBypassIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float s = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.55f;

    if (settings_.active)
    {
        g.drawLine(cx - s * 0.45f, cy + s * 0.05f, cx - s * 0.05f, cy + s * 0.40f, 1.8f);
        g.drawLine(cx - s * 0.05f, cy + s * 0.40f, cx + s * 0.45f, cy - s * 0.35f, 1.8f);
    }
}

void EqPopupComponent::paintRemoveIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float w = bounds.getWidth() * 0.55f;
    const float h = bounds.getHeight() * 0.60f;
    const float lidH = h * 0.25f;
    const float bodyTop = cy - h * 0.5f + lidH + 1.0f;
    const float bodyBot = cy + h * 0.5f;

    g.drawLine(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy - h * 0.5f, 1.5f);
    g.drawLine(cx - w * 0.2f, cy - h * 0.5f - lidH, cx + w * 0.2f, cy - h * 0.5f - lidH, 1.5f);
    g.drawLine(cx - w * 0.2f, cy - h * 0.5f, cx - w * 0.2f, cy - h * 0.5f - lidH, 1.5f);
    g.drawLine(cx + w * 0.2f, cy - h * 0.5f, cx + w * 0.2f, cy - h * 0.5f - lidH, 1.5f);
    g.drawLine(cx - w * 0.4f, bodyTop, cx - w * 0.35f, bodyBot, 1.3f);
    g.drawLine(cx + w * 0.4f, bodyTop, cx + w * 0.35f, bodyBot, 1.3f);
    g.drawLine(cx - w * 0.4f, bodyTop, cx + w * 0.4f, bodyTop, 1.3f);
    g.drawLine(cx - w * 0.35f, bodyBot, cx + w * 0.35f, bodyBot, 1.3f);
    g.drawLine(cx - w * 0.15f, bodyTop + 1.0f, cx - w * 0.15f, bodyBot - 1.0f, 1.0f);
    g.drawLine(cx + w * 0.15f, bodyTop + 1.0f, cx + w * 0.15f, bodyBot - 1.0f, 1.0f);
}

void EqPopupComponent::paintMaximizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float s = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.28f;

    if (isMaximized_)
    {
        g.drawLine(cx, cy - s, cx - s, cy, 1.3f);
        g.drawLine(cx, cy - s, cx + s, cy, 1.3f);
        g.drawLine(cx, cy + 1.0f, cx - s, cy + s + 1.0f, 1.3f);
        g.drawLine(cx, cy + 1.0f, cx + s, cy + s + 1.0f, 1.3f);
    }
    else
    {
        g.drawLine(cx, cy + s, cx - s, cy, 1.3f);
        g.drawLine(cx, cy + s, cx + s, cy, 1.3f);
        g.drawLine(cx, cy - 1.0f, cx - s, cy - s - 1.0f, 1.3f);
        g.drawLine(cx, cy - 1.0f, cx + s, cy - s - 1.0f, 1.3f);
    }
}

void EqPopupComponent::paintMinimizeIcon(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour color) const
{
    g.setColour(color);
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float s = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.35f;
    g.drawLine(cx - s, cy, cx + s, cy, 1.5f);
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
    case ButtonId::Minimize: return { topBar.getRight() - 4.0f - kBtnSize, btnY, kBtnSize, kBtnSize };
    case ButtonId::None:     return {};
    }
    return {};
}

EqPopupComponent::ButtonId EqPopupComponent::hitTestButton(juce::Point<float> pos) const
{
    for (auto id : { ButtonId::Maximize, ButtonId::Bypass, ButtonId::Remove, ButtonId::Minimize })
        if (buttonBounds(id).contains(pos))
            return id;
    return ButtonId::None;
}

// ============================================================================
// 鼠标交互
// ============================================================================

void EqPopupComponent::mouseMove(const juce::MouseEvent& event)
{
    activeMousePos_ = event.position;
    const auto pos = event.position;

    hoveredButton_ = -1;
    for (int i = 0; i < 4; ++i)
    {
        const auto id = static_cast<ButtonId>(i);
        if (buttonBounds(id).contains(pos))
        {
            hoveredButton_ = i;
            hoveredBand_ = -1;
            hoveredViewRange_ = -1;
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            repaint();
            return;
        }
    }

    // 浮动卡片区域不穿透 graph hit-test
    if (!isPreview_ && cardBand_ >= 0 && !showingRemoveConfirmation_
        && floatingCardBounds(cardAnchorPos_).contains(pos))
    {
        hoveredBand_ = cardBand_;
        hoveredViewRange_ = -1;
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        updateCardState();
        repaint();
        return;
    }

    hoveredBand_ = renderer_.hitTestAnchor(pos, 10.0f);
    if (hoveredBand_ < 0 && !isPreview_)
        hoveredBand_ = renderer_.hitTestCurve(pos, 12.0f);

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

    setMouseCursor(hoveredBand_ >= 0 ? juce::MouseCursor::PointingHandCursor
                   : (hoveredViewRange_ >= 0 ? juce::MouseCursor::PointingHandCursor
                                              : juce::MouseCursor::NormalCursor));

    updateCardState();
    repaint();
}

void EqPopupComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
        return;

    if (event.getNumberOfClicks() >= 2)
        pendingValueInputBand_ = -1;

    const auto pos = event.position;

    // Remove 确认弹窗交互
    if (showingRemoveConfirmation_)
    {
        if (event.getNumberOfClicks() >= 2)
        {
            dismissRemoveConfirmation();
            // 继续：双击可能命中 anchor
        }
        else
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
                if (onRemoveEq) onRemoveEq();
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
    }

    if (showingValueInput_ && cardBand_ >= 0
        && !floatingCardBounds(cardAnchorPos_).contains(pos))
    {
        dismissValueInputPopup();
    }

    // 多击：交 mouseDoubleClick 处理
    if (event.getNumberOfClicks() >= 2)
    {
        return;
    }

    // 浮动参数卡 filter 按钮点击
    if (cardBand_ >= 0 && !isPreview_)
    {
        const auto cardBounds = floatingCardBounds(cardAnchorPos_);
        const float btnRowY = cardBounds.getY() + 5.0f;
        const float btnRowH = kCardButtonHeight;
        const juce::Rectangle<float> btnRowArea(cardBounds.getX(), btnRowY, kCardWidth, btnRowH);

        if (pos.getY() >= btnRowY && pos.getY() < btnRowY + btnRowH && pos.getX() >= cardBounds.getX() && pos.getX() <= cardBounds.getRight())
        {
            float bx = cardBounds.getX() + 6.0f;
            const int n = static_cast<int>(settings_.filters.size());
            for (int i = 0; i < n; ++i)
            {
                const float bw = 18.0f;
                const juce::Rectangle<float> btnRect(bx, btnRowY, bw, btnRowH);
                if (btnRect.contains(pos))
                {
                    selectedFilterIndex_ = i;
                    dismissValueInputPopup();
                    showValueInputPopup(i);
                    repaint();
                    return;
                }
                bx += bw + 2.0f;
            }

            if (floatingCardBounds(cardAnchorPos_).contains(pos))
                return;
        }
    }

    // 按钮点击
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
            case ButtonId::Minimize:
                // 预览态：关闭预览窗口（保留已提交 EQ）
                // 完整态：toggleMaximize 回预览
                if (isPreview_)
                {
                    if (onClose) onClose();
                }
                else
                {
                    toggleMaximize();
                }
                return;
            }
        }
    }

    // 视图范围按钮
    if (!isPreview_)
    {
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

    // 锚点交互 — mouseDown 记录 selectedFilterIndex_ + pending drag
    {
        const int bandIdx = renderer_.hitTestAnchor(pos, 10.0f);
        if (bandIdx >= 0 && bandIdx < static_cast<int>(settings_.filters.size()))
        {
            if (showingValueInput_)
                dismissValueInputPopup();
            selectedFilterIndex_ = bandIdx;
            pendingDragBand_ = bandIdx;
            pendingDragStartPos_ = pos;
            hoveredBand_ = bandIdx;
            setMouseCursor(juce::MouseCursor::NoCursor);
            repaint();
            return;
        }
    }

    // 窗口拖拽 — 保留完整窗口空白拖动语义
    draggingWindow_ = true;
    const auto mouseInParent = getPosition() + event.getPosition().toInt();
    dragOffset_ = mouseInParent - getPosition();
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void EqPopupComponent::mouseDrag(const juce::MouseEvent& event)
{
    activeMousePos_ = event.position;

    // 窗口拖拽
    if (draggingWindow_)
    {
        const auto parentBounds = getParentComponent() != nullptr
            ? getParentComponent()->getLocalBounds() : getLocalBounds();
        const auto mouseInParent = getPosition() + event.getPosition().toInt();
        const int x = juce::jlimit(parentBounds.getX(),
                                   juce::jmax(parentBounds.getX(), parentBounds.getRight() - getWidth()),
                                   mouseInParent.x - dragOffset_.x);
        const int y = juce::jlimit(parentBounds.getY(),
                                   juce::jmax(parentBounds.getY(), parentBounds.getBottom() - getHeight()),
                                   mouseInParent.y - dragOffset_.y);
        setBounds(x, y, getWidth(), getHeight());
        return;
    }

    // 未启动 drag 时：检查 pending band 是否超过阈值
    if (!interaction_.isDragging())
    {
        if (pendingDragBand_ < 0)
            return;
        if (!interaction_.hasDragThreshold(pendingDragStartPos_, event.position))
            return;
        interaction_.startDrag(pendingDragBand_, pendingDragStartPos_, settings_);
        dragCommitted_ = false;
        wasDragging_ = true;
    }

    settings_ = interaction_.updateDrag(event.position, settings_);
    renderer_.setSettings(settings_);
    // 拖拽后更新浮动卡片位置
    if (cardBand_ >= 0 && cardBand_ < static_cast<int>(settings_.filters.size()))
        cardAnchorPos_ = renderer_.anchorPosition(cardBand_);
    repaint();
}

void EqPopupComponent::mouseUp(const juce::MouseEvent& event)
{
    // 窗口拖拽结束
    if (draggingWindow_)
    {
        draggingWindow_ = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }

    if (interaction_.isDragging())
    {
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

    // 未启动 drag：完整模式下用记录的 band 打开数值输入；预览模式仅反馈选中
    if (pendingDragBand_ >= 0)
    {
        const int band = pendingDragBand_;
        pendingDragBand_ = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        if (!isPreview_)
        {
            pendingValueInputBand_ = band;
            const juce::Component::SafePointer<EqPopupComponent> safeThis(this);
            juce::Timer::callAfterDelay(juce::MouseEvent::getDoubleClickTimeout(),
                                        [safeThis, band]() {
                if (safeThis != nullptr && safeThis->pendingValueInputBand_ == band)
                {
                    safeThis->pendingValueInputBand_ = -1;
                    safeThis->showValueInputPopup(band);
                }
            });
        }
        repaint();
    }

    // 视图范围按钮
    if (pressedViewRange_ >= 0)
    {
        const auto vr = renderer_.hitTestViewRangeButton(event.position);
        const bool sameButton = (vr == EqGraphRenderer::ViewRangeButton::Decrease && pressedViewRange_ == 0)
                             || (vr == EqGraphRenderer::ViewRangeButton::Increase && pressedViewRange_ == 1);
        if (sameButton)
        {
            const double currentRange = renderer_.viewGainRangeDb();
            double newRange = currentRange;
            if (pressedViewRange_ == 0)
                newRange = currentRange >= 30.0 ? 12.0 : (currentRange >= 12.0 ? 6.0 : 6.0);
            else
                newRange = currentRange <= 6.0 ? 12.0 : (currentRange <= 12.0 ? 30.0 : 30.0);
            if (newRange != currentRange)
                renderer_.setViewGainRangeDb(newRange);
        }
        pressedViewRange_ = -1;
        repaint();
    }
}

void EqPopupComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
        return;

    pendingValueInputBand_ = -1;
    pendingDragBand_ = -1;

    if (showingValueInput_)
        dismissValueInputPopup();
    if (showingRemoveConfirmation_)
        dismissRemoveConfirmation();

    const auto pos = event.position;

    // 卡片区域内的双击由卡片消费，不穿透到 graph
    if (cardBand_ >= 0 && floatingCardBounds(cardAnchorPos_).contains(pos))
        return;

    // 命中已有 anchor → 删除该 filter
    const int hitBand = renderer_.hitTestAnchor(pos, 10.0f);
    if (hitBand >= 0 && hitBand < static_cast<int>(settings_.filters.size()))
    {
        removeFilterAt(hitBand);
        return;
    }

    // 未命中 anchor + 完整模式 + 在 graphBounds → 创建新 filter
    if (!isPreview_ && renderer_.graphBounds().contains(pos)
        && static_cast<int>(settings_.filters.size()) < EqSettings::kMaxFilters)
    {
        double freq = renderer_.xToFreq(pos.x);
        double gain = renderer_.yToGain(pos.y);

        EqFilter newFilter;
        newFilter.paletteSlot = -1;
        if (freq < 200.0)
            newFilter.type = EqFilterType::LowShelf;
        else if (freq > 10000.0)
            newFilter.type = EqFilterType::HighShelf;
        else
            newFilter.type = EqFilterType::Peak;

        freq = std::clamp(freq, static_cast<double>(EqSettings::kMinFrequencyHz),
                          static_cast<double>(EqSettings::kMaxFrequencyHz));
        gain = std::clamp(gain, static_cast<double>(EqSettings::kMinGainDb),
                          static_cast<double>(EqSettings::kMaxGainDb));

        newFilter.frequencyHz = static_cast<float>(freq);
        newFilter.gainDb = static_cast<float>(gain);
        newFilter.q = 1.0f;

        settings_.filters.push_back(newFilter);
        EqGraphRenderer::assignPaletteSlots(settings_);
        const int newIndex = static_cast<int>(settings_.filters.size()) - 1;
        selectedFilterIndex_ = newIndex;
        hoveredBand_ = newIndex;

        renderer_.setSettings(settings_);
        commitSettings();
        updateCardState();
        repaint();
        return;
    }
}

void EqPopupComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    // 始终消费，不调用父类/向上转发

    // hover anchor 优先 → selected filter 后备
    int targetBand = renderer_.hitTestAnchor(event.position, 10.0f);
    if (targetBand < 0 || targetBand >= static_cast<int>(settings_.filters.size()))
        targetBand = selectedFilterIndex_;
    if (targetBand < 0 || targetBand >= static_cast<int>(settings_.filters.size()))
        return;

    auto& f = settings_.filters[targetBand];

    // 非 Peak 类型也消费事件（不修改 Q 但阻止滚轮传播）
    if (f.type != EqFilterType::Peak)
        return;

    // 基础步进 ±0.08，Shift 细调乘 0.35
    const double step = 0.08 * (event.mods.isShiftDown() ? 0.35 : 1.0);
    const double delta = wheel.deltaY > 0 ? step : -step;
    f.q = static_cast<float>(std::clamp(
        static_cast<double>(f.q) + delta,
        static_cast<double>(EqSettings::kMinQ),
        static_cast<double>(EqSettings::kMaxQ)));

    renderer_.setSettings(settings_);
    if (qLabel_ && valueInputBand_ == targetBand)
        qLabel_->setText(juce::String(f.q, 2), juce::dontSendNotification);
    commitSettings();
    updateCardState();
    repaint();
}

void EqPopupComponent::mouseExit(const juce::MouseEvent&)
{
    hoveredBand_ = -1;
    hoveredButton_ = -1;
    hoveredViewRange_ = -1;
    pressedViewRange_ = -1;
    repaint();
}

// ============================================================================
// Timer
// ============================================================================

void EqPopupComponent::timerCallback()
{
    const double dt = 1.0 / 30.0;
    if (onReadSpectrum)
        onReadSpectrum(spectrum_, spectrumPeaks_);
    else
    {
        spectrum_.fill(0.0f);
        spectrumPeaks_.fill(0.0f);
    }

    renderer_.advanceSpectrumColorCycle(dt);
    updateHoverBandFade(dt);

    if (interaction_.isDragging())
        repaint();

    // hover 动画活跃时刷新（动态数量）
    const int n = static_cast<int>(settings_.filters.size());
    for (int i = 0; i < n; ++i)
    {
        if (hoverBandAmounts_[static_cast<size_t>(i)] > 0.01)
        {
            repaint();
            break;
        }
    }

    // 频谱动画持续刷新
    repaint();
}

void EqPopupComponent::updateHoverBandFade(double dt)
{
    const int n = static_cast<int>(settings_.filters.size());
    for (int i = 0; i < n; ++i)
    {
        double& amount = hoverBandAmounts_[static_cast<size_t>(i)];
        if (i == hoveredBand_)
            amount = std::min(1.0, amount + dt / kHoverFadeInTime);
        else
            amount = std::max(0.0, amount - dt / kHoverFadeOutTime);
    }
}

// ============================================================================
// 状态操作
// ============================================================================

void EqPopupComponent::toggleMaximize()
{
    dismissValueInputPopup();
    if (isMaximized_)
    {
        isMaximized_ = false;
        isPreview_ = true;
        cardBand_ = -1;
        renderer_.setPreviewFreqRange(50.0, 20000.0);
        if (!savedPreviewBounds_.isEmpty())
            setBounds(savedPreviewBounds_);
        resized();
        repaint();
    }
    else
    {
        isMaximized_ = true;
        isPreview_ = false;
        renderer_.clearPreviewFreqRange();
        savedPreviewBounds_ = getBounds();

        const int targetW = kFullWidth;
        const int targetH = kFullHeight;
        int w = targetW;
        int h = targetH;

        if (auto* parent = getParentComponent())
        {
            const auto parentBounds = parent->getLocalBounds();
            if (w > parentBounds.getWidth() || h > parentBounds.getHeight())
            {
                const double scaleX = static_cast<double>(parentBounds.getWidth()) / w;
                const double scaleY = static_cast<double>(parentBounds.getHeight()) / h;
                const double scale = juce::jmin(scaleX, scaleY);
                w = static_cast<int>(w * scale);
                h = static_cast<int>(h * scale);
            }
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

bool EqPopupComponent::removeFilterAt(int index)
{
    if (showingValueInput_)
        dismissValueInputPopup();
    settings_.filters.erase(settings_.filters.begin() + index);

    if (settings_.filters.empty())
    {
        if (onRemoveEq) onRemoveEq();
        return true; // 'this' 可能已被销毁
    }

    if (selectedFilterIndex_ == index)
        selectedFilterIndex_ = -1;
    else if (selectedFilterIndex_ > index)
        --selectedFilterIndex_;

    if (hoveredBand_ == index)
        hoveredBand_ = -1;
    else if (hoveredBand_ > index)
        --hoveredBand_;

    // cardBand_ 修正：与 selectedFilterIndex_ 同逻辑
    if (cardBand_ == index)
        cardBand_ = -1;
    else if (cardBand_ > index)
        --cardBand_;

    for (int i = index; i < EqSettings::kMaxFilters - 1; ++i)
        hoverBandAmounts_[static_cast<size_t>(i)] = hoverBandAmounts_[static_cast<size_t>(i + 1)];
    hoverBandAmounts_[static_cast<size_t>(EqSettings::kMaxFilters - 1)] = 0.0;

    renderer_.setSettings(settings_);
    commitSettings();
    repaint();
    return false;
}

// ============================================================================
// 数值输入弹窗 — 动态 filter index
// ============================================================================

void EqPopupComponent::showValueInputPopup(int filterIndex)
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return;

    if (showingValueInput_)
        dismissValueInputPopup();

    showingValueInput_ = true;
    valueInputBand_ = filterIndex;
    cardBand_ = filterIndex;
    cardAnchorPos_ = renderer_.anchorPosition(filterIndex);

    const auto& f = settings_.filters[filterIndex];
    const double freq = f.frequencyHz;
    const double gain = f.gainDb;

    const bool hasGain = (f.type == EqFilterType::LowShelf
                       || f.type == EqFilterType::Peak
                       || f.type == EqFilterType::HighShelf);

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

        if (f.type == EqFilterType::Peak)
            qLabel_ = std::make_unique<juce::Label>(juce::String(), juce::String(f.q, 2));
        if (qLabel_)
        {
            qLabel_->setFont(juce::Font(juce::FontOptions(10.0f)));
            qLabel_->setColour(juce::Label::textColourId, EqGraphRenderer::hudTextColor());
            addAndMakeVisible(qLabel_.get());
        }
    }

    valueInputOk_ = std::make_unique<juce::TextButton>("OK");
    valueInputOk_->setColour(juce::TextButton::buttonColourId, EqGraphRenderer::combinedCurveColor());
    valueInputOk_->setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    valueInputOk_->onClick = [this] {
        if (freqEditor_ && freqEditor_->getText().isNotEmpty()
            && valueInputBand_ >= 0
            && valueInputBand_ < static_cast<int>(settings_.filters.size()))
        {
            auto& f = settings_.filters[valueInputBand_];
            const double newFreq = freqEditor_->getText().getDoubleValue();
            // Peak 频率范围 500-12000，其余 20-20000
            if (f.type == EqFilterType::Peak)
                f.frequencyHz = std::clamp(static_cast<float>(newFreq), 500.0f, 12000.0f);
            else
                f.frequencyHz = std::clamp(static_cast<float>(newFreq), 20.0f, 20000.0f);
        }
        if (gainEditor_ && gainEditor_->getText().isNotEmpty()
            && valueInputBand_ >= 0
            && valueInputBand_ < static_cast<int>(settings_.filters.size()))
        {
            auto& f = settings_.filters[valueInputBand_];
            const double newGain = gainEditor_->getText().getDoubleValue();
            f.gainDb = std::clamp(static_cast<float>(newGain), -12.0f, 12.0f);
        }
        // type 和 q 保留不变
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
    if (!isPreview_ && !showingRemoveConfirmation_)
        updateCardState();
}

void EqPopupComponent::layoutValueInputOverlay()
{
    if (cardBand_ < 0)
        return;

    const auto card = floatingCardBounds(cardAnchorPos_);
    const float panelX = card.getX();
    const float panelY = card.getY();
    const float panelW = card.getWidth();

    const float rowH = 20.0f;
    const float gap = 6.0f;
    const float fieldW = panelW - 48.0f;

    float y = panelY + kCardButtonHeight + 9.0f;
    const float x = panelX + 40.0f;

    if (freqEditor_)
        freqEditor_->setBounds(static_cast<int>(x), static_cast<int>(y),
                               static_cast<int>(fieldW), static_cast<int>(rowH));

    const bool hasGain = (valueInputBand_ >= 0
                       && valueInputBand_ < static_cast<int>(settings_.filters.size()))
                       && (settings_.filters[valueInputBand_].type == EqFilterType::LowShelf
                        || settings_.filters[valueInputBand_].type == EqFilterType::Peak
                        || settings_.filters[valueInputBand_].type == EqFilterType::HighShelf);

    if (hasGain)
    {
        y += rowH;
        if (gainEditor_)
            gainEditor_->setBounds(static_cast<int>(x), static_cast<int>(y),
                                   static_cast<int>(fieldW), static_cast<int>(rowH));
    }

    if (qLabel_)
    {
        y += rowH;
        qLabel_->setBounds(static_cast<int>(x), static_cast<int>(y),
                           static_cast<int>(fieldW), static_cast<int>(rowH));
    }

    const float btnW = 60.0f;
    const float buttonY = card.getBottom() - rowH - 5.0f;
    float buttonX = panelX + (panelW - btnW * 2.0f - gap) * 0.5f;

    if (valueInputOk_)
        valueInputOk_->setBounds(static_cast<int>(buttonX), static_cast<int>(buttonY),
                                 static_cast<int>(btnW), static_cast<int>(rowH));
    buttonX += btnW + gap;
    if (valueInputCancel_)
        valueInputCancel_->setBounds(static_cast<int>(buttonX), static_cast<int>(buttonY),
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

} // namespace OpenTune

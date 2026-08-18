/**
 * EQ Popup Component — per-note EQ 编辑弹窗（实现）
 *
 * 动态滤波器列表，最多 kMaxFilters=10。
 * 双击创建/删除 anchor，滚轮调 Q（Peak），Minimize 行为随态变化。
 */

#include "EqPopupComponent.h"
#include "../ParameterPanel.h"
#include "../UIColors.h"
#include <cmath>

namespace OpenTune {

EqPopupComponent::EqPopupComponent()
{
    setOpaque(false);
    startTimerHz(60);
    interaction_.setRenderer(&renderer_);

    // 创建旋钮 LookAndFeel
    cardKnobLookAndFeel_ = std::make_unique<LargeKnobLookAndFeel>();

    // 频率旋钮
    frequencySlider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    frequencySlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 74, 24);
    frequencySlider_.setRange(20.0, 20000.0, 1.0);
    frequencySlider_.setRotaryParameters(juce::degreesToRadians(210.0f), juce::degreesToRadians(510.0f), true);
    frequencySlider_.setDoubleClickReturnValue(true, 1000.0);
    frequencySlider_.setTextValueSuffix(" Hz");
    frequencySlider_.setNumDecimalPlacesToDisplay(0);
    frequencySlider_.setSkewFactorFromMidPoint(1000.0);
    frequencySlider_.setLookAndFeel(cardKnobLookAndFeel_.get());
    frequencySlider_.setScrollWheelEnabled(false);
    frequencySlider_.setPopupDisplayEnabled(false, false, nullptr);
    frequencySlider_.onValueChange = [this] { cardSliderChanged(0); };
    addAndMakeVisible(frequencySlider_);
    frequencySlider_.setVisible(false);

    // 增益旋钮
    gainSlider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    gainSlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 74, 24);
    gainSlider_.setRange(-12.0, 12.0, 0.1);
    gainSlider_.setRotaryParameters(juce::degreesToRadians(210.0f), juce::degreesToRadians(510.0f), true);
    gainSlider_.setDoubleClickReturnValue(true, 0.0);
    gainSlider_.setTextValueSuffix(" dB");
    gainSlider_.setNumDecimalPlacesToDisplay(1);
    gainSlider_.setLookAndFeel(cardKnobLookAndFeel_.get());
    gainSlider_.setScrollWheelEnabled(false);
    gainSlider_.setPopupDisplayEnabled(false, false, nullptr);
    gainSlider_.onValueChange = [this] { cardSliderChanged(1); };
    addAndMakeVisible(gainSlider_);
    gainSlider_.setVisible(false);

    // Q 旋钮
    qSlider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    qSlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 74, 24);
    qSlider_.setRange(0.25, 9.0, 0.01);
    qSlider_.setRotaryParameters(juce::degreesToRadians(210.0f), juce::degreesToRadians(510.0f), true);
    qSlider_.setDoubleClickReturnValue(true, 1.0);
    qSlider_.setNumDecimalPlacesToDisplay(2);
    qSlider_.setLookAndFeel(cardKnobLookAndFeel_.get());
    qSlider_.setScrollWheelEnabled(false);
    qSlider_.setPopupDisplayEnabled(false, false, nullptr);
    qSlider_.onValueChange = [this] { cardSliderChanged(2); };
    addAndMakeVisible(qSlider_);
    qSlider_.setVisible(false);
}

EqPopupComponent::~EqPopupComponent()
{
    stopTimer();
    frequencySlider_.setLookAndFeel(nullptr);
    gainSlider_.setLookAndFeel(nullptr);
    qSlider_.setLookAndFeel(nullptr);
    showingRemoveConfirmation_ = false;
}

// ============================================================================
// 浮动参数卡
// ============================================================================

juce::Rectangle<float> EqPopupComponent::floatingCardBounds() const
{
    const float cardH = kCardPadding + kCardIconRowH + 3.0f + kCardTypeRowH + 3.0f
                        + kCardLabelHeight + kCardSliderHeight + kCardPadding;
    const float compW = static_cast<float>(getWidth());
    const float compH = static_cast<float>(getHeight());
    float x = (compW - kCardWidth) * 0.5f;
    float y = compH - cardH - 4.0f;
    // 夹紧到组件边界内
    const float minX = 2.0f;
    const float maxX = compW - kCardWidth - 2.0f;
    if (maxX > minX)
        x = juce::jlimit(minX, maxX, x);
    else
        x = minX;
    const float minY = kTopBarHeight + 2.0f;
    y = juce::jmax(minY, y);
    return { x, y, kCardWidth, cardH };
}

void EqPopupComponent::paintFloatingCard(juce::Graphics& g, int filterIndex) const
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return;

    const auto card = floatingCardBounds();
    const auto color = renderer_.bandColor(filterIndex);

    // 卡片背景
    g.setColour(EqGraphRenderer::hudBgColor().withAlpha(0.93f));
    g.fillRoundedRectangle(card, 6.0f);
    g.setColour(color.withAlpha(0.45f));
    g.drawRoundedRectangle(card, 6.0f, 1.0f);

    // 顶部：滤波器图标色号矩阵，居中均匀分布
    {
        const int n = static_cast<int>(settings_.filters.size());
        const float iconSize = 18.0f;
        const float iconGap = 2.0f;
        const float totalW = n * iconSize + (n - 1) * iconGap;
        float bx = card.getX() + (card.getWidth() - totalW) * 0.5f;
        float cy = card.getY() + kCardPadding;
        for (int i = 0; i < n; ++i)
        {
            const bool isActive = (i == filterIndex);
            const auto c = renderer_.bandColor(i);
            const juce::Rectangle<float> iconRect(bx, cy, iconSize, kCardIconRowH);
            g.setColour(isActive ? c.withAlpha(0.85f) : c.withAlpha(0.30f));
            g.fillRoundedRectangle(iconRect, 3.0f);
            g.setColour(juce::Colours::white.withAlpha(isActive ? 0.9f : 0.35f));
            g.setFont(juce::FontOptions(9.0f));
            g.drawText(juce::String(i + 1), iconRect, juce::Justification::centred, false);
            bx += iconSize + iconGap;
        }
    }

    // 滤波器类型按钮行：5 个类型图标，居中排列
    {
        const float btnY = card.getY() + kCardPadding + kCardIconRowH + 3.0f;
        const int numTypes = 5;
        const float totalW = numTypes * kCardTypeBtnSize + (numTypes - 1) * kCardTypeBtnGap;
        float bx = card.getX() + (card.getWidth() - totalW) * 0.5f;
        const auto currentType = settings_.filters[filterIndex].type;
        for (int i = 0; i < numTypes; ++i)
        {
            const auto type = static_cast<EqFilterType>(i);
            const bool isActive = (type == currentType);
            const bool isHov = (i == hoveredTypeButton_);
            const juce::Rectangle<float> btnRect(bx, btnY, kCardTypeBtnSize, kCardTypeBtnSize);
            paintFilterTypeButton(g, type, btnRect, isActive, isHov);
            bx += kCardTypeBtnSize + kCardTypeBtnGap;
        }
    }

    // 旋钮上方标签：Freq / Gain / Q
    const float labelY = card.getY() + kCardPadding + kCardIconRowH + 3.0f + kCardTypeRowH + 3.0f;
    {
        const char* labels[] = { "Freq", "Gain", "Q" };
        for (int i = 0; i < 3; ++i)
        {
            const float x = card.getX() + kCardPadding + static_cast<float>(i) * kCardColumnWidth;
            g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.6f));
            g.setFont(juce::FontOptions(8.0f));
            g.drawText(labels[i], juce::Rectangle<float>(x, labelY, kCardColumnWidth, kCardLabelHeight),
                       juce::Justification::centred, false);
        }
    }
}

juce::Rectangle<float> EqPopupComponent::cardFilterButtonBounds(
    const juce::Rectangle<float>& cardBounds, int index, int count) const
{
    const float iconSize = 18.0f;
    const float iconGap = 2.0f;
    const float totalW = static_cast<float>(count) * iconSize + static_cast<float>(count - 1) * iconGap;
    const float startX = cardBounds.getX() + (cardBounds.getWidth() - totalW) * 0.5f;
    const float x = startX + static_cast<float>(index) * (iconSize + iconGap);
    return { x, cardBounds.getY() + kCardPadding, iconSize, kCardIconRowH };
}

juce::Rectangle<float> EqPopupComponent::cardFilterTypeButtonBounds(
    const juce::Rectangle<float>& cardBounds, int index) const
{
    const int numTypes = 5;
    const float totalW = numTypes * kCardTypeBtnSize + (numTypes - 1) * kCardTypeBtnGap;
    const float startX = cardBounds.getX() + (cardBounds.getWidth() - totalW) * 0.5f;
    const float x = startX + static_cast<float>(index) * (kCardTypeBtnSize + kCardTypeBtnGap);
    const float y = cardBounds.getY() + kCardPadding + kCardIconRowH + 3.0f;
    return { x, y, kCardTypeBtnSize, kCardTypeBtnSize };
}

int EqPopupComponent::hitTestFilterTypeButton(juce::Point<float> pos) const
{
    if (cardBand_ < 0 || cardBand_ >= static_cast<int>(settings_.filters.size()))
        return -1;
    const auto card = floatingCardBounds();
    for (int i = 0; i < 5; ++i)
    {
        if (cardFilterTypeButtonBounds(card, i).contains(pos))
            return i;
    }
    return -1;
}

void EqPopupComponent::paintFilterTypeButton(juce::Graphics& g, EqFilterType type,
    const juce::Rectangle<float>& bounds, bool isActive, bool isHovered) const
{
    const float inset = 5.0f;
    const float left = bounds.getX() + inset;
    const float right = bounds.getRight() - inset;
    const float top = bounds.getY() + inset;
    const float bottom = bounds.getBottom() - inset;
    const float mid = (top + bottom) * 0.5f;

    // 背景
    auto bgColor = isActive
        ? juce::Colour::fromRGBA(255, 255, 255, 32)
        : (isHovered ? juce::Colour::fromRGBA(255, 255, 255, 16) : juce::Colour::fromRGBA(0, 0, 0, 0));
    g.setColour(bgColor);
    g.fillRoundedRectangle(bounds, 3.0f);

    // 边框
    g.setColour(juce::Colours::white.withAlpha(isActive ? 0.55f : (isHovered ? 0.25f : 0.12f)));
    g.drawRoundedRectangle(bounds, 3.0f, 0.8f);

    // 图标曲线颜色
    auto curveColor = isActive
        ? juce::Colours::white.withAlpha(0.95f)
        : juce::Colours::white.withAlpha(isHovered ? 0.65f : 0.30f);
    g.setColour(curveColor);

    juce::Path p;
    switch (type)
    {
    case EqFilterType::LowCut:
        p.startNewSubPath(left, bottom);
        p.cubicTo(left, mid, left, mid, right, top);
        break;
    case EqFilterType::LowShelf:
        p.startNewSubPath(left, bottom);
        p.lineTo(left + (right - left) * 0.4f, bottom);
        p.cubicTo(left + (right - left) * 0.6f, bottom, left + (right - left) * 0.4f, top, right, top);
        break;
    case EqFilterType::Peak:
        p.startNewSubPath(left, mid);
        p.lineTo(left + (right - left) * 0.25f, mid);
        p.cubicTo(left + (right - left) * 0.35f, top, left + (right - left) * 0.65f, top, left + (right - left) * 0.75f, mid);
        p.lineTo(right, mid);
        break;
    case EqFilterType::HighShelf:
        p.startNewSubPath(left, top);
        p.lineTo(left + (right - left) * 0.4f, top);
        p.cubicTo(left + (right - left) * 0.6f, top, left + (right - left) * 0.4f, bottom, right, bottom);
        break;
    case EqFilterType::HighCut:
        p.startNewSubPath(left, top);
        p.cubicTo(left, mid, left, mid, right, bottom);
        break;
    }
    g.strokePath(p, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void EqPopupComponent::updateCardState()
{
    if (isPreview_ || showingRemoveConfirmation_)
    {
        hideCardControls();
        cardBand_ = -1;
        return;
    }
    if (selectedFilterIndex_ >= 0 && selectedFilterIndex_ < static_cast<int>(settings_.filters.size()))
    {
        cardBand_ = selectedFilterIndex_;
        configureCardControls(selectedFilterIndex_);
    }
    else
    {
        hideCardControls();
        cardBand_ = -1;
    }
}

// ============================================================================
// 卡片内旋钮控件
// ============================================================================

void EqPopupComponent::configureCardControls(int filterIndex)
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
    {
        hideCardControls();
        return;
    }

    const auto& f = settings_.filters[filterIndex];

    updatingCardControls_ = true;

    // 频率范围：Peak 使用 500..12000，其余 20..20000
    const double freqMin = (f.type == EqFilterType::Peak) ? 500.0 : 20.0;
    const double freqMax = (f.type == EqFilterType::Peak) ? 12000.0 : 20000.0;
    frequencySlider_.setRange(freqMin, freqMax, 1.0);
    frequencySlider_.setSkewFactorFromMidPoint(1000.0);
    frequencySlider_.setValue(f.frequencyHz, juce::dontSendNotification);

    // Gain 仅 LowShelf/Peak/HighShelf 启用
    const bool hasGain = (f.type == EqFilterType::LowShelf
                       || f.type == EqFilterType::Peak
                       || f.type == EqFilterType::HighShelf);
    gainSlider_.setEnabled(hasGain);
    gainSlider_.setAlpha(hasGain ? 1.0f : 0.3f);
    gainSlider_.setValue(f.gainDb, juce::dontSendNotification);

    // Q 始终显示
    qSlider_.setValue(f.q, juce::dontSendNotification);

    // 设置弧光颜色为滤波器主题色
    const auto filterColor = renderer_.bandColor(filterIndex);
    const auto colorVar = static_cast<int>(filterColor.getARGB());
    frequencySlider_.getProperties().set("arcColor", colorVar);
    gainSlider_.getProperties().set("arcColor", colorVar);
    qSlider_.getProperties().set("arcColor", colorVar);

    updatingCardControls_ = false;

    frequencySlider_.setVisible(true);
    gainSlider_.setVisible(true);
    qSlider_.setVisible(true);

    layoutCardControls();
}

void EqPopupComponent::hideCardControls()
{
    frequencySlider_.setVisible(false);
    gainSlider_.setVisible(false);
    qSlider_.setVisible(false);
}

void EqPopupComponent::layoutCardControls()
{
    if (cardBand_ < 0)
        return;

    const auto card = floatingCardBounds();
    const float sliderY = card.getY() + kCardPadding + kCardIconRowH + 3.0f + kCardTypeRowH + 3.0f + kCardLabelHeight;

    for (int i = 0; i < 3; ++i)
    {
        const float x = card.getX() + kCardPadding + static_cast<float>(i) * kCardColumnWidth;
        auto* slider = (i == 0) ? &frequencySlider_ : (i == 1) ? &gainSlider_ : &qSlider_;
        slider->setBounds(static_cast<int>(x), static_cast<int>(sliderY),
                          static_cast<int>(kCardColumnWidth), static_cast<int>(kCardSliderHeight));
    }
}

void EqPopupComponent::cardSliderChanged(int parameterIndex)
{
    if (updatingCardControls_)
        return;
    if (cardBand_ < 0 || cardBand_ >= static_cast<int>(settings_.filters.size()))
        return;

    auto& f = settings_.filters[cardBand_];

    switch (parameterIndex)
    {
    case 0: // Frequency
    {
        const double freqMin = (f.type == EqFilterType::Peak) ? 500.0 : 20.0;
        const double freqMax = (f.type == EqFilterType::Peak) ? 12000.0 : 20000.0;
        f.frequencyHz = static_cast<float>(juce::jlimit(freqMin, freqMax, frequencySlider_.getValue()));
        break;
    }
    case 1: // Gain
        f.gainDb = static_cast<float>(juce::jlimit(-12.0, 12.0, gainSlider_.getValue()));
        break;
    case 2: // Q
        f.q = static_cast<float>(juce::jlimit(
            static_cast<double>(EqSettings::kMinQ),
            static_cast<double>(EqSettings::kMaxQ),
            qSlider_.getValue()));
        break;
    }

    renderer_.setSettings(settings_);
    layoutCardControls();
    commitSettings();
    repaint();
}

// ============================================================================
// 固定公共 API
// ============================================================================

void EqPopupComponent::setEqSettings(const EqSettings& settings)
{
    hideCardControls();
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
    hideCardControls();
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
    if (cardBand_ >= 0)
    {
        layoutCardControls();
    }
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

    // 浮动参数卡（在按钮之上绘制；真实 Slider 子控件由 JUCE 自动绘制）
    if (!isPreview_ && cardBand_ >= 0 && !showingRemoveConfirmation_)
    {
        paintFloatingCard(g, cardBand_);
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
    // 右上角排列：最小化-最大化-删除-旁通（从右到左）
    const float startX = topBar.getRight() - 4.0f - kBtnSize;
    switch (id)
    {
    case ButtonId::Bypass:   return { startX, btnY, kBtnSize, kBtnSize };
    case ButtonId::Remove:   return { startX - (kBtnSize + kBtnGap), btnY, kBtnSize, kBtnSize };
    case ButtonId::Maximize: return { startX - (kBtnSize + kBtnGap) * 2, btnY, kBtnSize, kBtnSize };
    case ButtonId::Minimize: return { startX - (kBtnSize + kBtnGap) * 3, btnY, kBtnSize, kBtnSize };
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

    if (showingRemoveConfirmation_)
    {
        hoveredBand_ = -1;
        hoveredButton_ = -1;
        hoveredViewRange_ = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

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

    // 浮动卡片区域 — 最高命中层，不穿透 graph hit-test
    if (!isPreview_ && cardBand_ >= 0 && !showingRemoveConfirmation_
        && floatingCardBounds().contains(pos))
    {
        hoveredBand_ = -1;
        hoveredViewRange_ = -1;
        hoveredTypeButton_ = hitTestFilterTypeButton(pos);
        setMouseCursor(hoveredTypeButton_ >= 0 ? juce::MouseCursor::PointingHandCursor
                                               : juce::MouseCursor::NormalCursor);
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

    // 多击：交 mouseDoubleClick 处理
    if (event.getNumberOfClicks() >= 2)
    {
        return;
    }

    // 完整模式卡片守卫 — 最高优先级，拦截卡片区域所有单击事件
    if (!isPreview_ && cardBand_ >= 0 && !showingRemoveConfirmation_
        && floatingCardBounds().contains(pos))
    {
        // 命中顶部图标 → 切换 selectedFilterIndex_
        const int n = static_cast<int>(settings_.filters.size());
        const auto card = floatingCardBounds();
        for (int i = 0; i < n; ++i)
        {
            if (cardFilterButtonBounds(card, i, n).contains(pos))
            {
                selectedFilterIndex_ = i;
                updateCardState();
                repaint();
                return;
            }
        }

        // 命中滤波器类型按钮 → 切换当前滤波器类型
        const int typeIdx = hitTestFilterTypeButton(pos);
        if (typeIdx >= 0 && typeIdx < 5 && cardBand_ >= 0
            && cardBand_ < static_cast<int>(settings_.filters.size()))
        {
            auto& f = settings_.filters[cardBand_];
            const auto newType = static_cast<EqFilterType>(typeIdx);
            if (f.type != newType)
            {
                f.type = newType;
                // 频率范围适配
                if (newType == EqFilterType::Peak)
                    f.frequencyHz = std::clamp(f.frequencyHz, 500.0f, 12000.0f);
                else
                    f.frequencyHz = std::clamp(f.frequencyHz, 20.0f, 20000.0f);
                // Cut 类型增益归零
                if (newType == EqFilterType::LowCut || newType == EqFilterType::HighCut)
                    f.gainDb = 0.0f;
                renderer_.setSettings(settings_);
                configureCardControls(cardBand_);
                commitSettings();
                repaint();
            }
            return;
        }

        // 卡片其余空白区域 → 消费事件
        return;
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
                    hideCardControls();
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
            // 点击已选中的锚点 → 取消选中并隐藏卡片
            if (selectedFilterIndex_ == bandIdx)
                selectedFilterIndex_ = -1;
            else
                selectedFilterIndex_ = bandIdx;
            pendingDragBand_ = bandIdx;
            pendingDragStartPos_ = pos;
            hoveredBand_ = bandIdx;
            updateCardState();
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
    // 拖拽时同步卡片控件值（若卡片正在显示该 band）
    if (cardBand_ >= 0 && cardBand_ < static_cast<int>(settings_.filters.size()))
    {
        configureCardControls(cardBand_);
    }
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

    // 未启动 drag：单击锚点仅结束 pending drag，不再打开任何输入入口
    if (pendingDragBand_ >= 0)
    {
        pendingDragBand_ = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);
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
            {
                renderer_.setViewGainRangeDb(newRange);
                if (cardBand_ >= 0 && cardBand_ < static_cast<int>(settings_.filters.size()))
                {
                    layoutCardControls();
                }
            }
        }
        pressedViewRange_ = -1;
        repaint();
    }
}

void EqPopupComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
        return;

    pendingDragBand_ = -1;

    if (showingRemoveConfirmation_)
        dismissRemoveConfirmation();

    const auto pos = event.position;

    // 卡片区域内的双击由卡片消费，不穿透到 graph
    if (cardBand_ >= 0 && floatingCardBounds().contains(pos))
        return;

    // 命中已有 anchor → 删除该 filter
    const int hitBand = renderer_.hitTestAnchor(pos, 10.0f);
    if (hitBand >= 0 && hitBand < static_cast<int>(settings_.filters.size()))
    {
        removeFilterAt(hitBand);
        return;
    }

    // 未命中 anchor + 在 graphBounds → 创建新 filter
    if (renderer_.graphBounds().contains(pos)
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

        if (newFilter.type == EqFilterType::Peak)
            freq = std::clamp(freq, 500.0, 12000.0);
        else
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

    if (showingRemoveConfirmation_)
        return;

    // hover anchor 优先 → selected filter 后备
    if (!isPreview_ && cardBand_ >= 0
        && floatingCardBounds().contains(event.position))
    {
        return;
    }

    int targetBand = renderer_.hitTestAnchor(event.position, 10.0f);
    if (targetBand < 0 || targetBand >= static_cast<int>(settings_.filters.size()))
        targetBand = selectedFilterIndex_;
    if (targetBand < 0 || targetBand >= static_cast<int>(settings_.filters.size()))
        return;

    auto& f = settings_.filters[targetBand];

    // 所有类型均支持 Q 滚轮调节

    // 基础步进 ±0.08，Shift 细调乘 0.35
    const double step = 0.08 * (event.mods.isShiftDown() ? 0.35 : 1.0);
    const double delta = wheel.deltaY > 0 ? step : -step;
    f.q = static_cast<float>(std::clamp(
        static_cast<double>(f.q) + delta,
        static_cast<double>(EqSettings::kMinQ),
        static_cast<double>(EqSettings::kMaxQ)));

    renderer_.setSettings(settings_);

    // 如果卡片正显示该 band，同步 Q 旋钮
    if (cardBand_ == targetBand && !updatingCardControls_)
    {
        updatingCardControls_ = true;
        qSlider_.setValue(f.q, juce::dontSendNotification);
        updatingCardControls_ = false;
    }

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
    const double dt = 1.0 / 60.0;
    if (onReadSpectrum)
        onReadSpectrum(spectrum_, spectrumPeaks_);
    else
    {
        spectrum_.fill(0.0f);
        spectrumPeaks_.fill(0.0f);
    }

    renderer_.advanceSpectrumColorCycle(dt);
    updateHoverBandFade(dt);

    // 频谱动画每帧刷新（60fps 流畅动画）
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
    hideCardControls();
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
        updateCardState();
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
    hideCardControls();
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

    if (cardBand_ == index)
        cardBand_ = -1;
    else if (cardBand_ > index)
        --cardBand_;

    for (int i = index; i < EqSettings::kMaxFilters - 1; ++i)
        hoverBandAmounts_[static_cast<size_t>(i)] = hoverBandAmounts_[static_cast<size_t>(i + 1)];
    hoverBandAmounts_[static_cast<size_t>(EqSettings::kMaxFilters - 1)] = 0.0;

    renderer_.setSettings(settings_);
    updateCardState();
    commitSettings();
    repaint();
    return false;
}

// ============================================================================
// Remove 确认弹窗
// ============================================================================

void EqPopupComponent::dismissRemoveConfirmation()
{
    showingRemoveConfirmation_ = false;
    updateCardState();
    repaint();
}

} // namespace OpenTune

/**
 * EQ Popup Component — per-note EQ 编辑弹窗（实现）
 *
 * 动态滤波器列表，最多 kMaxFilters=10。
 * 双击创建/删除 anchor，滚轮调 Q（Peak），Minimize 行为随态变化。
 */

#include "EqPopupComponent.h"
#include "../ParameterPanel.h"
#include "../UIColors.h"
#include "../../../Utils/LocalizationManager.h"
#include "../../../Utils/EqFilterEditOps.h"
#include "../../../Utils/ScreenReaderAnnouncer.h"
#include <cmath>
#include <utility>

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
    frequencySlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 59, 19);
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
    gainSlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 59, 19);
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
    qSlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 59, 19);
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

    // Accessibility: the window itself is the keyboard grid. A mouse click must not take
    // focus from the notes, so mouse users keep their piano roll keys.
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(false);

    frequencySlider_.setTitle(LOC_RAW("Frequency"));
    gainSlider_.setTitle(LOC_RAW("Gain"));
    qSlider_.setTitle("Q");
    for (auto* knob : { &frequencySlider_, &gainSlider_, &qSlider_ })
    {
        knob->setWantsKeyboardFocus(true);
        knob->setMouseClickGrabsKeyboardFocus(false);
        knob->onKey = [this, knob](const juce::KeyPress& key) { return handleKnobKey(*knob, key); };
    }

    // Type list over the painted type buttons: invisible and click-through, the painted
    // buttons keep working for the mouse.
    for (int i = 0; i <= static_cast<int>(EqSettings::kMaxFilterType); ++i)
        typeBox_.addItem(filterTypeName(static_cast<EqFilterType>(i)), i + 1);
    typeBox_.setTitle(LOC_RAW("Type"));
    typeBox_.setWantsKeyboardFocus(true);
    typeBox_.setAlpha(0.0f);
    typeBox_.setInterceptsMouseClicks(false, false);
    typeBox_.onKey = [this](const juce::KeyPress& key) { return handleTypeKey(key); };
    typeBox_.onChange = [this] {
        if (!updatingCardControls_ && typeBox_.getSelectedId() > 0)
            setFilterType(cardBand_, static_cast<EqFilterType>(typeBox_.getSelectedId() - 1));
    };
    addChildComponent(typeBox_);

    // Remove confirmation: real buttons for the screen reader over the painted dialog.
    confirmPanel_.setTitle(LOC_RAW("Remove EQ?"));
    confirmPanel_.setAlpha(0.0f);
    confirmPanel_.setInterceptsMouseClicks(false, false);
    confirmPanel_.onKey = [this](const juce::KeyPress& key) { return handleConfirmKey(key); };
    confirmButton_.setButtonText(LOC_RAW("Confirm"));
    // The question goes first in the button's name ("Remove EQ? Confirm, button"): NVDA skips
    // the group name, and an announcement after the focus came after "Confirm button".
    confirmButton_.setTitle(LOC_RAW("Remove EQ?") + " " + LOC_RAW("Confirm"));
    cancelButton_.setButtonText(LOC(kCancel));
    dontShowToggle_.setButtonText(LOC_RAW("Don't show again"));
    confirmButton_.onClick = [this] { requestRemoveEq(); };
    cancelButton_.onClick = [this] {
        dismissRemoveConfirmation();
        keyRow_ = KeyRow::RemoveEq;
        focusGrid();
    };
    dontShowToggle_.onClick = [this] {
        suppressRemoveConfirmation_ = dontShowToggle_.getToggleState();
        if (onRemoveConfirmationSuppressed)
            onRemoveConfirmationSuppressed(suppressRemoveConfirmation_);
        repaint();
    };
    confirmPanel_.addAndMakeVisible(confirmButton_);
    confirmPanel_.addAndMakeVisible(cancelButton_);
    confirmPanel_.addAndMakeVisible(dontShowToggle_);
    addChildComponent(confirmPanel_);
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
    const float cardH = kCardPadding + kCardTypeRowH + 3.0f
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

    // 滤波器类型按钮行：5 个类型图标，居中排列
    {
        const float btnY = card.getY() + kCardPadding;
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
    const float labelY = card.getY() + kCardPadding + kCardTypeRowH + 3.0f;
    {
        const char* labels[] = { "Freq", "Gain", "Q" };
        for (int i = 0; i < 3; ++i)
        {
            const float x = card.getX() + kCardPadding + static_cast<float>(i) * kCardColumnWidth;
            g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.6f));
            g.setFont(UIColors::getUIFontExact(10.0f));
            g.drawText(labels[i], juce::Rectangle<float>(x, labelY, kCardColumnWidth, kCardLabelHeight),
                       juce::Justification::centred, false);
        }
    }

    // 卡片右上角：旁通 + 删除按钮
    {
        const bool bypassed = settings_.filters[filterIndex].bypassed;
        const auto bypassBounds = cardButtonBounds(CardButton::Bypass, card);
        const auto removeBounds = cardButtonBounds(CardButton::Remove, card);
        const bool hovBypass = (hoveredCardButton_ == static_cast<int>(CardButton::Bypass));
        const bool hovRemove = (hoveredCardButton_ == static_cast<int>(CardButton::Remove));

        auto bypassCol = EqGraphRenderer::axisLabelColor().withAlpha(bypassed ? 0.8f : (hovBypass ? 0.6f : 0.35f));
        auto removeCol = EqGraphRenderer::axisLabelColor().withAlpha(hovRemove ? 0.8f : 0.35f);
        if (hovRemove)
            removeCol = juce::Colour::fromRGB(220, 60, 60).withAlpha(0.8f);

        g.setColour(bypassCol);
        g.drawRoundedRectangle(bypassBounds, 3.0f, 1.0f);
        paintBypassIcon(g, bypassBounds.reduced(4.0f), bypassCol);

        g.setColour(removeCol);
        g.drawRoundedRectangle(removeBounds, 3.0f, 1.0f);
        paintRemoveIcon(g, removeBounds.reduced(4.0f), removeCol);
    }
}

juce::Rectangle<float> EqPopupComponent::cardFilterTypeButtonBounds(
    const juce::Rectangle<float>& cardBounds, int index) const
{
    const int numTypes = 5;
    const float totalW = numTypes * kCardTypeBtnSize + (numTypes - 1) * kCardTypeBtnGap;
    const float startX = cardBounds.getX() + (cardBounds.getWidth() - totalW) * 0.5f;
    const float x = startX + static_cast<float>(index) * (kCardTypeBtnSize + kCardTypeBtnGap);
    const float y = cardBounds.getY() + kCardPadding;
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
        hoveredCardButton_ = -1;
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
    typeBox_.setSelectedId(static_cast<int>(f.type) + 1, juce::dontSendNotification);

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
    typeBox_.setVisible(true);

    layoutCardControls();
}

void EqPopupComponent::hideCardControls()
{
    frequencySlider_.setVisible(false);
    gainSlider_.setVisible(false);
    qSlider_.setVisible(false);
    typeBox_.setVisible(false);
}

void EqPopupComponent::layoutCardControls()
{
    if (cardBand_ < 0)
        return;

    const auto card = floatingCardBounds();
    const float sliderY = card.getY() + kCardPadding + kCardTypeRowH + 3.0f + kCardLabelHeight;

    for (int i = 0; i < 3; ++i)
    {
        const float x = card.getX() + kCardPadding + static_cast<float>(i) * kCardColumnWidth;
        auto* slider = (i == 0) ? &frequencySlider_ : (i == 1) ? &gainSlider_ : &qSlider_;
        slider->setBounds(static_cast<int>(x), static_cast<int>(sliderY),
                          static_cast<int>(kCardColumnWidth), static_cast<int>(kCardSliderHeight));
    }

    // The type list covers the whole painted row of type buttons.
    const auto firstType = cardFilterTypeButtonBounds(card, 0);
    const auto lastType = cardFilterTypeButtonBounds(card, static_cast<int>(EqSettings::kMaxFilterType));
    typeBox_.setBounds(firstType.getUnion(lastType).getSmallestIntegerContainer());
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
        setEqFilterFrequencyAndGain(f, frequencySlider_.getValue(), f.gainDb);
        break;
    case 1: // Gain
        setEqFilterFrequencyAndGain(f, f.frequencyHz, gainSlider_.getValue());
        break;
    case 2: // Q
        adjustEqFilterQ(f, qSlider_.getValue() - f.q);
        break;
    }

    renderer_.setSettings(settings_);
    layoutCardControls();
    // Arrow keys change the knob many times a second: commit once they stop,
    // so a held key is one undo step and one render.
    if (keyboardAdjusting_)
        scheduleCommit();
    else
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
    auto bounds = getLocalBounds().toFloat();
    // 图谱边界从顶部栏下方开始，左右和底部保留3像素内边距
    const float left = bounds.getX() + 3.0f;
    const float right = bounds.getRight() - 3.0f;
    const float top = kTopBarHeight;
    const float bottom = bounds.getBottom() - 3.0f;
    renderer_.setGraphBounds({left, top, right - left, bottom - top});
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
    g.setColour(bgColor.withAlpha(0.85f));
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

    // 顶栏背景：覆盖 grid/axis 穿透，确保右上角按钮在最上层
    {
        const auto topBar = topBarBounds();
        g.setColour(bgColor);
        g.fillRect(topBar);
    }

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
            tooltipText = isMaximized_ ? LOC_RAW("Collapse to preview")
                                       : LOC_RAW("Expand EQ editor");
            break;
        case ButtonId::Bypass:
            tooltipText = settings_.active ? LOC_RAW("Bypass EQ")
                                           : LOC_RAW("Enable EQ");
            break;
        case ButtonId::Remove:
            tooltipText = LOC_RAW("Remove EQ processing");
            break;
        case ButtonId::Minimize:
            tooltipText = isPreview_ ? LOC_RAW("Hide EQ preview")
                                     : LOC_RAW("Minimize to preview");
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
            const auto font = UIColors::getUIFontExact(10.0f);
            const auto textW = juce::TextLayout::getStringWidth(font, tooltipText) + 8.0f;
            const float tooltipX = juce::jmax(2.0f, juce::jmin(btnRect.getX(),
                                                                static_cast<float>(getWidth()) - textW - 2.0f));
            g.fillRoundedRectangle(tooltipX, finalY, textW, tooltipH, 3.0f);
            g.setColour(EqGraphRenderer::hudTextColor());
            g.setFont(font);
            g.drawText(tooltipText, juce::Rectangle<float>(tooltipX, finalY, textW, tooltipH),
                       juce::Justification::centred, false);
        }
    }

    // 卡片按钮 tooltip（仅完整视图）
    if (!isPreview_ && hoveredCardButton_ >= 0 && cardBand_ >= 0
        && cardBand_ < static_cast<int>(settings_.filters.size()))
    {
        const auto cardBtn = static_cast<CardButton>(hoveredCardButton_);
        const auto card = floatingCardBounds();
        const auto btnRect = cardButtonBounds(cardBtn, card);
        juce::String tooltipText;
        if (cardBtn == CardButton::Bypass)
        {
            const bool bypassed = settings_.filters[cardBand_].bypassed;
            tooltipText = bypassed ? LOC_RAW("Enable this filter")
                                  : LOC_RAW("Bypass this filter");
        }
        else if (cardBtn == CardButton::Remove)
        {
            tooltipText = LOC_RAW("Delete this filter");
        }

        if (tooltipText.isNotEmpty())
        {
            const float tooltipH = 16.0f;
            const float tooltipY = btnRect.getBottom() + 3.0f;
            const float maxY = static_cast<float>(getHeight()) - tooltipH - 2.0f;
            const float finalY = juce::jmin(tooltipY, maxY);

            g.setColour(EqGraphRenderer::hudBgColor().withAlpha(0.92f));
            const auto font = UIColors::getUIFontExact(10.0f);
            const auto textW = juce::TextLayout::getStringWidth(font, tooltipText) + 8.0f;
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
        g.setFont(UIColors::getUIFontExact(11.0f));
        g.drawText(LOC_RAW("Remove EQ?"),
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
        g.setFont(UIColors::getUIFontExact(9.0f));
        g.drawText(LOC_RAW("Don't show again"), checkRect.translated(16.0f, 0.0f),
                   juce::Justification::centredLeft, false);

        const auto okRect = juce::Rectangle<float>(dialogX + 10.0f, dialogY + dialogH - 24.0f, 80.0f, 18.0f);
        g.setColour(EqGraphRenderer::combinedCurveColor().withAlpha(0.6f));
        g.fillRoundedRectangle(okRect, 3.0f);
        g.setColour(juce::Colours::black);
        g.setFont(UIColors::getUIFontExact(10.0f));
        g.drawText(LOC_RAW("Confirm"), okRect, juce::Justification::centred, false);

        const auto cancelRect = juce::Rectangle<float>(dialogX + dialogW - 90.0f, dialogY + dialogH - 24.0f, 80.0f, 18.0f);
        g.setColour(EqGraphRenderer::axisLabelColor().withAlpha(0.4f));
        g.fillRoundedRectangle(cancelRect, 3.0f);
        g.setColour(EqGraphRenderer::hudTextColor());
        g.drawText(LOC(kCancel), cancelRect, juce::Justification::centred, false);
    }

} // end of paint()

// ============================================================================
// 按钮绘制
// ============================================================================

void EqPopupComponent::paintButton(juce::Graphics& g, ButtonId id, juce::Rectangle<float> bounds, bool hovered) const
{
    if (bounds.isEmpty())
        return;
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

    if (isPreview_)
    {
        // 预览模式：4 按钮全在顶栏右上角
        const float startX = topBar.getRight() - 4.0f - kBtnSize;
        switch (id)
        {
        case ButtonId::Bypass:   return { startX, btnY, kBtnSize, kBtnSize };
        case ButtonId::Remove:   return { startX - (kBtnSize + kBtnGap), btnY, kBtnSize, kBtnSize };
        case ButtonId::Maximize: return { startX - (kBtnSize + kBtnGap) * 2, btnY, kBtnSize, kBtnSize };
        case ButtonId::Minimize: return { startX - (kBtnSize + kBtnGap) * 3, btnY, kBtnSize, kBtnSize };
        case ButtonId::None:     return {};
        }
    }
    else
    {
        // 完整视图：顶栏只有最大化/最小化
        const float startX = topBar.getRight() - 4.0f - kBtnSize;
        switch (id)
        {
        case ButtonId::Maximize: return { startX, btnY, kBtnSize, kBtnSize };
        case ButtonId::Minimize: return { startX - (kBtnSize + kBtnGap), btnY, kBtnSize, kBtnSize };
        case ButtonId::Bypass:   return {};
        case ButtonId::Remove:   return {};
        case ButtonId::None:     return {};
        }
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

// 卡片右上角按钮：旁通 + 删除（仅完整视图）
juce::Rectangle<float> EqPopupComponent::cardButtonBounds(
    CardButton btn, const juce::Rectangle<float>& cardBounds) const
{
    const float size = kCardTypeBtnSize;
    const float gap = kCardTypeBtnGap;
    const float x = cardBounds.getRight() - kCardPadding - size;
    const float y = cardBounds.getY() + kCardPadding;
    switch (btn)
    {
    case CardButton::Remove: return { x, y, size, size };
    case CardButton::Bypass: return { x - size - gap, y, size, size };
    case CardButton::None:   return {};
    }
    return {};
}

EqPopupComponent::CardButton EqPopupComponent::hitTestCardButton(juce::Point<float> pos) const
{
    if (isPreview_ || cardBand_ < 0)
        return CardButton::None;
    const auto card = floatingCardBounds();
    for (auto btn : { CardButton::Remove, CardButton::Bypass })
        if (cardButtonBounds(btn, card).contains(pos))
            return btn;
    return CardButton::None;
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
        const auto cardBtn = hitTestCardButton(pos);
        hoveredCardButton_ = static_cast<int>(cardBtn);
        const bool hasHover = hoveredTypeButton_ >= 0 || cardBtn != CardButton::None;
        setMouseCursor(hasHover ? juce::MouseCursor::PointingHandCursor
                                : juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

    hoveredBand_ = renderer_.hitTestAnchor(pos, 10.0f);
    hoveredCardButton_ = -1;
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

    flushPendingCommit();
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
                dontShowToggle_.setToggleState(suppressRemoveConfirmation_, juce::dontSendNotification);
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
        // 卡片右上角按钮：旁通 / 删除
        const auto cardBtn = hitTestCardButton(pos);
        if (cardBtn != CardButton::None && cardBand_ >= 0
            && cardBand_ < static_cast<int>(settings_.filters.size()))
        {
            if (cardBtn == CardButton::Bypass)
            {
                toggleEqFilterBypass(settings_.filters[cardBand_]);
                renderer_.setSettings(settings_);
                commitSettings();
                repaint();
            }
            else if (cardBtn == CardButton::Remove)
            {
                removeFilterAt(cardBand_);
            }
            return;
        }

        // 命中滤波器类型按钮 → 切换当前滤波器类型
        const int typeIdx = hitTestFilterTypeButton(pos);
        if (typeIdx >= 0 && typeIdx < 5 && cardBand_ >= 0
            && cardBand_ < static_cast<int>(settings_.filters.size()))
        {
            setFilterType(cardBand_, static_cast<EqFilterType>(typeIdx));
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
                toggleEqActive(settings_);
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
                    showRemoveConfirmation();
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
        settings_.filters.push_back(makeEqFilter(renderer_.xToFreq(pos.x), renderer_.yToGain(pos.y)));
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
    adjustEqFilterQ(settings_.filters[targetBand], wheel.deltaY > 0 ? step : -step);

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
    hoveredCardButton_ = -1;
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

    if (commitPending_ && juce::Time::getMillisecondCounterHiRes() >= commitDeadlineMs_)
        flushPendingCommit();

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
    if (settings_.removeFilter(index))
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
    confirmPanel_.setVisible(false);
    updateCardState();
    repaint();
}

void EqPopupComponent::showRemoveConfirmation()
{
    showingRemoveConfirmation_ = true;
    hideCardControls();
    dontShowToggle_.setToggleState(suppressRemoveConfirmation_, juce::dontSendNotification);
    // Same rectangles as the painted dialog in paint() and the clicks in mouseDown().
    const auto dialog = removeDialogBounds();
    confirmPanel_.setBounds(dialog.getSmallestIntegerContainer());
    const float w = dialog.getWidth();
    const float h = dialog.getHeight();
    dontShowToggle_.setBounds(juce::Rectangle<float>(10.0f, 28.0f, w - 20.0f, 14.0f).getSmallestIntegerContainer());
    confirmButton_.setBounds(juce::Rectangle<float>(10.0f, h - 24.0f, 80.0f, 18.0f).getSmallestIntegerContainer());
    cancelButton_.setBounds(juce::Rectangle<float>(w - 90.0f, h - 24.0f, 80.0f, 18.0f).getSmallestIntegerContainer());
    confirmPanel_.setVisible(true);
    repaint();
}

juce::Rectangle<float> EqPopupComponent::removeDialogBounds() const
{
    const auto area = getLocalBounds().toFloat();
    const float dialogW = 200.0f;
    const float dialogH = 70.0f;
    return { (area.getWidth() - dialogW) * 0.5f, (area.getHeight() - dialogH) * 0.5f, dialogW, dialogH };
}

void EqPopupComponent::setFilterType(int filterIndex, EqFilterType newType)
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return;

    auto& f = settings_.filters[filterIndex];
    if (f.type == newType)
        return;

    setEqFilterType(f, newType);
    renderer_.setSettings(settings_);
    configureCardControls(filterIndex);
    commitSettings();
    repaint();
}

// ============================================================================
// Accessibility: keyboard model
// ============================================================================

namespace {

// Keys with Ctrl or Alt that the window does not use go on to the host (Alt+F4, Ctrl+S).
// Navigation and edit keys stay here: in the piano roll they would change the notes
// behind the window, and undo would leave the window showing stale values.
bool passesToHost(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (!mods.isCtrlDown() && !mods.isAltDown() && !mods.isCommandDown())
        return false;

    const int code = key.getKeyCode();
    for (int kept : { juce::KeyPress::upKey, juce::KeyPress::downKey, juce::KeyPress::leftKey,
                      juce::KeyPress::rightKey, juce::KeyPress::homeKey, juce::KeyPress::endKey,
                      juce::KeyPress::pageUpKey, juce::KeyPress::pageDownKey, juce::KeyPress::deleteKey,
                      juce::KeyPress::insertKey, juce::KeyPress::returnKey, juce::KeyPress::backspaceKey,
                      juce::KeyPress::tabKey })
        if (code == kept)
            return false;

    if (mods.isCtrlDown() || mods.isCommandDown())
    {
        const auto letter = juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(code));
        for (juce::juce_wchar kept : { 'Z', 'Y', 'A', 'C', 'V', 'X', 'D' })
            if (letter == kept)
                return false;
    }
    return true;
}

bool isPlainKey(const juce::KeyPress& key, int code)
{
    return key.getKeyCode() == code && !key.getModifiers().isAnyModifierKeyDown();
}

} // namespace

juce::String EqPopupComponent::filterTypeName(EqFilterType type)
{
    switch (type)
    {
    case EqFilterType::LowCut:    return LOC_RAW("Low cut");
    case EqFilterType::LowShelf:  return LOC_RAW("Low shelf");
    case EqFilterType::Peak:      return LOC_RAW("Peak");
    case EqFilterType::HighShelf: return LOC_RAW("High shelf");
    case EqFilterType::HighCut:   return LOC_RAW("High cut");
    }
    return {};
}

std::unique_ptr<juce::AccessibilityHandler> EqPopupComponent::createAccessibilityHandler()
{
    // The title is computed on request, so NVDA+Up Arrow reads the current band and row.
    class EqAccessibilityHandler final : public juce::AccessibilityHandler {
    public:
        explicit EqAccessibilityHandler(EqPopupComponent& owner)
            : juce::AccessibilityHandler(owner, juce::AccessibilityRole::list), owner_(owner) {}

        juce::String getTitle() const override
        {
            juce::String title = "EQ";
            if (!owner_.settings_.active)
                title << ", " << LOC_RAW("bypassed");
            const auto line = owner_.describeBandAndRow();
            if (line.isNotEmpty())
                title << ", " << line;
            return title;
        }

    private:
        EqPopupComponent& owner_;
    };

    return std::make_unique<EqAccessibilityHandler>(*this);
}

void EqPopupComponent::startKeyboardEditing()
{
    keyRow_ = KeyRow::Type;
    ensureKeyBand();
    focusGrid();
}

int EqPopupComponent::keyBand() const
{
    const int count = static_cast<int>(settings_.filters.size());
    if (count == 0)
        return -1;
    return juce::jlimit(0, count - 1, selectedFilterIndex_);
}

void EqPopupComponent::ensureKeyBand()
{
    // The small preview has no band card; the keyboard always works in the full editor.
    if (isPreview_)
        toggleMaximize();
    if (showingRemoveConfirmation_)
        return;

    const int band = keyBand();
    if (band >= 0 && (selectedFilterIndex_ != band || cardBand_ != band))
    {
        selectedFilterIndex_ = band;
        updateCardState();
        repaint();
    }
}

void EqPopupComponent::selectKeyBand(int index)
{
    selectedFilterIndex_ = index;
    updateCardState();
    repaint();
    a11y::announce(*this, describeBandAndRow());
}

void EqPopupComponent::focusGrid()
{
    // Grabbing focus that is already here makes NVDA repeat the whole title.
    if (!hasKeyboardFocus(false))
        grabKeyboardFocus();
}

juce::String EqPopupComponent::describeRow(KeyRow row) const
{
    const int band = keyBand();
    if (band < 0)
        return {};

    const auto& f = settings_.filters[static_cast<size_t>(band)];
    const auto onOff = [](bool on) { return on ? LOC_RAW("On") : LOC(kOff); };
    switch (row)
    {
    case KeyRow::Type:
        return LOC_RAW("Type") + ", " + filterTypeName(f.type);
    case KeyRow::Frequency:
        return LOC_RAW("Frequency") + ", " + juce::String(juce::roundToInt(f.frequencyHz)) + " Hz";
    case KeyRow::Gain:
    {
        if (f.type == EqFilterType::LowCut || f.type == EqFilterType::HighCut)
            return Loc::format(LOC_RAW("Gain, not used by {0}"), filterTypeName(f.type));
        // Round first so a tiny negative value is not read as "minus 0.0".
        float gain = std::round(f.gainDb * 10.0f) / 10.0f;
        if (std::abs(gain) < 0.05f)
            gain = 0.0f;
        const juce::String number = juce::String(gain > 0.0f ? "+" : "") + juce::String(gain, 1);
        return LOC_RAW("Gain") + ", " + Loc::format(LOC_RAW("{0} dB"), number);
    }
    case KeyRow::Q:
        return "Q, " + juce::String(f.q, 2);
    case KeyRow::BypassBand:
        return LOC_RAW("Bypass band") + ", " + onOff(f.bypassed);
    case KeyRow::RemoveBand:
        return LOC_RAW("Remove band");
    case KeyRow::AddBand:
        return LOC_RAW("Add band");
    case KeyRow::BypassEq:
        return LOC_RAW("Bypass EQ") + ", " + onOff(!settings_.active);
    case KeyRow::RemoveEq:
        return LOC_RAW("Remove EQ");
    case KeyRow::Count:
        break;
    }
    return {};
}

juce::String EqPopupComponent::describeAllBands() const
{
    juce::String text = Loc::format(LOC_RAW("Bands: {0}"), juce::String(static_cast<int>(settings_.filters.size())));
    if (!settings_.active)
        text << ", " << LOC_RAW("EQ bypassed");
    for (size_t i = 0; i < settings_.filters.size(); ++i)
    {
        const auto& f = settings_.filters[i];
        text << ". " << juce::String(static_cast<int>(i) + 1) << ": " << filterTypeName(f.type)
             << ", " << juce::String(juce::roundToInt(f.frequencyHz)) << " Hz";
        if (f.type != EqFilterType::LowCut && f.type != EqFilterType::HighCut)
        {
            const float gain = std::abs(f.gainDb) < 0.05f ? 0.0f : f.gainDb;
            text << ", " << Loc::format(LOC_RAW("{0} dB"), juce::String(gain > 0.0f ? "+" : "") + juce::String(gain, 1));
        }
        text << ", Q " << juce::String(f.q, 2);
        if (f.bypassed)
            text << ", " << LOC_RAW("bypassed");
    }
    return text;
}

juce::String EqPopupComponent::describeBandAndRow() const
{
    const int band = keyBand();
    if (band < 0)
        return {};

    const auto& f = settings_.filters[static_cast<size_t>(band)];
    juce::String text = Loc::format(LOC_RAW("Band {0} of {1}"), juce::String(band + 1),
                                    juce::String(static_cast<int>(settings_.filters.size())));
    // The Type row says the type itself.
    if (keyRow_ != KeyRow::Type)
        text << ", " << filterTypeName(f.type);
    if (f.bypassed)
        text << ", " << LOC_RAW("bypassed");
    text << ", " << describeRow(keyRow_);
    return text;
}

bool EqPopupComponent::keyPressed(const juce::KeyPress& key)
{
    flushPendingCommit();
    if (passesToHost(key))
        return false;
    // While the confirmation is open its buttons own the keys.
    if (showingRemoveConfirmation_)
        return true;

    const int code = key.getKeyCode();
    const auto mods = key.getModifiers();
    const bool isButtonRow = keyRow_ >= KeyRow::BypassBand;

    if (code == juce::KeyPress::escapeKey)
    {
        requestClose();
        return true;
    }
    if (code == juce::KeyPress::spaceKey)
    {
        if (isButtonRow && !mods.isAnyModifierKeyDown())
        {
            activateKeyRow();
            return true;
        }
        return false; // play/stop in the piano roll, to hear the result
    }

    ensureKeyBand();
    const int count = static_cast<int>(settings_.filters.size());
    const int band = keyBand();

    if (isPlainKey(key, juce::KeyPress::leftKey) || isPlainKey(key, juce::KeyPress::rightKey)
        || isPlainKey(key, juce::KeyPress::homeKey) || isPlainKey(key, juce::KeyPress::endKey))
    {
        const int target = code == juce::KeyPress::leftKey  ? band - 1
                         : code == juce::KeyPress::rightKey ? band + 1
                         : code == juce::KeyPress::homeKey  ? 0
                                                            : count - 1;
        // At the edge nothing changes and nothing is said.
        if (target >= 0 && target < count && target != band)
            selectKeyBand(target);
        return true;
    }
    if (isPlainKey(key, juce::KeyPress::upKey) || isPlainKey(key, juce::KeyPress::downKey))
    {
        const int row = static_cast<int>(keyRow_) + (code == juce::KeyPress::upKey ? -1 : 1);
        if (row >= 0 && row < static_cast<int>(KeyRow::Count))
        {
            keyRow_ = static_cast<KeyRow>(row);
            a11y::announce(*this, describeRow(keyRow_));
        }
        return true;
    }
    if (code == juce::KeyPress::tabKey && !mods.isShiftDown())
    {
        enterKeyRow();
        return true;
    }
    if (isPlainKey(key, juce::KeyPress::returnKey))
    {
        if (isButtonRow)
            activateKeyRow();
        else
            enterKeyRow();
        return true;
    }
    if (isPlainKey(key, juce::KeyPress::insertKey))
    {
        addBandFromKeyboard();
        return true;
    }
    if (isPlainKey(key, juce::KeyPress::deleteKey))
    {
        removeBandFromKeyboard();
        return true;
    }
    // I: all bands at once, like I on a note reads all its details.
    if ((code == 'I' || code == 'i') && !mods.isAnyModifierKeyDown())
    {
        a11y::announce(*this, describeAllBands());
        return true;
    }
    // Everything else stays in the window, so it cannot edit the notes behind it.
    return true;
}

void EqPopupComponent::enterKeyRow()
{
    ensureKeyBand();
    switch (keyRow_)
    {
    case KeyRow::Type:
        typeBox_.grabKeyboardFocus();
        break;
    case KeyRow::Frequency:
        frequencySlider_.grabKeyboardFocus();
        break;
    case KeyRow::Gain:
        if (gainSlider_.isEnabled())
            gainSlider_.grabKeyboardFocus();
        else
            a11y::announce(*this, describeRow(KeyRow::Gain));
        break;
    case KeyRow::Q:
        qSlider_.grabKeyboardFocus();
        break;
    default:
        break; // buttons have nothing to adjust; Enter or Space presses them
    }
}

void EqPopupComponent::activateKeyRow()
{
    ensureKeyBand();
    const int band = keyBand();
    if (band < 0)
        return;

    switch (keyRow_)
    {
    case KeyRow::BypassBand:
    {
        toggleEqFilterBypass(settings_.filters[static_cast<size_t>(band)]);
        renderer_.setSettings(settings_);
        commitSettings();
        repaint();
        a11y::announce(*this, describeRow(keyRow_));
        break;
    }
    case KeyRow::RemoveBand:
        removeBandFromKeyboard();
        break;
    case KeyRow::AddBand:
        addBandFromKeyboard();
        break;
    case KeyRow::BypassEq:
        toggleEqActive(settings_);
        renderer_.setSettings(settings_);
        commitSettings();
        repaint();
        a11y::announce(*this, describeRow(keyRow_));
        break;
    case KeyRow::RemoveEq:
        if (suppressRemoveConfirmation_)
        {
            requestRemoveEq();
        }
        else
        {
            showRemoveConfirmation();
            confirmButton_.grabKeyboardFocus();
        }
        break;
    default:
        break;
    }
}

void EqPopupComponent::addBandFromKeyboard()
{
    if (static_cast<int>(settings_.filters.size()) >= EqSettings::kMaxFilters)
    {
        a11y::announce(*this, Loc::format(LOC_RAW("Maximum {0} bands"), juce::String(EqSettings::kMaxFilters)));
        return;
    }

    // Same as a double click in the middle of the graph: a Peak at 1000 Hz, 0 dB, Q 1.
    settings_.filters.push_back(makeEqFilter(1000.0, 0.0));
    EqGraphRenderer::assignPaletteSlots(settings_);
    selectedFilterIndex_ = static_cast<int>(settings_.filters.size()) - 1;
    keyRow_ = KeyRow::Type;

    renderer_.setSettings(settings_);
    commitSettings();
    updateCardState();
    repaint();
    a11y::announce(*this, Loc::format(LOC_RAW("Band {0} added"), juce::String(selectedFilterIndex_ + 1))
                              + ", " + describeRow(keyRow_));
}

void EqPopupComponent::removeBandFromKeyboard()
{
    ensureKeyBand();
    const int band = keyBand();
    if (band < 0)
        return;

    // As with the mouse, removing the last band removes the whole EQ.
    if (settings_.filters.size() <= 1)
    {
        requestRemoveEq();
        return;
    }

    if (removeFilterAt(band))
        return;
    selectedFilterIndex_ = juce::jmin(band, static_cast<int>(settings_.filters.size()) - 1);
    updateCardState();
    repaint();
    a11y::announce(*this, Loc::format(LOC_RAW("Band {0} removed"), juce::String(band + 1))
                              + ", " + describeBandAndRow());
}

void EqPopupComponent::requestClose()
{
    flushPendingCommit();
    // The owner deletes this window, so not from inside its own key handler.
    juce::Component::SafePointer<EqPopupComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis] {
        if (safeThis != nullptr && safeThis->onClose)
            safeThis->onClose();
    });
}

void EqPopupComponent::requestRemoveEq()
{
    flushPendingCommit();
    juce::Component::SafePointer<EqPopupComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis] {
        if (safeThis != nullptr && safeThis->onRemoveEq)
            safeThis->onRemoveEq();
    });
}

double EqPopupComponent::steppedKnobValue(const EqKnob& knob, const juce::KeyPress& key) const
{
    const int code = key.getKeyCode();
    const bool fine = key.getModifiers().isAltDown();
    const double value = knob.getValue();
    const double lo = knob.getMinimum();
    const double hi = knob.getMaximum();

    int direction = 0;
    bool big = false;
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
        direction = 1;
    else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
        direction = -1;
    else if (code == juce::KeyPress::pageUpKey)
        direction = 1, big = true;
    else if (code == juce::KeyPress::pageDownKey)
        direction = -1, big = true;
    // Home is the top and End the bottom, as the user expects from REAPER faders.
    else if (code == juce::KeyPress::homeKey)
        return hi;
    else if (code == juce::KeyPress::endKey)
        return lo;
    else
        return value;

    double next = value;
    if (&knob == &frequencySlider_)
    {
        // A semitone sounds like the same step at any frequency; Alt is 1 Hz, Page keys an octave.
        next = fine ? value + direction
                    : std::round(value * std::pow(2.0, direction * (big ? 1.0 : 1.0 / 12.0)));
        if (next == value)
            next = value + direction;
    }
    else if (&knob == &gainSlider_)
    {
        next = value + direction * (big ? 3.0 : fine ? 0.1 : 0.5);
    }
    else
    {
        next = value + direction * (big ? 1.0 : fine ? 0.01 : 0.1);
    }
    return juce::jlimit(lo, hi, next);
}

bool EqPopupComponent::handleKnobKey(EqKnob& knob, const juce::KeyPress& key)
{
    if (passesToHost(key))
        return false;

    const int code = key.getKeyCode();
    const auto mods = key.getModifiers();

    // The key that closed the number field bubbles up here as well; it is done already.
    if (editorJustClosed_)
        return true;

    juce::Label* valueBox = nullptr;
    for (auto* child : knob.getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child))
            valueBox = label;

    if (valueBox != nullptr && valueBox->isBeingEdited())
    {
        // The field takes the other keys itself. Tab keeps the number and closes the
        // circle grid -> knob -> field -> grid; Shift+Tab keeps it and goes back to the knob;
        // Esc drops it.
        if (code == juce::KeyPress::tabKey)
        {
            gridAfterEditor_ = !mods.isShiftDown();
            valueBox->hideEditor(false);
        }
        else if (code == juce::KeyPress::escapeKey)
            valueBox->hideEditor(true);
        return true;
    }

    if (isPlainKey(key, juce::KeyPress::spaceKey))
        return false; // play/stop

    if (code == juce::KeyPress::escapeKey || (code == juce::KeyPress::tabKey && mods.isShiftDown()))
    {
        flushPendingCommit();
        focusGrid();
        return true;
    }

    if (code == juce::KeyPress::tabKey || code == juce::KeyPress::returnKey)
    {
        flushPendingCommit();
        if (valueBox != nullptr)
        {
            juce::Component::SafePointer<EqPopupComponent> safeThis(this);
            juce::Component::SafePointer<EqKnob> safeKnob(&knob);
            valueBox->onEditorHide = [safeThis, safeKnob] {
                if (safeThis == nullptr)
                    return;
                safeThis->editorJustClosed_ = true;
                juce::MessageManager::callAsync([safeThis, safeKnob] {
                    if (safeThis == nullptr)
                        return;
                    safeThis->editorJustClosed_ = false;
                    const bool toGrid = std::exchange(safeThis->gridAfterEditor_, false);
                    if (safeKnob == nullptr || !safeKnob->isShowing())
                        return;
                    // Back to the knob for more arrows (or to the grid after Tab),
                    // unless a click took focus elsewhere.
                    auto* focused = juce::Component::getCurrentlyFocusedComponent();
                    const bool focusIsFree = focused == nullptr || focused == safeKnob.getComponent()
                                          || safeKnob->isParentOf(focused);
                    if (!focusIsFree)
                        return;
                    if (toGrid)
                        safeThis->focusGrid();
                    else if (!safeKnob->hasKeyboardFocus(false))
                        safeKnob->grabKeyboardFocus();
                });
            };
        }
        knob.showTextBox();
        return true;
    }

    // Backspace: the default, the same value as a double click (1000 Hz, 0 dB, Q 1).
    double next = isPlainKey(key, juce::KeyPress::backspaceKey) && knob.isDoubleClickReturnEnabled()
                      ? juce::jlimit(knob.getMinimum(), knob.getMaximum(), knob.getDoubleClickReturnValue())
                      : steppedKnobValue(knob, key);
    // At the edge the value does not change and NVDA says nothing.
    if (next != knob.getValue())
    {
        keyboardAdjusting_ = true;
        // NVDA speaks the new value of the focused slider itself.
        knob.setValue(next, juce::sendNotificationSync);
        keyboardAdjusting_ = false;
    }
    return true;
}

bool EqPopupComponent::handleTypeKey(const juce::KeyPress& key)
{
    if (passesToHost(key))
        return false;

    const int code = key.getKeyCode();
    if (isPlainKey(key, juce::KeyPress::spaceKey))
        return false; // play/stop

    // The type has no number to type in, so Tab goes back to the grid like Esc.
    if (code == juce::KeyPress::escapeKey || code == juce::KeyPress::tabKey || code == juce::KeyPress::returnKey)
    {
        focusGrid();
        return true;
    }

    // Arrows, Home and End choose the type (NVDA speaks it). Alt+Down would open the
    // drop-down menu, so keys with modifiers are only eaten.
    if (!key.getModifiers().isAnyModifierKeyDown())
        typeBox_.listKeyPressed(key);
    return true;
}

bool EqPopupComponent::handleConfirmKey(const juce::KeyPress& key)
{
    if (passesToHost(key))
        return false;

    const int code = key.getKeyCode();
    if (code == juce::KeyPress::escapeKey)
    {
        cancelButton_.onClick();
        return true;
    }

    juce::Button* const order[] = { &confirmButton_, &cancelButton_, &dontShowToggle_ };
    int current = 0;
    for (int i = 0; i < 3; ++i)
        if (order[i]->hasKeyboardFocus(false))
            current = i;

    // Tab and Shift+Tab go round the three controls of the dialog only.
    if (code == juce::KeyPress::tabKey)
    {
        order[(current + (key.getModifiers().isShiftDown() ? 2 : 1)) % 3]->grabKeyboardFocus();
        return true;
    }
    if (code == juce::KeyPress::spaceKey || code == juce::KeyPress::returnKey)
    {
        order[current]->triggerClick();
        return true;
    }
    return true;
}

void EqPopupComponent::scheduleCommit()
{
    commitPending_ = true;
    commitDeadlineMs_ = juce::Time::getMillisecondCounterHiRes() + 400.0;
}

void EqPopupComponent::flushPendingCommit()
{
    if (!commitPending_)
        return;
    commitPending_ = false;
    commitSettings();
}

} // namespace OpenTune

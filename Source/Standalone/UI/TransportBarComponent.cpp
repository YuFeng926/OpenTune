#include "TransportBarComponent.h"
#include "OpenTuneLookAndFeel.h"
#include "UIColors.h"
#include "ToolbarIcons.h"
#include "../../Utils/LocalizationManager.h"
#include <cmath>

namespace OpenTune {

namespace {

constexpr const char* kOverdoseUiRoleKey = "overdoseUiRole";
constexpr const char* kOverdoseUiRoleTransport = "transport";
constexpr const char* kOverdoseUiRoleSegment = "segment";
constexpr float kAuroraToolbarChromeIntensity = 0.42f;

constexpr int kValidDenominators[] = { 1, 2, 4, 8, 16, 32, 64 };

} // namespace

DigitalTimeDisplay::DigitalTimeDisplay()
{
    setInterceptsMouseClicks(false, false);
}

void DigitalTimeDisplay::setTimeString(const juce::String& time)
{
    if (timeString_ != time || isBarsMode_)
    {
        timeString_ = time;
        isBarsMode_ = false;
        repaint();
    }
}

void DigitalTimeDisplay::setBarsString(int bar, int beat)
{
    juce::String barsText = juce::String(bar) + "." + juce::String(beat);
    if (timeString_ != barsText || !isBarsMode_)
    {
        bar_ = bar;
        beat_ = beat;
        isBarsMode_ = true;
        timeString_ = barsText;
        repaint();
    }
}

void DigitalTimeDisplay::paint(juce::Graphics& g)
{
    const auto& style = UIColors::currentThemeStyle();
    auto bounds = getLocalBounds().toFloat().reduced(4.0f);
    const float yOffset = juce::jlimit(0.0f, 2.0f, bounds.getHeight() * 0.03f);
    bounds = bounds.translated(0.0f, yOffset);
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    if (UIColors::currentThemeId() == ThemeId::Overdose)
        UIColors::drawOverdoseDisplayWell(g, bounds.reduced(3.0f, 2.0f), style.fieldRadius);

    if (!style.timeSegmentStyle)
    {
        // Overdose：深紫底 + 粉数字
        if (UIColors::currentThemeId() == ThemeId::Overdose)
            g.setColour(juce::Colour(Overdose::Colors::PrimaryPink));
        else
            g.setColour(UIColors::textPrimary);
        g.setFont(UIColors::getMonoFont(UIColors::navMonoFontHeight));
        g.drawFittedText(timeString_, getLocalBounds().reduced(6, 0), juce::Justification::centred, 1, 1.0f);
        return;
    }

    float totalWeight = 0.0f;
    for (int i = 0; i < timeString_.length(); ++i)
    {
        auto c = timeString_[i];
        totalWeight += (c == ':' || c == '.' || c == ' ') ? 0.50f : 1.00f;
    }
    if (totalWeight <= 0.0f)
        totalWeight = 1.0f;

    const float baseCharW = bounds.getWidth() / totalWeight;
    float x = bounds.getX();

    for (int i = 0; i < timeString_.length(); ++i)
    {
        auto c = timeString_[i];
        const float w = (c == ':' || c == '.' || c == ' ') ? baseCharW * 0.50f : baseCharW;
        drawChar(g, c, { x, bounds.getY(), w, bounds.getHeight() });
        x += w;
    }
}

void DigitalTimeDisplay::drawChar(juce::Graphics& g, juce::juce_wchar c, juce::Rectangle<float> area)
{
    const auto& style = UIColors::currentThemeStyle();
    if (c == ':')
    {
        const float dotSize = area.getWidth() * 0.55f;
        const float cx = area.getCentreX();

        if (UIColors::currentThemeId() == ThemeId::Overdose)
        {
             g.setColour(UIColors::displayText);
        }
        else if (UIColors::currentThemeId() == ThemeId::BlueBreeze)
        {
             g.setColour(juce::Colour { BlueBreeze::Colors::DisplayText });
        }
        else
        {
             g.setColour(style.timeActive);
        }

        g.fillEllipse(cx - dotSize * 0.5f, area.getCentreY() - area.getHeight() * 0.22f - dotSize * 0.5f, dotSize, dotSize);
        g.fillEllipse(cx - dotSize * 0.5f, area.getCentreY() + area.getHeight() * 0.22f - dotSize * 0.5f, dotSize, dotSize);
        return;
    }

    if (c == '.')
    {
        const float dotSize = area.getWidth() * 0.75f;

        if (UIColors::currentThemeId() == ThemeId::Overdose)
        {
             g.setColour(UIColors::displayText);
        }
        else if (UIColors::currentThemeId() == ThemeId::BlueBreeze)
        {
             g.setColour(juce::Colour { BlueBreeze::Colors::DisplayText });
        }
        else
        {
             g.setColour(style.timeActive);
        }

        g.fillEllipse(area.getCentreX() - dotSize * 0.5f, area.getBottom() - dotSize * 1.45f, dotSize, dotSize);
        return;
    }

    if (c == ' ')
        return;

    bool seg[7] = { false, false, false, false, false, false, false };
    switch (static_cast<char>(c))
    {
        case '0': seg[0]=seg[1]=seg[2]=seg[3]=seg[4]=seg[5]=true; break;
        case '1': seg[1]=seg[2]=true; break;
        case '2': seg[0]=seg[1]=seg[6]=seg[4]=seg[3]=true; break;
        case '3': seg[0]=seg[1]=seg[6]=seg[2]=seg[3]=true; break;
        case '4': seg[5]=seg[6]=seg[1]=seg[2]=true; break;
        case '5': seg[0]=seg[5]=seg[6]=seg[2]=seg[3]=true; break;
        case '6': seg[0]=seg[5]=seg[4]=seg[3]=seg[2]=seg[6]=true; break;
        case '7': seg[0]=seg[1]=seg[2]=true; break;
        case '8': seg[0]=seg[1]=seg[2]=seg[3]=seg[4]=seg[5]=seg[6]=true; break;
        case '9': seg[0]=seg[1]=seg[2]=seg[3]=seg[5]=seg[6]=true; break;
        default: break;
    }

    auto digitArea = area.reduced(area.getWidth() * 0.12f, area.getHeight() * 0.08f);
    for (int i = 0; i < 7; ++i)
        drawSegment(g, i, digitArea, seg[i]);
}

void DigitalTimeDisplay::drawSegment(juce::Graphics& g, int segment, juce::Rectangle<float> area, bool active)
{
    const auto& style = UIColors::currentThemeStyle();

    if (UIColors::currentThemeId() == ThemeId::Overdose)
    {
        g.setColour(active ? UIColors::displayText : UIColors::displayTextDim);
    }
    else if (UIColors::currentThemeId() == ThemeId::BlueBreeze)
    {
        g.setColour(active ? juce::Colour { BlueBreeze::Colors::DisplayText }
                           : juce::Colour { BlueBreeze::Colors::DisplayTextDim });
    }
    else
    {
        g.setColour(active ? style.timeActive : style.timeInactive);
    }

    const float t = juce::jlimit(1.5f, 10.0f, area.getWidth() * 0.19f);
    const float x = area.getX();
    const float y = area.getY();
    const float w = area.getWidth();
    const float h = area.getHeight();

    juce::Rectangle<float> r;
    switch (segment)
    {
        case 0: r = { x + t,         y,               w - 2.0f*t, t }; break;
        case 1: r = { x + w - t,     y + t,           t,          h * 0.5f - 1.5f*t }; break;
        case 2: r = { x + w - t,     y + h * 0.5f + 0.5f*t, t,    h * 0.5f - 1.5f*t }; break;
        case 3: r = { x + t,         y + h - t,       w - 2.0f*t, t }; break;
        case 4: r = { x,             y + h * 0.5f + 0.5f*t, t,    h * 0.5f - 1.5f*t }; break;
        case 5: r = { x,             y + t,           t,          h * 0.5f - 1.5f*t }; break;
        case 6: r = { x + t,         y + h * 0.5f - 0.5f*t, w - 2.0f*t, t }; break;
    }

    g.fillRoundedRectangle(r, t * 0.40f);
}

BpmValueField::BpmValueField()
{
    setWantsKeyboardFocus(true);
    startTimerHz(3);
}

void BpmValueField::setValue(double value)
{
    text_ = juce::String(value, 1);
    caretIndex_ = text_.length();
    lastValidValue_ = value;
    isEditing_ = false;
    repaint();
}

double BpmValueField::getValue() const
{
    return text_.getDoubleValue();
}

void BpmValueField::setTimeSignature(int numerator, int denominator)
{
    timeSigNum_ = numerator;
    timeSigDenom_ = denominator;
    repaint();
}

void BpmValueField::setReadOnly(bool readOnly)
{
    if (readOnly_ == readOnly)
        return;

    readOnly_ = readOnly;

    if (readOnly_)
    {
        // End any active editing and release focus
        if (isEditing_)
        {
            isEditing_ = false;
            text_ = juce::String(lastValidValue_, 1);
            caretIndex_ = text_.length();
        }
        if (hasKeyboardFocus(true))
            giveAwayKeyboardFocus();
    }

    repaint();
}

BpmValueField::LayoutRects BpmValueField::calculateLayout() const
{
    auto content = getLocalBounds().reduced(4, 0);

    // 拍号区域：右侧固定紧凑分配 (denominator 18 + slash 6 + numerator 18 = 42px)
    auto denom = content.removeFromRight(18);
    auto slash = content.removeFromRight(6);
    auto num = content.removeFromRight(18);

    // BPM 与拍号之间保留 4px 分隔带，divider 落在该区域内
    content.removeFromRight(4);

    // BPM 使用剩余宽度（102 - 42 - 4 = 56px），水平居中
    auto bpmValue = content;

    return { bpmValue, num, slash, denom };
}

juce::String BpmValueField::formatBpmText() const
{
    if (std::floor(lastValidValue_) == lastValidValue_)
        return juce::String(static_cast<int>(lastValidValue_));
    return juce::String(lastValidValue_, 1);
}

void BpmValueField::paint(juce::Graphics& g)
{
    const auto& style = UIColors::currentThemeStyle();
    const auto themeId = UIColors::currentThemeId();
    auto bounds = getLocalBounds().toFloat().reduced(2.0f); // 与 UnifiedToolbarButton 缩进一致，视觉高度统一 36px
    const auto focused = isEditing_ && hasKeyboardFocus(true);

    if (themeId == ThemeId::Overdose)
    {
        UIColors::fillOverdosePanelBackground(g, bounds, style.fieldRadius);

        if (focused || isMouseOver())
        {
            g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(focused ? 0.38f : 0.18f));
            g.drawRoundedRectangle(bounds.reduced(0.75f), style.fieldRadius, focused ? 1.35f : 1.0f);
        }
    }
    else if (themeId == ThemeId::Aurora)
    {
        if (focused)
            UIColors::drawAuroraGlow(g, bounds, UIColors::correctedF0, 0.46f, 0.62f);

        UIColors::drawAuroraButtonChrome(g,
                                         bounds,
                                         style.fieldRadius,
                                         isMouseOver() || focused,
                                         false,
                                         focused,
                                         {},
                                         {},
                                         nullptr,
                                         kAuroraToolbarChromeIntensity);
    }
    else if (themeId == ThemeId::DarkBlueGrey)
    {
        juce::ColourGradient grad(UIColors::backgroundLight.brighter(0.06f), bounds.getX(), bounds.getY(),
                                  UIColors::backgroundLight.darker(0.08f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(bounds, style.fieldRadius);
        g.setColour(UIColors::textPrimary.withAlpha(0.10f));
        g.drawLine(bounds.getX() + style.fieldRadius, bounds.getY() + 1.0f,
                   bounds.getRight() - style.fieldRadius, bounds.getY() + 1.0f, style.strokeThin);
    }
    else
    {
        OpenTuneLookAndFeel::drawBlueBreezeSurface(g,
                                                   bounds,
                                                   style.fieldRadius,
                                                   isMouseOver() || focused,
                                                   false,
                                                   focused);
    }

    if (themeId != ThemeId::Aurora && themeId != ThemeId::BlueBreeze && themeId != ThemeId::Overdose)
    {
        g.setColour(focused ? UIColors::accent : UIColors::panelBorder);
        g.drawRoundedRectangle(bounds.reduced(0.5f), style.fieldRadius, focused ? style.focusRingThickness : style.strokeThin);
    }

    // Calculate layout rectangles (paint and hit-test use same logic)
    auto lr = calculateLayout();

    // Hover feedback is painted below text so labels remain crisp.
    if (!readOnly_ && isMouseOver())
    {
        const auto mouse = getMouseXYRelative();
        const juce::Rectangle<int>* hoverRect = nullptr;
        if (lr.bpmValue.contains(mouse))
            hoverRect = &lr.bpmValue;
        else if (lr.numerator.contains(mouse))
            hoverRect = &lr.numerator;
        else if (lr.denominator.contains(mouse))
            hoverRect = &lr.denominator;

        if (hoverRect != nullptr)
        {
            g.setColour(UIColors::textPrimary.withAlpha(0.12f));
            g.fillRect(hoverRect->toFloat());
        }
    }

    auto font = UIColors::getLabelFont(UIColors::navFontHeight);
    g.setFont(font);

    // Draw vertical divider line between BPM and time signature
    const int dividerX = lr.bpmValue.getRight() + 2;
    g.setColour(UIColors::textSecondary.withAlpha(0.25f));
    g.drawVerticalLine(dividerX, bounds.getY() + 8.0f, bounds.getBottom() - 8.0f);

    // BPM value - 水平居中
    g.setFont(font);
    g.setColour(readOnly_ ? UIColors::textSecondary : UIColors::textPrimary);
    juce::String bpmText = isEditing_ ? text_ : formatBpmText();
    g.drawFittedText(bpmText, lr.bpmValue, juce::Justification::centred, 1, 1.0f);

    // Time signature: draw each part separately for precise hit-test alignment
    g.setColour(readOnly_ ? UIColors::textSecondary.withAlpha(0.70f) : UIColors::textSecondary.withAlpha(0.85f));
    g.setFont(UIColors::getLabelFont(UIColors::navFontHeight - 4.0f));

    // Numerator
    g.drawFittedText(juce::String(timeSigNum_), lr.numerator, juce::Justification::centred, 1, 1.0f);
    // Slash
    g.drawFittedText("/", lr.slash, juce::Justification::centred, 1, 1.0f);
    // Denominator
    g.drawFittedText(juce::String(timeSigDenom_), lr.denominator, juce::Justification::centred, 1, 1.0f);

    // Caret for BPM editing - 与居中文本起点一致
    if (isEditing_ && showCaret_)
    {
        auto getTextWidth = [&](const juce::String& s) -> float {
            juce::GlyphArrangement ga;
            ga.addLineOfText(font, s, 0.0f, 0.0f);
            return ga.getBoundingBox(0, 0, true).getWidth();
        };
        const auto area = lr.bpmValue.toFloat();
        const auto textW = getTextWidth(text_);
        const auto cx = area.getCentreX() - textW * 0.5f;
        const auto caretX = cx + getTextWidth(text_.substring(0, caretIndex_));
        g.setColour(UIColors::textPrimary.withAlpha(0.8f));
        g.drawLine(caretX, static_cast<float>(bounds.getY()) + 7.0f, caretX, static_cast<float>(bounds.getBottom()) - 7.0f, style.strokeThick);
    }
}

void BpmValueField::mouseDown(const juce::MouseEvent& e)
{
    if (readOnly_)
        return;

    auto lr = calculateLayout();

    if (lr.bpmValue.contains(e.getPosition()))
    {
        grabKeyboardFocus();
        if (!isEditing_)
        {
            isEditing_ = true;
            text_.clear();
            caretIndex_ = 0;
        }
        repaint();
    }
    else if (lr.numerator.contains(e.getPosition()))
    {
        showNumeratorMenu();
    }
    else if (lr.denominator.contains(e.getPosition()))
    {
        showDenominatorMenu();
    }
}

void BpmValueField::showNumeratorMenu()
{
    juce::PopupMenu menu;

    for (int n = 1; n <= 64; ++n)
    {
        menu.addItem(n, juce::String(n), true, n == timeSigNum_);
    }

    juce::Component::SafePointer<BpmValueField> safeThis(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(calculateLayout().numerator)).withMaximumNumColumns(1).withItemThatMustBeVisible(timeSigNum_).withInitiallySelectedItem(timeSigNum_),
        [safeThis](int result) {
            if (safeThis != nullptr && result > 0)
            {
                safeThis->timeSigNum_ = result;
                if (safeThis->onTimeSignatureCommit)
                    safeThis->onTimeSignatureCommit(safeThis->timeSigNum_, safeThis->timeSigDenom_);
                safeThis->repaint();
            }
        });
}

void BpmValueField::showDenominatorMenu()
{
    juce::PopupMenu menu;

    for (int denom : kValidDenominators)
    {
        menu.addItem(denom, juce::String(denom), true, denom == timeSigDenom_);
    }

    // SafePointer for async lifetime
    juce::Component::SafePointer<BpmValueField> safeThis(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(calculateLayout().denominator)),
        [safeThis](int result) {
            if (safeThis != nullptr && result > 0)
            {
                safeThis->timeSigDenom_ = result;
                if (safeThis->onTimeSignatureCommit)
                    safeThis->onTimeSignatureCommit(safeThis->timeSigNum_, safeThis->timeSigDenom_);
                safeThis->repaint();
            }
        });
}

bool BpmValueField::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();

    if (code == juce::KeyPress::returnKey)
    {
        commit();
        giveAwayKeyboardFocus();
        return true;
    }

    if (code == juce::KeyPress::escapeKey)
    {
        cancelEdit();
        giveAwayKeyboardFocus();
        return true;
    }

    if (code == juce::KeyPress::backspaceKey)
    {
        if (caretIndex_ > 0 && text_.isNotEmpty())
        {
            text_ = text_.substring(0, caretIndex_ - 1) + text_.substring(caretIndex_);
            caretIndex_ = juce::jmax(0, caretIndex_ - 1);
            repaint();
        }
        return true;
    }

    if (code == juce::KeyPress::leftKey)
    {
        caretIndex_ = juce::jmax(0, caretIndex_ - 1);
        repaint();
        return true;
    }

    if (code == juce::KeyPress::rightKey)
    {
        caretIndex_ = juce::jmin(text_.length(), caretIndex_ + 1);
        repaint();
        return true;
    }

    auto ch = key.getTextCharacter();
    if (ch >= '0' && ch <= '9')
    {
        text_ = text_.substring(0, caretIndex_) + juce::String::charToString(ch) + text_.substring(caretIndex_);
        caretIndex_ = juce::jmin(text_.length(), caretIndex_ + 1);
        repaint();
        return true;
    }

    if (ch == '.')
    {
        // Allow one decimal point
        if (!text_.containsChar('.'))
        {
            text_ = text_.substring(0, caretIndex_) + "." + text_.substring(caretIndex_);
            caretIndex_ = juce::jmin(text_.length(), caretIndex_ + 1);
            repaint();
        }
        return true;
    }

    return false;
}

void BpmValueField::focusLost(juce::Component::FocusChangeType cause)
{
    juce::ignoreUnused(cause);
    commit();
}

void BpmValueField::timerCallback()
{
    if (isEditing_ && hasKeyboardFocus(true))
    {
        showCaret_ = !showCaret_;
        repaint();
    }
    else
    {
        if (showCaret_)
        {
            showCaret_ = false;
            repaint();
        }
    }
}

void BpmValueField::commit()
{
    isEditing_ = false;

    // Empty text: cancel edit, restore last valid value
    if (text_.isEmpty())
    {
        text_ = juce::String(lastValidValue_, 1);
        caretIndex_ = text_.length();
        repaint();
        return;
    }

    auto v = text_.getDoubleValue();

    // Update local state with raw value (no UI clamp)
    lastValidValue_ = v;
    text_ = juce::String(v, 1);
    caretIndex_ = text_.length();
    repaint();

    // Send raw value to Editor; processor handles canonical validation
    if (onCommit)
        onCommit(v);
}

void BpmValueField::cancelEdit()
{
    isEditing_ = false;
    text_ = juce::String(lastValidValue_, 1);
    caretIndex_ = text_.length();
    repaint();
}

// UnifiedToolbarButton implementation
UnifiedToolbarButton::UnifiedToolbarButton(const juce::String& name, juce::Path iconPath, juce::Path toggledIconPath)
    : juce::Button(name), iconPath_(iconPath), toggledIconPath_(toggledIconPath)
{
    setWantsKeyboardFocus(false);
}

void UnifiedToolbarButton::setIcon(juce::Path iconPath)
{
    iconPath_ = iconPath;
    repaint();
}

void UnifiedToolbarButton::setConnectedEdges(int edges)
{
    connectedEdges_ = edges;
    repaint();
}

void UnifiedToolbarButton::setSolidIcon(bool solid)
{
    solidIcon_ = solid;
    repaint();
}

void UnifiedToolbarButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    const auto themeId = UIColors::currentThemeId();
    float radius = (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? UIColors::currentThemeStyle().controlRadius : 6.0f;

    const bool roundTopLeft = ! (connectedEdges_ & Left);
    const bool roundBottomLeft = ! (connectedEdges_ & Left);
    const bool roundTopRight = ! (connectedEdges_ & Right);
    const bool roundBottomRight = ! (connectedEdges_ & Right);

    bool isToggled = getToggleState();
    bool isActive = isToggled || shouldDrawButtonAsDown;
    bool isHover = shouldDrawButtonAsHighlighted;
    const auto overdoseRole = getProperties()[kOverdoseUiRoleKey].toString();
    const bool isSegmentRole = overdoseRole == kOverdoseUiRoleSegment;

    auto createRoundedRectPath = [](juce::Rectangle<float> rect, float r,
                                    bool tl, bool tr, bool bl, bool br) -> juce::Path
    {
        juce::Path p;
        p.addRoundedRectangle(rect.getX(),
                              rect.getY(),
                              rect.getWidth(),
                              rect.getHeight(),
                              r, r,
                              tl, tr, bl, br);
        return p;
    };

    // 1. Background
    if (themeId == ThemeId::Overdose)
    {
        juce::Path p;
        if (connectedEdges_ == None)
        {
            p.addRoundedRectangle(bounds, radius);
        }
        else
        {
            p = createRoundedRectPath(bounds, radius,
                                      roundTopLeft, roundTopRight,
                                      roundBottomLeft, roundBottomRight);
        }

        // 普通按钮底色：低饱和紫灰渐变（参考图：#EDE9F3 → #C6C0DE → #AFA7CD，与托盘融合）
        const auto drawIdleFill = [&]()
        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(p);
            juce::ColourGradient idle(
                juce::Colour(0xFFEDE9F3),
                bounds.getX(), bounds.getY(),
                juce::Colour(0xFFAFA7CD),
                bounds.getX(), bounds.getBottom(),
                false);
            idle.addColour(0.45f, juce::Colour(0xFFC6C0DE));
            g.setGradientFill(idle);
            g.fillPath(p);

            // 顶部细高光（参考图：EDE9F3 亮顶）
            g.setColour(juce::Colour(0xFFFFFFFF).withAlpha(0.35f));
            g.drawLine(bounds.getX() + radius, bounds.getY() + 1.0f,
                       bounds.getRight() - radius, bounds.getY() + 1.0f, 1.0f);

            // 底部紫灰压暗（浮雕收边，柔和）
            auto bottomShade = bounds.withTrimmedTop(bounds.getHeight() * 0.62f);
            juce::ColourGradient bs(
                juce::Colours::transparentBlack,
                bottomShade.getCentreX(), bottomShade.getY(),
                juce::Colour(0xFF51406F).withAlpha(0.10f),
                bottomShade.getCentreX(), bottomShade.getBottom(), false);
            g.setGradientFill(bs);
            g.fillRect(bottomShade);
        };

        // 激活态：浅粉紫渐变（参考图：#F9E4F2 → #E7D7ED → #D2B3DB）+ 清晰粉紫描边
        const auto drawActiveFill = [&]()
        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(p);
            juce::ColourGradient activeFill(
                juce::Colour(0xFFF9E4F2),
                bounds.getX(), bounds.getY(),
                juce::Colour(0xFFD2B3DB),
                bounds.getX(), bounds.getBottom(),
                false);
            activeFill.addColour(0.45f, juce::Colour(0xFFE7D7ED));
            activeFill.addColour(0.78f, juce::Colour(0xFFDDC0E2));
            g.setGradientFill(activeFill);
            g.fillPath(p);

            // 顶部柔和高光（克制，以浅粉紫渐变为主）
            auto topHighlight = bounds.withHeight(bounds.getHeight() * 0.38f);
            juce::ColourGradient hl(
                juce::Colour(0xFFFFFFFF).withAlpha(0.28f),
                topHighlight.getCentreX(), topHighlight.getY(),
                juce::Colours::transparentWhite,
                topHighlight.getCentreX(), topHighlight.getBottom(),
                false);
            g.setGradientFill(hl);
            g.fillRect(topHighlight);

            // 底部微压暗（浮雕收边，柔和）
            auto bottomShade = bounds.withTrimmedTop(bounds.getHeight() * 0.60f);
            juce::ColourGradient bs(
                juce::Colours::transparentBlack,
                bottomShade.getCentreX(), bottomShade.getY(),
                juce::Colour(0xFF803090).withAlpha(0.10f),
                bottomShade.getCentreX(), bottomShade.getBottom(), false);
            g.setGradientFill(bs);
            g.fillRect(bottomShade);
        };

        if (isSegmentRole)
        {
            // 段按钮：普通态紫灰渐变，激活态浅粉紫渐变
            if (isActive)
                drawActiveFill();
            else
                drawIdleFill();
        }
        else
        {
            if (isActive)
                drawActiveFill();
            else
                drawIdleFill();
        }

        if (isHover && !isActive)
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(p);
            g.setColour(juce::Colour(Overdose::Colors::PanelHighlight).withAlpha(isSegmentRole ? 0.12f : 0.16f));
            g.fillRect(bounds);
        }

        // 粉紫描边：激活时清晰（参考图 #B951A7），普通时低透明
        {
            const float outlineAlpha = isActive ? (isSegmentRole ? 0.85f : 0.95f)
                                                : (isHover ? 0.45f : 0.28f);
            const float outlineWidth = isActive ? 1.3f : (isHover ? 1.0f : 0.8f);
            g.setColour(juce::Colour(0xFFB951A7).withAlpha(outlineAlpha));
            g.strokePath(p, juce::PathStrokeType(outlineWidth));
        }

        if (connectedEdges_ != None && !isSegmentRole)
        {
            g.setColour(juce::Colour(Overdose::Colors::PanelInsetShadow).withAlpha(0.20f));
            if (connectedEdges_ & Left)
                g.drawLine(bounds.getX(), bounds.getY() + 3.0f, bounds.getX(), bounds.getBottom() - 3.0f, 1.0f);
            if (connectedEdges_ & Right)
                g.drawLine(bounds.getRight(), bounds.getY() + 3.0f, bounds.getRight(), bounds.getBottom() - 3.0f, 1.0f);
        }
    }
    else if (themeId == ThemeId::BlueBreeze)
    {
        juce::Path p;
        if (connectedEdges_ == None)
        {
            p.addRoundedRectangle(bounds, radius);
        }
        else
        {
            p = createRoundedRectPath(bounds, radius,
                                      roundTopLeft, roundTopRight,
                                      roundBottomLeft, roundBottomRight);
        }

        OpenTuneLookAndFeel::drawBlueBreezeSurface(g,
                                                   bounds,
                                                   radius,
                                                   isHover,
                                                   shouldDrawButtonAsDown,
                                                   isActive,
                                                   &p);

        if (connectedEdges_ != None)
        {
            g.setColour(juce::Colour { BlueBreeze::Colors::PanelInset }.withAlpha(0.22f));
            if (connectedEdges_ & Left)
                g.drawLine(bounds.getX(), bounds.getY() + 3.0f, bounds.getX(), bounds.getBottom() - 3.0f, 1.0f);
            if (connectedEdges_ & Right)
                g.drawLine(bounds.getRight(), bounds.getY() + 3.0f, bounds.getRight(), bounds.getBottom() - 3.0f, 1.0f);
        }
    }
    else if (themeId == ThemeId::Aurora)
    {
        juce::Path p;
        if (connectedEdges_ == None) {
            p.addRoundedRectangle(bounds, radius);
        } else {
            p = createRoundedRectPath(bounds, radius,
                                      roundTopLeft, roundTopRight,
                                      roundBottomLeft, roundBottomRight);
        }

        UIColors::drawAuroraButtonChrome(g,
                                         bounds,
                                         radius,
                                         isHover,
                                         shouldDrawButtonAsDown,
                                         isToggled,
                                         {},
                                         {},
                                         &p,
                                         kAuroraToolbarChromeIntensity);
    }
    else
    {
        // Fallback for other themes (using basic colors)
        auto base = isActive ? UIColors::accent : UIColors::buttonNormal;
        getLookAndFeel().drawButtonBackground(g, *this, base, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }

    // 2. Icon
    juce::Path& path = (isToggled && !toggledIconPath_.isEmpty()) ? toggledIconPath_ : iconPath_;

    // 获取按钮名称，用于判断是否是 Play/Pause/Stop
    juce::String buttonName = getName();
    bool isTransportButton = (buttonName == "Play" || buttonName == "Pause" || buttonName == "Stop");

    juce::Colour iconColor;
    if (themeId == ThemeId::BlueBreeze)
    {
        if (isActive)
        {
            iconColor = juce::Colour(BlueBreeze::Colors::AccentBlue); // Active = Blue Icon
        }
        else if (isHover)
        {
            iconColor = juce::Colour(BlueBreeze::Colors::TextDark); // Hover = Darker
        }
        else
        {
            // Play/Pause/Stop 按钮使用深色（与 TAP 按钮一致）
            if (isTransportButton)
                iconColor = juce::Colour(BlueBreeze::Colors::TextDark); // 深色
            else
                iconColor = juce::Colour(BlueBreeze::Colors::TextDim); // Normal = Grey
        }
    }
    else if (themeId == ThemeId::Overdose)
    {
        if (isActive)
            iconColor = juce::Colour(0xFFEC4BAB);       // 激活：亮粉（参考图 #EC4BAB）
        else if (isHover)
            iconColor = juce::Colour(0xFFEC4BAB).brighter(0.12f);
        else
            iconColor = juce::Colour(0xFFE443A1);       // 普通：粉红（参考图 #E443A1）
    }
    else if (themeId == ThemeId::Aurora)
    {
        if (!isEnabled())
            iconColor = UIColors::textDisabled.withAlpha(0.46f);
        else if (isActive)
            iconColor = UIColors::textPrimary.withAlpha(0.96f);
        else if (isHover)
            iconColor = UIColors::textPrimary.interpolatedWith(UIColors::accent, 0.28f);
        else
            iconColor = UIColors::textPrimary.withAlpha(isTransportButton ? 0.92f : 0.78f);
    }
    else
    {
        iconColor = isEnabled() ? UIColors::textPrimary : UIColors::textDisabled;
    }

    if (!isEnabled())
    {
        iconColor = iconColor.withAlpha(0.4f);
    }

    // Draw Icon centered with hover scale animation
    auto iconArea = bounds.reduced(bounds.getWidth() * 0.06f, bounds.getHeight() * 0.06f);

    // Hover: subtle scale-up (1.08x) and brightness boost
    if (shouldDrawButtonAsHighlighted && !shouldDrawButtonAsDown)
    {
        const float hoverScale = 1.08f;
        iconArea = iconArea.withSizeKeepingCentre(iconArea.getWidth() * hoverScale, iconArea.getHeight() * hoverScale);
        iconColor = iconColor.brighter(0.15f);
    }

    // 参考图图标特征：深紫黑描边 + 鲜艳粉色主体
    if (themeId == ThemeId::Overdose)
    {
        const auto deepOutline = juce::Colour(0xFF0C0010).withAlpha(isActive ? 0.95f : 0.80f);
        if (solidIcon_)
        {
            // 实心图标（播放三角/暂停条/停止方块）：粉色填充 + 深紫黑描边
            ToolbarIcons::drawIcon(g, path, iconArea, deepOutline, 2.2f, false);
            ToolbarIcons::drawIcon(g, path, iconArea, iconColor, 1.0f, true);
        }
        else
        {
            // 线条型图标（文件/铅笔/眼睛/循环）：深紫黑粗线 + 粉色细芯
            ToolbarIcons::drawIcon(g, path, iconArea, deepOutline, 3.6f, false);
            ToolbarIcons::drawIcon(g, path, iconArea, iconColor, 1.7f, false);
        }
    }
    else
    {
        ToolbarIcons::drawIcon(g, path, iconArea, iconColor, 1.5f, false);
    }
}

TransportBarComponent::TransportBarComponent()
    : fileButton_(LOC(kFile), ToolbarIcons::getFileIcon())
    , editButton_(LOC(kEdit), ToolbarIcons::getEditIcon())
    , viewButton_(LOC(kView), ToolbarIcons::getEyeIcon())
    , playButton_(LOC(kPlay), ToolbarIcons::getPlayIcon())
    , pauseButton_(LOC(kPause), ToolbarIcons::getPauseIcon())
    , stopButton_(LOC(kStop), ToolbarIcons::getStopIcon())
    , loopButton_(LOC(kLoop), ToolbarIcons::getLoopIcon())
    , recordButton_(LOC(kRecord), ToolbarIcons::getRecordIcon())
    , trackViewButton_(LOC(kTracks), ToolbarIcons::getTrackViewIcon())
    , pianoViewButton_(LOC(kPianoRollView), ToolbarIcons::getPianoViewIcon())
    , tapButton_("Tap", ToolbarIcons::getTapIcon())
{
    // Setup Menu Buttons
    fileButton_.setTooltip(LOC(kTooltipFile));
    fileButton_.onClick = [this] { if (onFileMenuRequested) onFileMenuRequested(); };
    addAndMakeVisible(fileButton_);

    editButton_.setTooltip(LOC(kTooltipEdit));
    editButton_.onClick = [this] { if (onEditMenuRequested) onEditMenuRequested(); };
    addAndMakeVisible(editButton_);

    viewButton_.setTooltip(LOC(kTooltipView));
    viewButton_.onClick = [this] { if (onViewMenuRequested) onViewMenuRequested(); };
    addAndMakeVisible(viewButton_);

    // Setup Play Button
    playButton_.onClick = [this] { onPlayClicked(); };
    playButton_.setTooltip(LOC(kTooltipPlay) + "\nSpace");
    playButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleTransport);
    playButton_.setSolidIcon(true);
    addAndMakeVisible(playButton_);

    // Setup Pause Button
    pauseButton_.onClick = [this] { onPauseClicked(); };
    pauseButton_.setEnabled(false);
    pauseButton_.setTooltip(LOC(kTooltipPause) + "\nSpace");
    pauseButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleTransport);
    pauseButton_.setSolidIcon(true);
    addAndMakeVisible(pauseButton_);

    // Setup Stop Button
    stopButton_.onClick = [this] { onStopClicked(); };
    stopButton_.setTooltip(LOC(kTooltipStop));
    stopButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleTransport);
    stopButton_.setSolidIcon(true);
    addAndMakeVisible(stopButton_);

    // Setup Loop Button
    loopButton_.setClickingTogglesState(false);
    loopButton_.onClick = [this] { onLoopToggled(); };
    loopButton_.setTooltip(LOC(kTooltipLoop));
    loopButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleTransport);
    addAndMakeVisible(loopButton_);

    // Setup Record Button (read audio from DAW via ARA)
    recordButton_.onClick = [this] { onRecordClicked(); };
    recordButton_.setTooltip(LOC(kTooltipRecord));
    recordButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleTransport);
    addAndMakeVisible(recordButton_);

    // Setup Track View Button
    trackViewButton_.setClickingTogglesState(true);
    trackViewButton_.setToggleState(true, juce::dontSendNotification);
    trackViewButton_.onClick = [this] { onTrackViewClicked(); };
    trackViewButton_.setTooltip(LOC(kTooltipTrackView));
    trackViewButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleSegment);
    addAndMakeVisible(trackViewButton_);


    // Setup Piano View Button
    pianoViewButton_.setClickingTogglesState(true);
    pianoViewButton_.setToggleState(false, juce::dontSendNotification);
    pianoViewButton_.onClick = [this] { onPianoViewClicked(); };
    pianoViewButton_.setTooltip(LOC(kTooltipPianoRollView));
    pianoViewButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleSegment);
    addAndMakeVisible(pianoViewButton_);

    // Setup Joined Buttons (Segmented Control style)
    trackViewButton_.setConnectedEdges(UnifiedToolbarButton::Right);
    pianoViewButton_.setConnectedEdges(UnifiedToolbarButton::Left);

    bpmField_.setValue(120.0);
    bpmField_.onCommit = [this](double) { onBpmChanged(); };
    bpmField_.onTimeSignatureCommit = [this](int num, int denom) { onTimeSignatureChanged(num, denom); };
    bpmField_.setTooltip(LOC(kTooltipBpm));
    addAndMakeVisible(bpmField_);

    // Setup Tap Button
    tapButton_.onClick = [this] { onTapClicked(); };
    tapButton_.setTooltip(LOC(kTooltipTapTempo));
    tapButton_.getProperties().set(kOverdoseUiRoleKey, kOverdoseUiRoleTransport);
    addAndMakeVisible(tapButton_);

    timeDisplay_.setTimeString("00:00");
    timeDisplay_.setTooltip(LOC(kTooltipTimeline));
    addAndMakeVisible(timeDisplay_);

    // Apply styling (transport buttons use custom paintButton)

    // Setup scale selector
    scaleLabel_.setText("Scale:", juce::dontSendNotification);
    scaleLabel_.setFont(UIColors::getUIFont(UIColors::navFontHeight));
    scaleLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
    scaleLabel_.setJustificationType(juce::Justification::centredRight);
    scaleLabel_.setVisible(false);

    // Root note selector
    const char* notes[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < 12; ++i)
    {
        scaleRootSelector_.addItem(notes[i], i + 1);
    }
    scaleRootSelector_.setSelectedId(1, juce::dontSendNotification);
    scaleRootSelector_.onChange = [this] { onScaleChanged(); };
    scaleRootSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    scaleRootSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    scaleRootSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::primaryPurple);
    scaleRootSelector_.getProperties().set("noArrow", true);
    scaleRootSelector_.getProperties().set("fontHeight", UIColors::navFontHeight);
    scaleRootSelector_.getProperties().set(UIColors::auroraChromeIntensityProperty, kAuroraToolbarChromeIntensity);
    scaleRootSelector_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(scaleRootSelector_);

    // Scale type selector
    scaleTypeSelector_.addItem("Maj.", 1);
    scaleTypeSelector_.addItem("Min.", 2);
    scaleTypeSelector_.addItem("Chr.", 3);
    scaleTypeSelector_.addItem("H.Min.", 4);
    scaleTypeSelector_.addItem("Dor.", 5);
    scaleTypeSelector_.addItem("Mix.", 6);
    scaleTypeSelector_.addItem("Pent.", 7);
    scaleTypeSelector_.addItem("m.Pent.", 8);
    scaleTypeSelector_.setSelectedId(1, juce::dontSendNotification);
    scaleTypeSelector_.onChange = [this] { onScaleChanged(); };
    scaleTypeSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    scaleTypeSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    scaleTypeSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::primaryPurple);
    scaleTypeSelector_.getProperties().set("noArrow", true);
    scaleTypeSelector_.getProperties().set("fontHeight", UIColors::navFontHeight);
    scaleTypeSelector_.getProperties().set(UIColors::auroraChromeIntensityProperty, kAuroraToolbarChromeIntensity);
    scaleTypeSelector_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(scaleTypeSelector_);

    applyTheme();
}

TransportBarComponent::~TransportBarComponent()
{
}

void TransportBarComponent::refreshLocalizedText()
{
    // 刷新所有按钮的 tooltip
    fileButton_.setTooltip(LOC(kTooltipFile));
    editButton_.setTooltip(LOC(kTooltipEdit));
    viewButton_.setTooltip(LOC(kTooltipView));
    playButton_.setTooltip(LOC(kTooltipPlay) + "\nSpace");
    pauseButton_.setTooltip(LOC(kTooltipPause) + "\nSpace");
    stopButton_.setTooltip(LOC(kTooltipStop));
    loopButton_.setTooltip(LOC(kTooltipLoop));
    recordButton_.setTooltip(LOC(kTooltipRecord));
    trackViewButton_.setTooltip(LOC(kTooltipTrackView));
    pianoViewButton_.setTooltip(LOC(kTooltipPianoRollView));
    tapButton_.setTooltip(LOC(kTooltipTapTempo));

    // 刷新 scaleLabel
    scaleLabel_.setText(LOC(kScale), juce::dontSendNotification);

    repaint();
}

void TransportBarComponent::applyTheme()
{
    scaleLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);

    scaleRootSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    scaleRootSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    scaleRootSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);
    scaleRootSelector_.setColour(juce::ComboBox::arrowColourId, UIColors::accent);

    scaleTypeSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    scaleTypeSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    scaleTypeSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);
    scaleTypeSelector_.setColour(juce::ComboBox::arrowColourId, UIColors::accent);

    repaint();
}

void TransportBarComponent::setEmbeddedInTopBar(bool embedded)
{
    embeddedInTopBar_ = embedded;
    repaint();
}

void TransportBarComponent::setLayoutProfile(LayoutProfile profile)
{
    if (layoutProfile_ == profile)
        return;

    layoutProfile_ = profile;
    resized();
    repaint();
}

void TransportBarComponent::paint(juce::Graphics& g)
{
    const auto& style = UIColors::currentThemeStyle();
    const auto themeId = UIColors::currentThemeId();
    auto bounds = getLocalBounds().toFloat();

    if (!embeddedInTopBar_)
    {
        UIColors::drawShadow(g, bounds, UIColors::ShadowLevel::Float);
        if (themeId == ThemeId::Overdose)
        {
            juce::Path tray;
            tray.addRoundedRectangle(bounds, style.panelRadius);

            // 玻璃拟态面板：白→淡紫渐变+顶部高光+底部内阴影
            juce::ColourGradient panelGrad(juce::Colour(Overdose::Colors::PanelOpaqueTop).withAlpha(0.96f),
                                            bounds.getX(), bounds.getY(),
                                            juce::Colour(Overdose::Colors::PanelOpaqueBottom).withAlpha(0.96f),
                                            bounds.getX(), bounds.getBottom(), false);
            panelGrad.addColour(0.40f, juce::Colour(Overdose::Colors::PanelOpaqueMid).withAlpha(0.92f));
            g.setGradientFill(panelGrad);
            g.fillPath(tray);

            // 顶部高光
            {
                juce::Graphics::ScopedSaveState clipState(g);
                g.reduceClipRegion(tray);
                auto highlightBand = bounds.withHeight(bounds.getHeight() * 0.35f);
                juce::ColourGradient hl(juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.22f),
                                         highlightBand.getX(), highlightBand.getY(),
                                         juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.0f),
                                         highlightBand.getX(), highlightBand.getBottom(), false);
                g.setGradientFill(hl);
                g.fillRect(highlightBand);
            }

            // 边框
            g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.45f));
            g.strokePath(tray, juce::PathStrokeType(0.8f));
        }
        else if (themeId == ThemeId::BlueBreeze)
        {
            UIColors::fillBlueBreezeTray(g, bounds, style.panelRadius);
            UIColors::drawPanelFrame(g, bounds, style.panelRadius);
        }
        else
        {
            UIColors::fillPanelBackground(g, bounds, style.panelRadius);
            UIColors::drawPanelFrame(g, bounds, style.panelRadius);
        }
    }

    if (themeId == ThemeId::Overdose
        && trackViewButton_.isVisible()
        && pianoViewButton_.isVisible())
    {
        auto segmentBounds = trackViewButton_.getBounds().getUnion(pianoViewButton_.getBounds()).toFloat().reduced(2.0f);
        // 程序化绘制 Tab 底（淡紫渐变+粉边）
        juce::ColourGradient tabGrad(juce::Colour(Overdose::Colors::PanelOpaqueTop).withAlpha(0.85f),
                                     segmentBounds.getX(), segmentBounds.getY(),
                                     juce::Colour(Overdose::Colors::PanelOpaqueBottom).withAlpha(0.85f),
                                     segmentBounds.getX(), segmentBounds.getBottom(), false);
        g.setGradientFill(tabGrad);
        g.fillRoundedRectangle(segmentBounds, style.controlRadius);
        g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.40f));
        g.drawRoundedRectangle(segmentBounds, style.controlRadius, 0.8f);
    }

    // 时间码背景：与 UnifiedToolbarButton 一致缩进 2px，视觉高度统一为 36px
    auto displayBounds = timeDisplay_.getBounds().toFloat().reduced(2.0f);
    // 鏃堕棿鐮侊細LCD 椋庢牸鏄剧ず灞忥紙鍙傝€冨浘鐗囬鏍硷級
    if (themeId == ThemeId::Overdose)
    {
        UIColors::drawOverdoseDisplayWell(g, displayBounds, style.fieldRadius);
    }
    else if (themeId == ThemeId::BlueBreeze)
    {
        UIColors::fillBlueBreezeDisplayWell(g, displayBounds, style.fieldRadius);
    }
    else if (themeId == ThemeId::Aurora)
    {
        UIColors::drawAuroraButtonChrome(g,
                                         displayBounds,
                                         style.fieldRadius,
                                         false,
                                         false,
                                         false,
                                         {},
                                         {},
                                         nullptr,
                                         kAuroraToolbarChromeIntensity);
    }
    else
    {
        g.setColour(UIColors::backgroundDark.darker(0.2f));
        g.fillRoundedRectangle(displayBounds, style.fieldRadius);
        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.drawRoundedRectangle(displayBounds, style.fieldRadius, 1.0f);
    }

    if (!embeddedInTopBar_ && renderStatusText_.isNotEmpty()) {
        g.setColour(UIColors::textSecondary.withAlpha(0.9f));
        g.setFont(UIColors::getUIFont(12.0f));
        auto statusBounds = getLocalBounds().toFloat().reduced(14.0f, 6.0f);
        statusBounds.removeFromLeft(300.0f);
        statusBounds.removeFromRight(10.0f);
        g.drawText(renderStatusText_, statusBounds, juce::Justification::centredRight, true);
    }
}

void TransportBarComponent::mouseDown(const juce::MouseEvent& e)
{
    // 如果点击的不是BpmField，让BpmField失去焦点
    if (!bpmField_.getBounds().contains(e.getPosition()))
    {
        if (bpmField_.hasKeyboardFocus(true))
        {
            bpmField_.giveAwayKeyboardFocus();
        }
    }

    // 缁х画浼犻€掍簨浠剁粰鐖剁被
    juce::Component::mouseDown(e);
}

void TransportBarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(4, 4);

    const int controlHeight = 40;
    const int buttonWidth = 50;
    const int spacing = 3;
    const int groupGap = 5;

    auto row = bounds.withHeight(controlHeight).withY(bounds.getCentreY() - controlHeight / 2);

    if (layoutProfile_ == LayoutProfile::VST3AraSingleClip)
    {
        fileButton_.setBounds(row.removeFromLeft(buttonWidth));
        row.removeFromLeft(spacing);
        editButton_.setBounds(row.removeFromLeft(buttonWidth));
        row.removeFromLeft(spacing);
        viewButton_.setBounds(row.removeFromLeft(buttonWidth));
        row.removeFromLeft(groupGap);

        playButton_.setVisible(false);
        pauseButton_.setVisible(false);
        stopButton_.setVisible(false);
        loopButton_.setVisible(false);

        trackViewButton_.setVisible(false);
        pianoViewButton_.setVisible(false);

        bpmField_.setVisible(true);
        bpmField_.setReadOnly(true);
        tapButton_.setVisible(false);

        // 时间显示：恢复历史宽度 140px
        const int timeDisplayWidth = 140;
        timeDisplay_.setBounds(row.removeFromLeft(timeDisplayWidth));
        row.removeFromLeft(spacing);

        recordButton_.setVisible(true);
        recordButton_.setBounds(row.removeFromLeft(buttonWidth));

        const int bpmWidth = 110;
        bpmField_.setBounds(row.removeFromLeft(bpmWidth));
        row.removeFromLeft(spacing);

        const int rootWidth = 60;
        const int typeWidth = 180;

        scaleRootSelector_.setBounds(row.removeFromLeft(rootWidth));
        row.removeFromLeft(4);
        scaleTypeSelector_.setBounds(row.removeFromLeft(typeWidth));

        return;
    }

    playButton_.setVisible(true);
    pauseButton_.setVisible(true);
    stopButton_.setVisible(true);
    loopButton_.setVisible(true);
    trackViewButton_.setVisible(true);
    pianoViewButton_.setVisible(true);
    bpmField_.setVisible(true);
    bpmField_.setReadOnly(false);
    tapButton_.setVisible(true);
    recordButton_.setVisible(false);

    fileButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(spacing);
    editButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(spacing);
    viewButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(groupGap);

    playButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(spacing);
    pauseButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(spacing);
    stopButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(spacing);
    loopButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(groupGap);

    trackViewButton_.setBounds(row.removeFromLeft(buttonWidth));
    pianoViewButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(spacing);

    // 时间显示：恢复历史宽度 156px
    const int timeDisplayWidth = 156;
    timeDisplay_.setBounds(row.removeFromLeft(timeDisplayWidth));
    row.removeFromLeft(spacing);

    const int bpmWidth = 110;
    bpmField_.setBounds(row.removeFromLeft(bpmWidth));
    row.removeFromLeft(spacing);
    tapButton_.setBounds(row.removeFromLeft(buttonWidth));
    row.removeFromLeft(4);

    const int rootWidth = 60;
    const int typeWidth = 180;

    scaleRootSelector_.setBounds(row.removeFromLeft(rootWidth));
    row.removeFromLeft(4);
    scaleTypeSelector_.setBounds(row.removeFromLeft(typeWidth));
}

void TransportBarComponent::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void TransportBarComponent::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void TransportBarComponent::setPlaying(bool playing)
{
    isPlaying_ = playing;
    playButton_.setEnabled(!playing);
    pauseButton_.setEnabled(playing);
}

bool TransportBarComponent::isPlaying() const
{
    return isPlaying_;
}

void TransportBarComponent::setRenderStatusText(const juce::String& text)
{
    if (renderStatusText_ != text) {
        renderStatusText_ = text;
        repaint();
    }
}

void TransportBarComponent::setLoopEnabled(bool enabled)
{
    loopButton_.setToggleState(enabled, juce::dontSendNotification);
}

bool TransportBarComponent::isLoopEnabled() const
{
    return loopButton_.getToggleState();
}

void TransportBarComponent::setRecordButtonState(RecordButtonState state)
{
    switch (state) {
        case RecordButtonState::Idle:
            recordButton_.setToggleState(false, juce::dontSendNotification);
            recordButton_.setEnabled(true);
            break;
        case RecordButtonState::Capturing:
            recordButton_.setToggleState(true, juce::dontSendNotification);
            recordButton_.setEnabled(true);
            break;
        case RecordButtonState::Processing:
            recordButton_.setToggleState(false, juce::dontSendNotification);
            recordButton_.setEnabled(false);
            break;
    }
    recordButton_.repaint();
}

void TransportBarComponent::setWorkspaceView(bool workspaceView)
{
    workspaceView_ = workspaceView;
    // Update both buttons based on the state
    trackViewButton_.setToggleState(workspaceView_, juce::dontSendNotification);
    pianoViewButton_.setToggleState(!workspaceView_, juce::dontSendNotification);
}

bool TransportBarComponent::isWorkspaceView() const
{
    return workspaceView_;
}

void TransportBarComponent::setBpm(double bpm)
{
    currentBpm_ = bpm;
    bpmField_.setValue(bpm);
}

double TransportBarComponent::getBpm() const
{
    return bpmField_.getValue();
}

void TransportBarComponent::setTimeSignature(int numerator, int denominator)
{
    currentTimeSigNum_ = numerator;
    currentTimeSigDenom_ = denominator;
    bpmField_.setTimeSignature(numerator, denominator);
}

void TransportBarComponent::setScale(int rootNote, int scaleType)
{
    // rootNote: 0-11 (C-B)
    // scaleType: 1=Maj, 2=Min, 3=Chr, 4=H.Min, 5=Dor, 6=Mix, 7=Pent, 8=m.Pent
    scaleRootSelector_.setSelectedId(rootNote + 1, juce::dontSendNotification);
    scaleTypeSelector_.setSelectedId(scaleType, juce::dontSendNotification);
}

void TransportBarComponent::setPositionSeconds(double seconds)
{
    currentPositionSeconds_ = seconds;

    if (timelineDisplayMode_ == TimelineDisplayMode::Bars)
    {
        // Convert seconds to bars/beats using canonical BPM and time signature
        const double quarterSeconds = 60.0 / currentBpm_;
        const double beatSeconds = quarterSeconds * 4.0 / currentTimeSigDenom_;

        const double totalBeats = seconds / beatSeconds;
        const int bar = static_cast<int>(totalBeats / currentTimeSigNum_) + 1;
        const int beat = (static_cast<int>(totalBeats) % currentTimeSigNum_) + 1;

        timeDisplay_.setBarsString(bar, beat);
    }
    else
    {
        const int totalSeconds = static_cast<int>(seconds);
        const int minutes = totalSeconds / 60;
        const int secs = totalSeconds % 60;
        const int milliseconds = static_cast<int>(std::fmod(seconds * 1000.0, 1000.0));

        timeDisplay_.setTimeString(juce::String::formatted("%02d:%02d.%03d", minutes, secs, milliseconds));
    }
}

void TransportBarComponent::setTimelineDisplayMode(TimelineDisplayMode mode)
{
    if (timelineDisplayMode_ == mode)
        return;

    timelineDisplayMode_ = mode;

    // 刷新数字显示
    setPositionSeconds(currentPositionSeconds_);
}

void TransportBarComponent::onPlayClicked()
{
    listeners_.call([](Listener& l) { l.playRequested(); });
}

void TransportBarComponent::onPauseClicked()
{
    listeners_.call([](Listener& l) { l.pauseRequested(); });
}

void TransportBarComponent::onStopClicked()
{
    listeners_.call([](Listener& l) { l.stopRequested(); });
}

void TransportBarComponent::onLoopToggled()
{
    bool enabled = !loopButton_.getToggleState();
    listeners_.call([enabled](Listener& l) { l.loopToggled(enabled); });
}

void TransportBarComponent::onTrackViewClicked()
{
    // If already in track view, do nothing or re-assert
    if (workspaceView_) {
        trackViewButton_.setToggleState(true, juce::dontSendNotification);
        return;
    }

    setWorkspaceView(true);
    listeners_.call([this](Listener& l) { l.viewToggled(workspaceView_); });
}

void TransportBarComponent::onPianoViewClicked()
{
    // If already in piano view, do nothing or re-assert
    if (!workspaceView_) {
        pianoViewButton_.setToggleState(true, juce::dontSendNotification);
        return;
    }

    setWorkspaceView(false);
    listeners_.call([this](Listener& l) { l.viewToggled(workspaceView_); });
}

void TransportBarComponent::onBpmChanged()
{
    double bpm = getBpm();
    currentBpm_ = bpm;
    listeners_.call([bpm](Listener& l) { l.bpmChanged(bpm); });
}

void TransportBarComponent::onTimeSignatureChanged(int num, int denom)
{
    currentTimeSigNum_ = num;
    currentTimeSigDenom_ = denom;
    listeners_.call([num, denom](Listener& l) { l.timeSignatureChanged(num, denom); });
}

void TransportBarComponent::onTapClicked()
{
    auto currentTime = juce::Time::getCurrentTime();

    if (lastTapTime_.toMilliseconds() == 0)
    {
        // First tap
        lastTapTime_ = currentTime;
        tapIntervals_.clear();
    }
    else
    {
        // Calculate interval from last tap
        auto interval = currentTime.toMilliseconds() - lastTapTime_.toMilliseconds();
        lastTapTime_ = currentTime;

        // Ignore intervals that are too long (> 2 seconds) - reset
        if (interval > 2000)
        {
            tapIntervals_.clear();
        }
        else if (interval > 200)  // Ignore very short intervals (< 200ms = 300 BPM)
        {
            tapIntervals_.push_back(static_cast<double>(interval));

            // Keep only recent taps
            if (tapIntervals_.size() > maxTapSamples_)
            {
                tapIntervals_.erase(tapIntervals_.begin());
            }

            // Calculate average interval
            double averageInterval = 0.0;
            for (double i : tapIntervals_)
                averageInterval += i;
            averageInterval /= tapIntervals_.size();

            // Convert to BPM (60000 ms per minute)
            double bpm = 60000.0 / averageInterval;

            // Round to 1 decimal place
            bpm = std::round(bpm * 10.0) / 10.0;

            // Update BPM
            setBpm(bpm);
            onBpmChanged();
        }
    }
}

void TransportBarComponent::onScaleChanged()
{
    int rootNote = scaleRootSelector_.getSelectedId() - 1;  // 0-11
    int scaleType = scaleTypeSelector_.getSelectedId();  // 1=Major, 2=Minor, 3=Chromatic
    listeners_.call([rootNote, scaleType](Listener& l) { l.scaleChanged(rootNote, scaleType); });
}

void TransportBarComponent::onRecordClicked()
{
    listeners_.call([](Listener& l) { l.recordRequested(); });
}

} // namespace OpenTune

#include "TransportBarComponent.h"
#include "UIColors.h"
#include "ToolbarIcons.h"
#include <cmath>

namespace OpenTune {

DigitalTimeDisplay::DigitalTimeDisplay()
{
    setInterceptsMouseClicks(false, false);
}

void DigitalTimeDisplay::setTimeString(const juce::String& time)
{
    if (timeString_ != time)
    {
        timeString_ = time;
        repaint();
    }
}

void DigitalTimeDisplay::paint(juce::Graphics& g)
{
    const auto& style = Theme::getActiveStyle();
    auto bounds = getLocalBounds().toFloat().reduced(4.0f);
    const float yOffset = juce::jlimit(0.0f, 2.0f, bounds.getHeight() * 0.03f);
    bounds = bounds.translated(0.0f, yOffset);
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    if (!style.timeSegmentStyle)
    {
        g.setColour(UIColors::textPrimary);
        // 导航栏字体统一：时间码与其他控件保持同一字号体系
        g.setFont(UIColors::getMonoFont(UIColors::navMonoFontHeight));
        g.drawFittedText(timeString_, getLocalBounds().reduced(6, 0), juce::Justification::centred, 1, 1.0f);
        return;
    }

    float totalWeight = 0.0f;
    for (int i = 0; i < timeString_.length(); ++i)
    {
        auto c = timeString_[i];
        totalWeight += (c == ':' || c == '.') ? 0.50f : 1.00f;
    }
    if (totalWeight <= 0.0f)
        totalWeight = 1.0f;

    const float baseCharW = bounds.getWidth() / totalWeight;
    float x = bounds.getX();

    for (int i = 0; i < timeString_.length(); ++i)
    {
        auto c = timeString_[i];
        const float w = (c == ':' || c == '.') ? baseCharW * 0.50f : baseCharW;
        drawChar(g, c, { x, bounds.getY(), w, bounds.getHeight() });
        x += w;
    }
}

void DigitalTimeDisplay::drawChar(juce::Graphics& g, juce::juce_wchar c, juce::Rectangle<float> area)
{
    const auto& style = Theme::getActiveStyle();
    if (c == ':')
    {
        const float dotSize = area.getWidth() * 0.55f;
        const float cx = area.getCentreX();
        
        if (Theme::getActiveTheme() == ThemeId::BlueBreeze)
        {
             juce::Colour lightBlueText(0xFF9BB2C4);
             g.setColour(lightBlueText);
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
        
        if (Theme::getActiveTheme() == ThemeId::BlueBreeze)
        {
             juce::Colour lightBlueText(0xFF9BB2C4);
             g.setColour(lightBlueText);
        }
        else
        {
             g.setColour(style.timeActive);
        }
        
        g.fillEllipse(area.getCentreX() - dotSize * 0.5f, area.getBottom() - dotSize * 1.45f, dotSize, dotSize);
        return;
    }

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
    const auto& style = Theme::getActiveStyle();
    
    if (Theme::getActiveTheme() == ThemeId::BlueBreeze)
    {
        // Light Blue/Grey color (e.g. #9BB2C4 or similar)
        juce::Colour lightBlueText(0xFF9BB2C4);
        g.setColour(active ? lightBlueText : lightBlueText.withAlpha(0.1f));
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
    text_ = juce::String(static_cast<int>(value));
    caretIndex_ = text_.length();
    lastValidValue_ = static_cast<int>(value);
    isEditing_ = false;
    repaint();
}

double BpmValueField::getValue() const
{
    return text_.getDoubleValue();
}

void BpmValueField::paint(juce::Graphics& g)
{
    const auto& style = Theme::getActiveStyle();
    const auto themeId = Theme::getActiveTheme();
    auto bounds = getLocalBounds().toFloat();

    // 更厚实的输入框质感（深蓝灰主题）
    if (themeId == ThemeId::DarkBlueGrey)
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
        g.setColour(UIColors::backgroundLight);
        g.fillRoundedRectangle(bounds, style.fieldRadius);
    }

    auto focused = isEditing_ && hasKeyboardFocus(true);
    g.setColour(focused ? UIColors::accent : UIColors::panelBorder);
    g.drawRoundedRectangle(bounds.reduced(0.5f), style.fieldRadius, focused ? style.focusRingThickness : style.strokeThin);

    // 统一导航栏文本字号
    auto font = UIColors::getLabelFont(UIColors::navFontHeight);
    g.setFont(font);

    // 小字放进控件内部：BPM 不再占用按钮左右两边
    auto content = getLocalBounds().reduced(8, 0);
    auto prefixArea = content.removeFromLeft(40);  // 缩小宽度，减少右侧空隙
    auto valueArea = content;

    g.setColour(UIColors::textSecondary.withAlpha(0.85f));
    g.drawFittedText("BPM", prefixArea, juce::Justification::centredLeft, 1, 1.0f);

    g.setColour(UIColors::textPrimary);
    g.drawFittedText(text_, valueArea, juce::Justification::centredLeft, 1, 1.0f);

    if (isEditing_ && showCaret_)
    {
        auto getTextWidth = [&](const juce::String& s) -> float {
            juce::GlyphArrangement ga;
            ga.addLineOfText(font, s, 0.0f, 0.0f);
            return ga.getBoundingBox(0, 0, true).getWidth();
        };
        const auto area = valueArea.toFloat();
        const auto cx = area.getX();
        const auto caretX = cx + getTextWidth(text_.substring(0, caretIndex_));
        g.setColour(UIColors::textPrimary.withAlpha(0.8f));
        g.drawLine(caretX, static_cast<float>(bounds.getY()) + 7.0f, caretX, static_cast<float>(bounds.getBottom()) - 7.0f, style.strokeThick);
    }
}

void BpmValueField::mouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    grabKeyboardFocus();
    if (!isEditing_)
    {
        isEditing_ = true;
        text_.clear();
        caretIndex_ = 0;
    }
    repaint();
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
    
    auto v = text_.getIntValue();
    if (v <= 0 || text_.isEmpty())
    {
        text_ = juce::String(lastValidValue_);
        caretIndex_ = text_.length();
        repaint();
        return;
    }
    
    lastValidValue_ = v;
    text_ = juce::String(v);
    caretIndex_ = text_.length();
    repaint();
    
    if (onCommit)
        onCommit(static_cast<double>(v));
}

void BpmValueField::cancelEdit()
{
    isEditing_ = false;
    text_ = juce::String(lastValidValue_);
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

void UnifiedToolbarButton::setToggledIcon(juce::Path iconPath)
{
    toggledIconPath_ = iconPath;
    repaint();
}

void UnifiedToolbarButton::setConnectedEdges(int edges)
{
    connectedEdges_ = edges;
    repaint();
}

void UnifiedToolbarButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // 统一尺寸和圆角
    auto bounds = getLocalBounds().toFloat().reduced(2.0f); 
    float radius = 6.0f; 
    
    // Handle connected edges for corners
    // JUCE Rectangle doesn't have topLeft/topRight/bottomLeft/bottomRight static members
    // We'll use boolean flags directly below
    
    // We will use Path to draw specific rounded corners
    bool roundTopLeft = ! (connectedEdges_ & Left);
    bool roundBottomLeft = ! (connectedEdges_ & Left);
    bool roundTopRight = ! (connectedEdges_ & Right);
    bool roundBottomRight = ! (connectedEdges_ & Right);

    // If connected, we might want to extend the background slightly to avoid double borders
    // But for now let's just handle the shape.

    bool isToggled = getToggleState();
    bool isActive = isToggled || shouldDrawButtonAsDown;
    bool isHover = shouldDrawButtonAsHighlighted;

    const auto themeId = Theme::getActiveTheme();

    // 辅助函数：创建带选择性圆角的矩形路径
    auto createRoundedRectPath = [](juce::Rectangle<float> rect, float r, 
                                    bool tl, bool tr, bool bl, bool br) -> juce::Path {
        juce::Path p;
        float x = rect.getX(), y = rect.getY(), w = rect.getWidth(), h = rect.getHeight();
        
        // 从左上角开始
        if (tl) {
            p.startNewSubPath(x, y + r);
            p.addArc(x, y, r * 2, r * 2, juce::MathConstants<float>::pi, juce::MathConstants<float>::pi * 1.5f);
        } else {
            p.startNewSubPath(x, y);
        }
        
        // 上边到右上角
        if (tr) {
            p.lineTo(x + w - r, y);
            p.addArc(x + w - r * 2, y, r * 2, r * 2, juce::MathConstants<float>::pi * 1.5f, 0);
        } else {
            p.lineTo(x + w, y);
        }
        
        // 右边到右下角
        if (br) {
            p.lineTo(x + w, y + h - r);
            p.addArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, juce::MathConstants<float>::pi * 0.5f);
        } else {
            p.lineTo(x + w, y + h);
        }
        
        // 下边到左下角
        if (bl) {
            p.lineTo(x + r, y + h);
            p.addArc(x, y + h - r * 2, r * 2, r * 2, juce::MathConstants<float>::pi * 0.5f, juce::MathConstants<float>::pi);
        } else {
            p.lineTo(x, y + h);
        }
        
        // 关闭路径
        if (tl) {
            p.lineTo(x, y + r);
        } else {
            p.lineTo(x, y);
        }
        
        p.closeSubPath();
        return p;
    };

    // 1. Background
    if (themeId == ThemeId::BlueBreeze)
    {
        // 独立按钮使用标准圆角矩形，连接按钮使用选择性圆角
        juce::Path p;
        if (connectedEdges_ == None) {
            // 独立按钮：标准圆角矩形
            p.addRoundedRectangle(bounds, radius);
        } else {
            // 连接按钮：选择性圆角
            p = createRoundedRectPath(bounds, radius, 
                                      roundTopLeft, roundTopRight, 
                                      roundBottomLeft, roundBottomRight);
        }

        if (isActive)
        {
            // Active: White Background with Shadow
            g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
            g.fillPath(p);
            
            // Subtle Shadow
            g.setColour(juce::Colour(BlueBreeze::Colors::ShadowColor).withAlpha(0.15f));
            g.strokePath(p, juce::PathStrokeType(1.0f));
        }
        else if (isHover)
        {
            // Hover: Light Overlay
            g.setColour(juce::Colour(BlueBreeze::Colors::HoverOverlay));
            g.fillPath(p);
            
            // Subtle Border
            g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(0.5f));
            g.strokePath(p, juce::PathStrokeType(1.0f));
        }
        else
        {
             // Normal state - if connected, maybe draw a separator or border?
             // If we want them to look "joined", we usually draw a border around the whole group
             // or draw borders for each segment.
             
             // For BlueBreeze normal buttons are usually transparent/flat.
             // But if they are joined, we might want a border to define the area?
             // Let's stick to the original logic: no background for normal state unless needed.
             // But wait, user said "two knobs joined together". 
             // If they are toggle buttons (View Switch), one is usually active.
             
             // Let's add a faint border for the structure if it's a connected group
             if (connectedEdges_ != None) {
                 g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(0.3f));
                 g.strokePath(p, juce::PathStrokeType(1.0f));
             }
        }
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
    else
    {
        iconColor = isEnabled() ? UIColors::textPrimary : UIColors::textDisabled;
    }
    
    if (!isEnabled())
    {
        iconColor = iconColor.withAlpha(0.4f);
    }

    // Draw Icon centered
    // Icon area: reduced to keep icon proportional
    auto iconArea = bounds.reduced(bounds.getWidth() * 0.22f, bounds.getHeight() * 0.22f);
    
    // Use ToolbarIcons helper
    ToolbarIcons::drawIcon(g, path, iconArea, iconColor, 1.5f, false);
}

TransportBarComponent::TransportBarComponent()
    : fileButton_("File", ToolbarIcons::getFileIcon())
    , editButton_("Edit", ToolbarIcons::getEditIcon())
    , viewButton_("View", ToolbarIcons::getEyeIcon())
    , playButton_("Play", ToolbarIcons::getPlayIcon())
    , pauseButton_("Pause", ToolbarIcons::getPauseIcon())
    , stopButton_("Stop", ToolbarIcons::getStopIcon())
    , loopButton_("Loop", ToolbarIcons::getLoopIcon())
    , trackViewButton_("Tracks", ToolbarIcons::getTrackViewIcon())
    , pianoViewButton_("Piano", ToolbarIcons::getPianoViewIcon())
    , tapButton_("Tap", ToolbarIcons::getTapIcon())
{
    // Setup Menu Buttons
    fileButton_.setTooltip("File");
    fileButton_.onClick = [this] { if (onFileMenuRequested) onFileMenuRequested(); };
    addAndMakeVisible(fileButton_);

    editButton_.setTooltip("Edit");
    editButton_.onClick = [this] { if (onEditMenuRequested) onEditMenuRequested(); };
    addAndMakeVisible(editButton_);

    viewButton_.setTooltip("View");
    viewButton_.onClick = [this] { if (onViewMenuRequested) onViewMenuRequested(); };
    addAndMakeVisible(viewButton_);

    // Setup Play Button
    playButton_.onClick = [this] { onPlayClicked(); };
    playButton_.setTooltip("Play");
    addAndMakeVisible(playButton_);

    // Setup Pause Button
    pauseButton_.onClick = [this] { onPauseClicked(); };
    pauseButton_.setEnabled(false);
    pauseButton_.setTooltip("Pause");
    addAndMakeVisible(pauseButton_);

    // Setup Stop Button
    stopButton_.onClick = [this] { onStopClicked(); };
    stopButton_.setTooltip("Stop");
    addAndMakeVisible(stopButton_);

    // Setup Loop Button
    loopButton_.setClickingTogglesState(true);
    loopButton_.onClick = [this] { onLoopToggled(); };
    loopButton_.setTooltip("Loop");
    addAndMakeVisible(loopButton_);

    // Setup Track View Button
    trackViewButton_.setClickingTogglesState(true);
    trackViewButton_.setToggleState(true, juce::dontSendNotification); // Default active
    trackViewButton_.onClick = [this] { onTrackViewClicked(); };
    trackViewButton_.setTooltip("Track View");
    addAndMakeVisible(trackViewButton_);


    // Setup Piano View Button
    pianoViewButton_.setClickingTogglesState(true);
    pianoViewButton_.setToggleState(false, juce::dontSendNotification);
    pianoViewButton_.onClick = [this] { onPianoViewClicked(); };
    pianoViewButton_.setTooltip("Piano Roll View");
    addAndMakeVisible(pianoViewButton_);
    
    // Setup Joined Buttons (Segmented Control style)
    trackViewButton_.setConnectedEdges(UnifiedToolbarButton::Right);
    pianoViewButton_.setConnectedEdges(UnifiedToolbarButton::Left);

    // Setup BPM Label
    bpmLabel_.setText("BPM", juce::dontSendNotification);
    bpmLabel_.setFont(UIColors::getUIFont(UIColors::navFontHeight));
    bpmLabel_.setJustificationType(juce::Justification::centredRight);
    bpmLabel_.setVisible(false);

    bpmField_.setValue(120.0);
    bpmField_.onCommit = [this](double) { onBpmChanged(); };
    addAndMakeVisible(bpmField_);

    // Setup Tap Button
    tapButton_.onClick = [this] { onTapClicked(); };
    tapButton_.setTooltip("Tap Tempo");
    addAndMakeVisible(tapButton_);

    timeDisplay_.setTimeString("00:00");
    addAndMakeVisible(timeDisplay_);

    // Apply styling (transport buttons use custom paintButton)
    bpmLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);

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
    scaleRootSelector_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(scaleRootSelector_);

    // Scale type selector
    scaleTypeSelector_.addItem("Maj.", 1);
    scaleTypeSelector_.addItem("Min.", 2);
    scaleTypeSelector_.addItem("Chr.", 3);
    scaleTypeSelector_.setSelectedId(1, juce::dontSendNotification);
    scaleTypeSelector_.onChange = [this] { onScaleChanged(); };
    scaleTypeSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    scaleTypeSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    scaleTypeSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::primaryPurple);
    scaleTypeSelector_.getProperties().set("noArrow", true);
    scaleTypeSelector_.getProperties().set("fontHeight", UIColors::navFontHeight);
    scaleTypeSelector_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(scaleTypeSelector_);

    applyTheme();
}

TransportBarComponent::~TransportBarComponent()
{
}

void TransportBarComponent::applyTheme()
{
    bpmLabel_.setColour(juce::Label::textColourId, Theme::getActiveTheme() == ThemeId::DarkBlueGrey ? UIColors::textPrimary : UIColors::textSecondary);
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

void TransportBarComponent::paint(juce::Graphics& g)
{
    const auto& style = Theme::getActiveStyle();
    auto bounds = getLocalBounds().toFloat();

    if (!embeddedInTopBar_)
    {
        UIColors::drawShadow(g, bounds);
        UIColors::fillPanelBackground(g, bounds, style.panelRadius);
        UIColors::drawPanelFrame(g, bounds, style.panelRadius);
    }

    auto displayBounds = timeDisplay_.getBounds().toFloat();
    // 时间码：LCD 风格显示屏（参考图片风格）
    if (Theme::getActiveTheme() == ThemeId::BlueBreeze)
    {
        // Reference style: Transparent background for BlueBreeze
        // g.setColour(juce::Colour(BlueBreeze::Colors::KnobBody)); 
    }
    else
    {
        g.setColour(UIColors::backgroundDark.darker(0.2f));
        g.fillRoundedRectangle(displayBounds, style.fieldRadius);
    }
    
    // Inner shadow for depth
    if (Theme::getActiveTheme() == ThemeId::BlueBreeze)
    {
        // Transparent background, Light gray border
        g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(0.8f));
        g.drawRoundedRectangle(displayBounds, style.fieldRadius, 1.5f);
    }
    else
    {
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

// 点击TransportBar非BpmField区域时，让BpmField失去焦点，停止光标闪烁
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
    
    // 继续传递事件给父类
    juce::Component::mouseDown(e);
}

void TransportBarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(12, 4);
    
    const int controlHeight = 40;
    const int buttonWidth = 50;
    const int spacing = 10;
    const int groupGap = 20;
    
    auto row = bounds.withHeight(controlHeight).withY(bounds.getCentreY() - controlHeight / 2);

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
    row.removeFromLeft(-10);
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
    bpmField_.setValue(bpm);
}

double TransportBarComponent::getBpm() const
{
    return bpmField_.getValue();
}

void TransportBarComponent::setScale(int rootNote, int scaleType)
{
    // rootNote: 0-11 (C-B)
    // scaleType: 1=Major, 2=Minor, 3=Chromatic (Matches ScaleInference + 1)
    scaleRootSelector_.setSelectedId(rootNote + 1, juce::dontSendNotification);
    scaleTypeSelector_.setSelectedId(scaleType, juce::dontSendNotification);
}

void TransportBarComponent::setPositionSeconds(double seconds)
{
    const int totalSeconds = static_cast<int>(seconds);
    const int minutes = totalSeconds / 60;
    const int secs = totalSeconds % 60;
    const int milliseconds = static_cast<int>(std::fmod(seconds * 1000.0, 1000.0));

    timeDisplay_.setTimeString(juce::String::formatted("%02d:%02d.%03d", minutes, secs, milliseconds));
}

void TransportBarComponent::onPlayClicked()
{
    setPlaying(true);
    listeners_.call([](Listener& l) { l.playRequested(); });
}

void TransportBarComponent::onPauseClicked()
{
    setPlaying(false);
    listeners_.call([](Listener& l) { l.pauseRequested(); });
}

void TransportBarComponent::onStopClicked()
{
    setPlaying(false);
    listeners_.call([](Listener& l) { l.stopRequested(); });
}

void TransportBarComponent::onLoopToggled()
{
    bool enabled = loopButton_.getToggleState();
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

    // Clamp BPM to reasonable range
    bpm = juce::jlimit(1.0, 480.0, bpm);

    // Update text editor with clamped value
    setBpm(bpm);

    listeners_.call([bpm](Listener& l) { l.bpmChanged(bpm); });
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

} // namespace OpenTune

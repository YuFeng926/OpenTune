#include "ParameterPanel.h"
#include "ToolbarIcons.h"
#include "../../Utils/PitchControlConfig.h"
#include "../../Utils/LocalizationManager.h"
#include <cmath>
#include <vector>

namespace OpenTune {

namespace {

constexpr float kAuroraSidebarChromeIntensity = 0.42f;
constexpr float kAuroraSidebarCurveBlendStart = 0.26f;
constexpr float kAuroraSidebarCurveBlendEnd = 0.44f;
constexpr float kAuroraSidebarCoreStart = 0.42f;
constexpr float kAuroraSidebarCoreEnd = 0.86f;
constexpr float kAuroraSidebarBottomLiftEnd = 1.0f;
constexpr float kAuroraSidebarCurveOpacity = 0.70f;

float smoothStep(float start, float end, float position)
{
    const auto t = juce::jlimit(0.0f, 1.0f, (position - start) / (end - start));
    return t * t * (3.0f - 2.0f * t);
}

float fixedDitherNoise(int x, int y)
{
    auto phase = 0.06711056f * static_cast<float>(x) + 0.00583715f * static_cast<float>(y);
    phase -= std::floor(phase);
    phase *= 52.9829189f;
    return phase - std::floor(phase) - 0.5f;
}

juce::Image makeAuroraSidebarSurfaceCurve(int width, int height)
{
    const auto curveTop = juce::Colour(0xFF0D2A43);
    const auto curveBottom = juce::Colour(0xFF0A2139);
    const auto bottomLift = juce::Colour(0xFF0B2A46);
    juce::Image surface(juce::Image::ARGB, width, height, true);

    {
        juce::Image::BitmapData pixels(surface, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < height; ++y)
        {
            const auto position = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
            const auto curveBlend = smoothStep(kAuroraSidebarCurveBlendStart, kAuroraSidebarCurveBlendEnd, position);
            const auto coreBlend = smoothStep(kAuroraSidebarCoreStart, kAuroraSidebarCoreEnd, position);
            const auto bottomBlend = smoothStep(kAuroraSidebarCoreEnd, kAuroraSidebarBottomLiftEnd, position);
            const auto curveChannel = [coreBlend, bottomBlend](juce::uint8 top,
                                                                juce::uint8 bottom,
                                                                juce::uint8 lift)
            {
                const auto core = static_cast<float>(top) + (static_cast<float>(bottom) - top) * coreBlend;
                return core + (static_cast<float>(lift) - core) * bottomBlend;
            };
            const auto red = curveChannel(curveTop.getRed(), curveBottom.getRed(), bottomLift.getRed());
            const auto green = curveChannel(curveTop.getGreen(), curveBottom.getGreen(), bottomLift.getGreen());
            const auto blue = curveChannel(curveTop.getBlue(), curveBottom.getBlue(), bottomLift.getBlue());
            const auto alpha = static_cast<juce::uint8>(juce::roundToInt(255.0f * kAuroraSidebarCurveOpacity * curveBlend));

            for (int x = 0; x < width; ++x)
            {
                const auto dither = fixedDitherNoise(x, y);
                pixels.setPixelColour(x,
                                      y,
                                      juce::Colour(static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(red + dither))),
                                                   static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(green + dither))),
                                                   static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(blue + dither))),
                                                   alpha));
            }
        }
    }

    return surface;
}

juce::String buildAutoButtonTooltip(const ParameterPanel::AutoButtonPresentation& presentation)
{
    juce::String tooltip = presentation.tooltip;
    if (tooltip.isEmpty()) {
        switch (presentation.mode) {
            case ParameterPanel::AutoButtonPresentation::Mode::ReferenceAuto:
                tooltip = juce::String::fromUTF8(u8"按参考 Clip 自动修音并对齐节奏");
                break;
            case ParameterPanel::AutoButtonPresentation::Mode::ReferenceBoundButFallbackToAuto:
                tooltip = juce::String::fromUTF8(u8"已绑定参考源，但当前缺少 GAME backend / models，本次执行普通 AUTO。");
                break;
            case ParameterPanel::AutoButtonPresentation::Mode::StandardAuto:
            default:
                tooltip = juce::String::fromUTF8(u8"自动修音（吸附到临近音阶）");
                break;
        }
    }

    return tooltip + "\n6";
}

} // namespace

ParameterPanel::ToolIconButton::ToolIconButton(int toolId, const juce::String& name, const juce::String& tooltip)
    : juce::Button(name), toolId_(toolId)
{
    setTooltip(tooltip);
    setClickingTogglesState(true);
}

void ParameterPanel::ToolIconButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);

    const auto themeId = UIColors::currentThemeId();

    if (themeId == ThemeId::Overdose)
    {
        const auto active = getToggleState() || shouldDrawButtonAsDown;
        UIColors::fillOverdoseButtonShell(g, bounds, UIColors::currentThemeStyle().controlRadius);

        if (active)
        {
            juce::Graphics::ScopedSaveState clip(g);
            juce::Path activeShape;
            activeShape.addRoundedRectangle(bounds.reduced(1.5f),
                                            juce::jmax(0.0f, UIColors::currentThemeStyle().controlRadius - 1.5f));
            g.reduceClipRegion(activeShape);

            juce::ColourGradient fill(juce::Colour(Overdose::Colors::ButtonActiveTop).withAlpha(0.96f),
                                      bounds.getX(),
                                      bounds.getY(),
                                      juce::Colour(Overdose::Colors::ButtonActiveBottom).withAlpha(0.96f),
                                      bounds.getX(),
                                      bounds.getBottom(),
                                      false);
            fill.addColour(0.54f, juce::Colour(Overdose::Colors::PanelHighlight).withAlpha(0.74f));
            g.setGradientFill(fill);
            g.fillRoundedRectangle(bounds.reduced(1.5f),
                                   juce::jmax(0.0f, UIColors::currentThemeStyle().controlRadius - 1.5f));

            g.setColour(juce::Colours::white.withAlpha(0.22f));
            g.drawLine(bounds.getX() + 9.0f,
                       bounds.getY() + 3.0f,
                       bounds.getRight() - 9.0f,
                       bounds.getY() + 3.0f,
                       1.0f);
        }
        else if (shouldDrawButtonAsHighlighted)
        {
            juce::Graphics::ScopedSaveState clip(g);
            juce::Path hoverShape;
            hoverShape.addRoundedRectangle(bounds.reduced(1.5f),
                                           juce::jmax(0.0f, UIColors::currentThemeStyle().controlRadius - 1.5f));
            g.reduceClipRegion(hoverShape);
            g.setColour(juce::Colour(Overdose::Colors::PanelHighlight).withAlpha(0.14f));
            g.fillRoundedRectangle(bounds.reduced(1.5f),
                                   juce::jmax(0.0f, UIColors::currentThemeStyle().controlRadius - 1.5f));
        }

        if (shouldDrawButtonAsHighlighted || active || shouldDrawButtonAsDown)
        {
            const auto colour = active || shouldDrawButtonAsDown
                ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.58f)
                : juce::Colour(Overdose::Colors::PanelHighlight).withAlpha(0.22f);
            g.setColour(colour);
            g.drawRoundedRectangle(bounds.reduced(0.75f), UIColors::currentThemeStyle().controlRadius, active ? 1.6f : 1.1f);
        }
    }
    else if (themeId == ThemeId::Aurora)
    {
        const auto active = getToggleState();
        UIColors::drawAuroraButtonChrome(g,
                                         bounds,
                                         UIColors::currentThemeStyle().controlRadius,
                                         shouldDrawButtonAsHighlighted,
                                         shouldDrawButtonAsDown,
                                         active,
                                         {},
                                         {},
                                         nullptr,
                                         kAuroraSidebarChromeIntensity);
    }
    else
    {
        auto base = getToggleState() ? UIColors::accent : UIColors::buttonNormal;
        getLookAndFeel().drawButtonBackground(g, *this, base, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }

    if (textIcon_.isNotEmpty())
    {
        const auto active = themeId == ThemeId::Overdose && (getToggleState() || shouldDrawButtonAsDown);

        if (subTextIcon_.isNotEmpty())
        {
            auto textArea = getLocalBounds().toFloat();
            const float mainFontSize = 16.0f;
            const float subFontSize = 11.0f;

            auto mainArea = textArea.removeFromTop(textArea.getHeight() * 0.55f);
            g.setColour(active ? juce::Colours::white.withAlpha(0.96f) : UIColors::textPrimary);
            g.setFont(UIColors::getUIFont(mainFontSize));
            g.drawText(textIcon_, mainArea, juce::Justification::centredBottom);

            g.setColour(UIColors::textSecondary);
            g.setFont(UIColors::getUIFont(subFontSize));
            g.drawText(subTextIcon_, textArea, juce::Justification::centredTop);
        }
        else
        {
            g.setColour(active ? juce::Colours::white.withAlpha(0.96f) : UIColors::textPrimary);
            g.setFont(UIColors::getUIFont(16.0f));
            g.drawText(textIcon_, getLocalBounds().toFloat(), juce::Justification::centred);
        }
    }
    else
    {
        auto iconArea = getLocalBounds().toFloat().reduced(10.0f);
        const auto active = themeId == ThemeId::Overdose && (getToggleState() || shouldDrawButtonAsDown);
        auto iconColor = active ? juce::Colours::white.withAlpha(0.96f) : UIColors::textPrimary;

        // Hover: subtle scale-up (1.08x) and brightness boost
        if (shouldDrawButtonAsHighlighted && !shouldDrawButtonAsDown)
        {
            const float hoverScale = 1.08f;
            iconArea = iconArea.withSizeKeepingCentre(iconArea.getWidth() * hoverScale, iconArea.getHeight() * hoverScale);
            iconColor = active ? iconColor : iconColor.brighter(0.15f);
        }

        ToolbarIcons::drawIcon(g, iconPath_, iconArea, iconColor, 2.0f, fillIcon_);
    }

}

// ============================================================================
// LargeKnobLookAndFeel Implementation
// ============================================================================

LargeKnobLookAndFeel::LargeKnobLookAndFeel()
{
    // Set default colors for text boxes
    setColour(juce::Slider::textBoxTextColourId, UIColors::textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId, UIColors::backgroundDark);
    setColour(juce::Slider::textBoxOutlineColourId, UIColors::panelBorder);
}

void LargeKnobLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPosProportional, float rotaryStartAngle,
                                           float rotaryEndAngle, juce::Slider& slider)
{
    const auto themeId = UIColors::currentThemeId();
    if (themeId == ThemeId::Overdose)
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                            static_cast<float>(y),
                                            static_cast<float>(width),
                                            static_cast<float>(height)).reduced(2.0f);
        UIColors::drawOverdoseKnob(g,
                                   bounds,
                                   sliderPosProportional,
                                   slider.isMouseOverOrDragging(),
                                   rotaryStartAngle,
                                   rotaryEndAngle);
        return;
    }

    if (themeId == ThemeId::Aurora)
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                            static_cast<float>(y),
                                            static_cast<float>(width),
                                            static_cast<float>(height)).reduced(2.0f);
        UIColors::drawAuroraKnob(g,
                                 bounds,
                                 sliderPosProportional,
                                 slider.isMouseOverOrDragging(),
                                 rotaryStartAngle,
                                 rotaryEndAngle);
        return;
    }

    if (themeId == ThemeId::BlueBreeze)
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                            static_cast<float>(y),
                                            static_cast<float>(width),
                                            static_cast<float>(height)).reduced(2.0f);
        UIColors::drawBlueBreezePianoKnob(g,
                                          bounds,
                                          sliderPosProportional,
                                          slider.isMouseOverOrDragging(),
                                          rotaryStartAngle,
                                          rotaryEndAngle);
        return;
    }

    // Piano Black Minimalist Knob
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height)).reduced(2.0f);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto center = bounds.getCentre();
    auto trackRadius = radius * 0.85f;

    // 1. Shadow for depth
    juce::DropShadow ds;
    ds.radius = 10;
    ds.offset = { 0, 4 };
    ds.colour = UIColors::backgroundDark.withAlpha(0.50f);
    
    juce::Path shadowPath;
    shadowPath.addEllipse(center.x - trackRadius, center.y - trackRadius, trackRadius * 2, trackRadius * 2);
    ds.drawForPath(g, shadowPath);

    // 2. Piano Black Body
    // Main body - deep black
    g.setColour(juce::Colour(0xff0a0a0a)); 
    g.fillEllipse(center.x - trackRadius, center.y - trackRadius, trackRadius * 2, trackRadius * 2);

    // Glossy reflection (top half)
    juce::Path glossPath;
    glossPath.addEllipse(center.x - trackRadius * 0.9f, center.y - trackRadius * 0.9f, trackRadius * 1.8f, trackRadius * 1.0f);
    
    juce::ColourGradient glossGradient(
        juce::Colours::white.withAlpha(0.15f), center.x, center.y - trackRadius,
        juce::Colours::transparentWhite, center.x, center.y, false);
    
    g.setGradientFill(glossGradient);
    g.fillPath(glossPath);

    // Subtle rim highlight (bottom-right)
    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.drawEllipse(center.x - trackRadius, center.y - trackRadius, trackRadius * 2, trackRadius * 2, 1.0f);

    // 3. Pointer (White Dot)
    float currentAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    
    // Position dot slightly inside the rim
    float dotDistance = trackRadius * 0.75f; 
    float dotRadius = trackRadius * 0.08f; 
    
    float dotX = center.x + dotDistance * std::sin(currentAngle);
    float dotY = center.y - dotDistance * std::cos(currentAngle);
    
    // Dot shadow/glow
    juce::Path dotPath;
    dotPath.addEllipse(dotX - dotRadius, dotY - dotRadius, dotRadius * 2, dotRadius * 2);
    
    // Dot body
    g.setColour(juce::Colours::white);
    g.fillPath(dotPath);
}

// ============================================================================
// ParameterPanel Implementation
// ============================================================================

ParameterPanel::ParameterPanel()
{
    // ========== Pitch Correction Section ==========
    setupHeader(pitchCorrectionHeader_, LOC(kPitchCorrection));
    addAndMakeVisible(pitchCorrectionHeader_);

    setupLabel(retuneSpeedLabel_, LOC(kRetuneSpeed));
    retuneSpeedLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(retuneSpeedLabel_);

    setupLargeKnob(retuneSpeedSlider_, 0.0, 100.0, PitchControlConfig::kDefaultRetuneSpeedPercent, "%");
    retuneSpeedSlider_.getProperties().set("minimalKnob", true);
    retuneSpeedSlider_.onValueChange = [this] { onRetuneSpeedChanged(); };
    addAndMakeVisible(retuneSpeedSlider_);
    retuneSpeedSlider_.setTooltip(LOC(kTooltipRetuneSpeed));

    setupLabel(vibratoDepthLabel_, LOC(kVibratoDepth));
    vibratoDepthLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(vibratoDepthLabel_);

    setupLargeKnob(vibratoDepthSlider_, 0.0, 100.0, PitchControlConfig::kDefaultVibratoDepth, "%");
    vibratoDepthSlider_.getProperties().set("minimalKnob", true);
    vibratoDepthSlider_.onValueChange = [this] { onVibratoDepthChanged(); };
    addAndMakeVisible(vibratoDepthSlider_);
    vibratoDepthSlider_.setTooltip(LOC(kTooltipVibratoDepth));

    setupLabel(vibratoRateLabel_, LOC(kVibratoRate));
    vibratoRateLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(vibratoRateLabel_);

    setupLargeKnob(vibratoRateSlider_, 3.0, 12.0, PitchControlConfig::kDefaultVibratoRateHz, " Hz");
    vibratoRateSlider_.getProperties().set("minimalKnob", true);
    vibratoRateSlider_.onValueChange = [this] { onVibratoRateChanged(); };
    addAndMakeVisible(vibratoRateSlider_);
    vibratoRateSlider_.setTooltip(LOC(kTooltipVibratoRate));

    setupLabel(noteSplitLabel_, LOC(kNoteSplit));
    noteSplitLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(noteSplitLabel_);

    setupLargeKnob(noteSplitSlider_,
                   PitchControlConfig::kMinNoteSplitCents,
                   PitchControlConfig::kMaxNoteSplitCents,
                   PitchControlConfig::kDefaultNoteSplitCents,
                   " cents");
    noteSplitSlider_.getProperties().set("minimalKnob", true);
    noteSplitSlider_.onValueChange = [this] { onNoteSplitChanged(); };
    addAndMakeVisible(noteSplitSlider_);
    noteSplitSlider_.setTooltip(LOC(kTooltipNoteSplit));

    // ========== Tools Section (Replaces Info Display) ==========
    setupHeader(toolsHeader_, LOC(kTools));
    addAndMakeVisible(toolsHeader_);

    autoTuneToolButton_ = std::make_unique<ToolIconButton>(0, "Auto", LOC(kTooltipAutoTune) + "\n6");
    autoTuneToolButton_->setClickingTogglesState(false);
    autoTuneToolButton_->setTextIcon("AUTO");
    autoTuneToolButton_->onClick = [this] {
        listeners_.call([](Listener& l) { l.autoTuneRequested(); });
    };
    addAndMakeVisible(*autoTuneToolButton_);

    selectToolButton_ = std::make_unique<ToolIconButton>(1, "Select", LOC(kTooltipSelect) + "\n3");
    selectToolButton_->setRadioGroupId(1001);
    selectToolButton_->setToggleState(true, juce::dontSendNotification);
    selectToolButton_->setIcon(ToolbarIcons::getSelectIcon(), true);
    selectToolButton_->onClick = [this] { onToolClicked(1); };
    addAndMakeVisible(*selectToolButton_);

    drawNoteToolButton_ = std::make_unique<ToolIconButton>(2, "DrawNote", LOC(kTooltipDrawNote) + "\n2");
    drawNoteToolButton_->setRadioGroupId(1001);
    drawNoteToolButton_->setIcon(ToolbarIcons::getDrawNoteIcon(), false);
    drawNoteToolButton_->onClick = [this] { onToolClicked(2); };
    addAndMakeVisible(*drawNoteToolButton_);

    lineAnchorToolButton_ = std::make_unique<ToolIconButton>(3, "LineAnchor", LOC(kTooltipLineAnchor) + "\n4");
    lineAnchorToolButton_->setRadioGroupId(1001);
    lineAnchorToolButton_->setIcon(ToolbarIcons::getLineAnchorIcon(), false);
    lineAnchorToolButton_->onClick = [this] { onToolClicked(3); };
    addAndMakeVisible(*lineAnchorToolButton_);

    handDrawToolButton_ = std::make_unique<ToolIconButton>(4, "HandDraw", LOC(kTooltipHandDraw) + "\n5");
    handDrawToolButton_->setRadioGroupId(1001);
    handDrawToolButton_->setIcon(ToolbarIcons::getHandDrawIcon(), false);
    handDrawToolButton_->onClick = [this] { onToolClicked(4); };
    addAndMakeVisible(*handDrawToolButton_);

    // Pitch Shift action button
    pitchShiftButton_ = std::make_unique<juce::TextButton>("Pitch Shift...");
    pitchShiftButton_->setTooltip(juce::String::fromUTF8(u8"整体移调"));
    pitchShiftButton_->getProperties().set(UIColors::auroraChromeIntensityProperty,
                                           kAuroraSidebarChromeIntensity);
    pitchShiftButton_->onClick = [this] {
        listeners_.call(&Listener::pitchShiftRequested);
    };
    addAndMakeVisible(*pitchShiftButton_);

    // ⚡️ vocal-time-stretch §8.4 — Time tool palette button.
    // toolId=5 matches ToolId::TimeTool; tooltip uses 'T' shortcut to align
    // with PianoRollToolHandler::keyPressed binding.
    timeToolButton_ = std::make_unique<ToolIconButton>(5, "TimeTool", LOC(kTooltipTimeTool) + "\nT");
    timeToolButton_->setRadioGroupId(1001);
    timeToolButton_->setIcon(ToolbarIcons::getTimeToolIcon(), false);
    timeToolButton_->onClick = [this] { onToolClicked(5); };
    addAndMakeVisible(*timeToolButton_);
    timeToolButton_->setVisible(experimentalFeaturesEnabled_);
}

ParameterPanel::~ParameterPanel()
{
    retuneSpeedSlider_.setLookAndFeel(nullptr);
    vibratoDepthSlider_.setLookAndFeel(nullptr);
    vibratoRateSlider_.setLookAndFeel(nullptr);
    noteSplitSlider_.setLookAndFeel(nullptr);
}

void ParameterPanel::rebuildAuroraSidebarSurface(juce::Rectangle<float> bounds)
{
    const auto scale = getDesktopScaleFactor();
    const auto width = juce::roundToInt(bounds.getWidth() * scale);
    const auto height = juce::roundToInt(bounds.getHeight() * scale);

    if (auroraSidebarSurface_.getWidth() == width
        && auroraSidebarSurface_.getHeight() == height
        && auroraSidebarSurfaceScale_ == scale)
        return;

    auroraSidebarSurface_ = makeAuroraSidebarSurfaceCurve(width, height);
    auroraSidebarSurfaceScale_ = scale;
}

void ParameterPanel::paint(juce::Graphics& g)
{
    const auto& style = UIColors::currentThemeStyle();
    const auto themeId = UIColors::currentThemeId();
    // 阴影边距：背景在 reduced(12) 区域内绘制，阴影在边距内渲染
    const float shadowMargin = 12.0f;
    auto bounds = getLocalBounds().toFloat().reduced(shadowMargin);

    // Background Shadow (if needed, though MainControlPanel usually handles the main shadow)
    UIColors::drawShadow(g, bounds);

    if (themeId == ThemeId::Overdose)
    {
        UIColors::fillOverdosePanelBackground(g, bounds, style.panelRadius);

        const auto drawSectionHeader = [&](const juce::Label& header)
        {
            if (!header.isVisible())
                return;

            auto headerBounds = header.getBounds().toFloat();
            if (headerBounds.isEmpty())
                return;

            // 粉色半透明下划线，比之前更明显
            const float lineY = headerBounds.getBottom() - 1.5f;
            g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.32f));
            g.drawLine(headerBounds.getX() + 6.0f, lineY,
                       headerBounds.getRight() - 6.0f, lineY, 1.0f);

            // 标题文字柔光底层（label 自身 paint 在上层）
            g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.08f));
            g.drawText(header.getText(), headerBounds.translated(0.0f, -0.5f),
                       juce::Justification::centred);
        };

        drawSectionHeader(pitchCorrectionHeader_);
        drawSectionHeader(toolsHeader_);
        return;
    }

    if (themeId == ThemeId::Aurora)
    {
        UIColors::fillAuroraSidebarShell(g, bounds, style.panelRadius);
        rebuildAuroraSidebarSurface(bounds);
        {
            juce::Graphics::ScopedSaveState clipState(g);
            juce::Path panelShape;
            panelShape.addRoundedRectangle(bounds, style.panelRadius);
            g.reduceClipRegion(panelShape);
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImage(auroraSidebarSurface_, bounds, juce::RectanglePlacement::stretchToFit, false);
        }
        UIColors::drawAuroraSidebarShellFrame(g, bounds, style.panelRadius);
        return;
    }

    // Create rounded path for background and clipping
    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(bounds, style.panelRadius);
    g.reduceClipRegion(backgroundPath);
    
    // Fill Background
    UIColors::fillPanelBackground(g, bounds, style.panelRadius);
    
    // Draw Frame
    UIColors::drawPanelFrame(g, bounds, style.panelRadius);
}

void ParameterPanel::resized()
{
    auroraSidebarSurface_ = juce::Image();
    const auto themeId = UIColors::currentThemeId();
    const bool isBlueBreeze = (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose);

    // 旋钮尺寸：BlueBreeze 主题使用更大尺寸 (115px)，其他主题使用 92px
    const int knobSize = isBlueBreeze ? 115 : 92;

    // 阴影边距：内容区域在 reduced(12) 范围内布局
    // 再加上 8px 内边距 = 总共 reduced(20)
    const int shadowMargin = 12;
    const int innerPadding = 8;
    auto mainArea = getLocalBounds().reduced(shadowMargin + innerPadding);
    
    const int headerHeight = 24;
    const int labelHeight = 20;
    const int spacing = 12;
    const int toolButtonSize = 60;  // 放大 0.25 倍：48 * 1.25 = 60
    const int toolButtonGap = 10;   // 相应增大间距
    const int toolButtonHorizontalGap = 12; // 列间距相应增大
    const int toolHeaderGap = 8;
    const int rows = 3; // 使用3行布局（2×2网格 + 1个居中按钮）
    // Tools区域高度计算：header + gap + 3行按钮 + 2个行间距
    const int toolsHeight = headerHeight + toolHeaderGap + rows * toolButtonSize + (rows - 1) * toolButtonGap;

    // 向上平移 150px：先预留底部空间，再取出 Tools 区域
    mainArea.removeFromBottom(150);
    auto toolsArea = mainArea.removeFromBottom(toolsHeight);

    // Pitch Correction
    pitchCorrectionHeader_.setBounds(mainArea.removeFromTop(headerHeight));
    mainArea.removeFromTop(spacing);

    // 2x2 Grid Layout for Knobs
    const int rowHeight = labelHeight + knobSize + 8;
    
    auto layoutKnobCell = [&](juce::Rectangle<int> area, juce::Label& label, juce::Slider& slider) {
        label.setBounds(area.removeFromTop(labelHeight));
        slider.setBounds(area.reduced(4, 0));
    };

    // Row 1 (Retune Speed | Vibrato Depth)
    auto row1 = mainArea.removeFromTop(rowHeight);
    mainArea.removeFromTop(spacing);
    
    // Row 2 (Vibrato Rate | Note Split)
    auto row2 = mainArea.removeFromTop(rowHeight);
    mainArea.removeFromTop(spacing);

    const int colWidth = row1.getWidth() / 2;

    layoutKnobCell(row1.removeFromLeft(colWidth), retuneSpeedLabel_, retuneSpeedSlider_);
    layoutKnobCell(row1, vibratoDepthLabel_, vibratoDepthSlider_);
    
    layoutKnobCell(row2.removeFromLeft(colWidth), vibratoRateLabel_, vibratoRateSlider_);
    layoutKnobCell(row2, noteSplitLabel_, noteSplitSlider_);

    // Pitch Shift button — between knobs and tools
    const int pitchShiftButtonHeight = 28;
    pitchShiftButton_->setBounds(mainArea.removeFromTop(pitchShiftButtonHeight).reduced(4, 0));
    mainArea.removeFromTop(spacing);

    // Header
    toolsHeader_.setBounds(toolsArea.removeFromTop(headerHeight));
    toolsArea.removeFromTop(toolHeaderGap);

    // 工具按钮布局：2列×3行网格，AUTO居中在第3行
    auto toolsColumn = toolsArea.reduced(5, 0);
    int startY = toolsColumn.getY();

    // 按钮数组（vocal-time-stretch §8.4: 6 个按钮 = 3 行 × 2 列网格）
    std::vector<juce::Component*> buttons;
    if (selectToolButton_) buttons.push_back(selectToolButton_.get());        // 第1行第1列
    if (drawNoteToolButton_) buttons.push_back(drawNoteToolButton_.get());    // 第1行第2列
    if (lineAnchorToolButton_) buttons.push_back(lineAnchorToolButton_.get()); // 第2行第1列
    if (handDrawToolButton_) buttons.push_back(handDrawToolButton_.get());    // 第2行第2列
    if (timeToolButton_) buttons.push_back(timeToolButton_.get());            // 第3行第1列
    if (autoTuneToolButton_) buttons.push_back(autoTuneToolButton_.get());    // 第3行第2列

    // 2列×3行网格布局
    if (!experimentalFeaturesEnabled_ && timeToolButton_) {
        for (auto it = buttons.begin(); it != buttons.end();) {
            if (*it == timeToolButton_.get()) {
                it = buttons.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        int row = i / 2;
        int col = i % 2;
        // 计算2列网格的起始X坐标（居中对齐）
        int totalWidth = 2 * toolButtonSize + toolButtonHorizontalGap;
        int gridStartX = toolsColumn.getCentreX() - totalWidth / 2;
        int x = gridStartX + col * (toolButtonSize + toolButtonHorizontalGap);
        int y = startY + row * (toolButtonSize + toolButtonGap);
        buttons[i]->setBounds(x, y, toolButtonSize, toolButtonSize);
    }

}

void ParameterPanel::applyTheme()
{
    auroraSidebarSurface_ = juce::Image();
    pitchCorrectionHeader_.setColour(juce::Label::textColourId, UIColors::textPrimary);
    toolsHeader_.setColour(juce::Label::textColourId, UIColors::textPrimary);

    retuneSpeedLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
    vibratoDepthLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
    vibratoRateLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
    noteSplitLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);

    largeKnobLookAndFeel_.setColour(juce::Slider::textBoxTextColourId, UIColors::textPrimary);
    largeKnobLookAndFeel_.setColour(juce::Slider::textBoxBackgroundColourId, UIColors::backgroundDark);
    largeKnobLookAndFeel_.setColour(juce::Slider::textBoxOutlineColourId, UIColors::panelBorder);

    if (pitchShiftButton_) {
        pitchShiftButton_->setColour(juce::TextButton::buttonColourId, UIColors::backgroundMedium);
        pitchShiftButton_->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    }

    resized();
    repaint();
}

void ParameterPanel::refreshLocalizedText()
{
    // 更新 Pitch Correction 部分
    pitchCorrectionHeader_.setText(LOC(kPitchCorrection), juce::dontSendNotification);
    retuneSpeedLabel_.setText(LOC(kRetuneSpeed), juce::dontSendNotification);
    vibratoDepthLabel_.setText(LOC(kVibratoDepth), juce::dontSendNotification);
    vibratoRateLabel_.setText(LOC(kVibratoRate), juce::dontSendNotification);
    noteSplitLabel_.setText(LOC(kNoteSplit), juce::dontSendNotification);
    
    // 更新 Tools 部分
    toolsHeader_.setText(LOC(kTools), juce::dontSendNotification);
    
    // 更新工具按钮 tooltip
    setAutoButtonPresentation(autoButtonPresentation_);
    if (selectToolButton_)
        selectToolButton_->setTooltip(LOC(kTooltipSelect) + "\n3");
    if (drawNoteToolButton_)
        drawNoteToolButton_->setTooltip(LOC(kTooltipDrawNote) + "\n2");
    if (lineAnchorToolButton_)
        lineAnchorToolButton_->setTooltip(LOC(kTooltipLineAnchor) + "\n4");
    if (handDrawToolButton_)
        handDrawToolButton_->setTooltip(LOC(kTooltipHandDraw) + "\n5");
    if (timeToolButton_)
        timeToolButton_->setTooltip(LOC(kTooltipTimeTool) + "\nT");

    repaint();
}

void ParameterPanel::setPitchShiftIndicator(int semitone, int cents)
{
    if (pitchShiftButton_ == nullptr) return;
    if (semitone == 0 && cents == 0) {
        pitchShiftButton_->setButtonText("Pitch Shift...");
    } else {
        juce::String text = "Pitch Shift: ";
        if (semitone != 0) text += juce::String(semitone > 0 ? "+" : "") + juce::String(semitone) + "st";
        if (cents != 0) {
            if (semitone != 0) text += " ";
            text += juce::String(cents > 0 ? "+" : "") + juce::String(cents) + "c";
        }
        pitchShiftButton_->setButtonText(text);
    }
}

void ParameterPanel::setupHeader(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(UIColors::getHeaderFont(14.0f)); // Bold header font
    label.setColour(juce::Label::textColourId, UIColors::textPrimary);
    label.setJustificationType(juce::Justification::centred);
}

void ParameterPanel::setupLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(UIColors::getUIFont(12.0f));
    label.setColour(juce::Label::textColourId, UIColors::textSecondary);
}

void ParameterPanel::setupLargeKnob(juce::Slider& slider, double min, double max, double defaultVal, const juce::String& suffix)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    // 文本框略加宽加高，减少“薄片感”
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 28);
    slider.setRange(min, max, 0.01);
    slider.setValue(defaultVal);
    slider.setRotaryParameters(juce::degreesToRadians(210.0f), juce::degreesToRadians(510.0f), true);
    slider.setDoubleClickReturnValue(true, defaultVal);
    slider.setTextValueSuffix(suffix);
    slider.setLookAndFeel(&largeKnobLookAndFeel_);
}

void ParameterPanel::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void ParameterPanel::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void ParameterPanel::setActiveTool(int toolId)
{
    if (selectToolButton_) selectToolButton_->setToggleState(toolId == 1, juce::dontSendNotification);
    if (drawNoteToolButton_) drawNoteToolButton_->setToggleState(toolId == 2, juce::dontSendNotification);
    if (lineAnchorToolButton_) lineAnchorToolButton_->setToggleState(toolId == 3, juce::dontSendNotification);
    if (handDrawToolButton_) handDrawToolButton_->setToggleState(toolId == 4, juce::dontSendNotification);
    if (timeToolButton_) timeToolButton_->setToggleState(experimentalFeaturesEnabled_ && toolId == 5,
                                                         juce::dontSendNotification);
}

void ParameterPanel::setExperimentalFeaturesEnabled(bool enabled)
{
    if (experimentalFeaturesEnabled_ == enabled) {
        return;
    }

    experimentalFeaturesEnabled_ = enabled;
    if (timeToolButton_ != nullptr) {
        if (!enabled) {
            timeToolButton_->setToggleState(false, juce::dontSendNotification);
            timeToolButton_->setBounds({});
        }
        timeToolButton_->setVisible(enabled);
    }
    resized();
    repaint();
}

void ParameterPanel::setAutoButtonPresentation(const AutoButtonPresentation& presentation)
{
    autoButtonPresentation_ = presentation;
    if (autoTuneToolButton_ == nullptr) {
        return;
    }

    autoTuneToolButton_->setTextIcon("AUTO");
    const bool hasReference = autoButtonPresentation_.mode == AutoButtonPresentation::Mode::ReferenceAuto;
    const auto resolvedTooltip = buildAutoButtonTooltip(autoButtonPresentation_);
    {
        if (hasReference)
        {
            autoTuneToolButton_->setSubTextIcon("(Ref)");
            autoTuneToolButton_->setTooltip(juce::String::fromUTF8(u8"按参考 Clip 自动修音并对齐节奏"));
        }
        else
        {
            autoTuneToolButton_->setSubTextIcon({});
            autoTuneToolButton_->setTooltip(juce::String::fromUTF8(u8"自动修音（吸附到临近音阶）"));
            autoTuneToolButton_->setTooltip(resolvedTooltip);
        }
    }
}

// Getters and Setters

void ParameterPanel::setRetuneSpeed(float speed)
{
    retuneSpeedSlider_.setValue(speed, juce::dontSendNotification);
}

float ParameterPanel::getRetuneSpeed() const
{
    return static_cast<float>(retuneSpeedSlider_.getValue());
}

void ParameterPanel::setVibratoDepth(float value)
{
    vibratoDepthSlider_.setValue(value, juce::dontSendNotification);
}

float ParameterPanel::getVibratoDepth() const
{
    return static_cast<float>(vibratoDepthSlider_.getValue());
}

void ParameterPanel::setVibratoRate(float value)
{
    vibratoRateSlider_.setValue(value, juce::dontSendNotification);
}

float ParameterPanel::getVibratoRate() const
{
    return static_cast<float>(vibratoRateSlider_.getValue());
}

void ParameterPanel::setNoteSplit(float value)
{
    noteSplitSlider_.setValue(value, juce::dontSendNotification);
}

float ParameterPanel::getNoteSplit() const
{
    return static_cast<float>(noteSplitSlider_.getValue());
}

void ParameterPanel::onRetuneSpeedChanged()
{
    listeners_.call([this](Listener& l) { l.retuneSpeedChanged(static_cast<float>(retuneSpeedSlider_.getValue())); });
}

void ParameterPanel::onVibratoDepthChanged()
{
    listeners_.call([this](Listener& l) { l.vibratoDepthChanged(static_cast<float>(vibratoDepthSlider_.getValue())); });
}

void ParameterPanel::onVibratoRateChanged()
{
    listeners_.call([this](Listener& l) { l.vibratoRateChanged(static_cast<float>(vibratoRateSlider_.getValue())); });
}

void ParameterPanel::onNoteSplitChanged()
{
    listeners_.call([this](Listener& l) { l.noteSplitChanged(static_cast<float>(noteSplitSlider_.getValue())); });
}

void ParameterPanel::onToolClicked(int toolId)
{
    listeners_.call([this, toolId](Listener& l) { l.toolSelected(toolId); });
}

} // namespace OpenTune

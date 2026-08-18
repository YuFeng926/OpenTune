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
        const auto radius = UIColors::currentThemeStyle().controlRadius;

        // 阴影（增强立体感）
        juce::DropShadow softShadow;
        softShadow.colour = juce::Colour(Overdose::Colors::SoftShadow).withAlpha(active ? 0.25f : 0.18f);
        softShadow.radius = active ? 10 : 8;
        softShadow.offset = { 0, active ? 3 : 2 };
        juce::Path shape;
        shape.addRoundedRectangle(bounds, radius);
        softShadow.drawForPath(g, shape);

        // 主体渐变
        if (active)
        {
            // 激活：粉色玻璃渐变（参考图：饱满的玻璃质感粉色，更强渐变，极饱和）
            juce::ColourGradient fill(juce::Colour(0xFFFFFAFE),  // 顶部极亮粉
                                       bounds.getX(), bounds.getY(),
                                       juce::Colour(0xFFD80050),  // 底部深粉
                                       bounds.getX(), bounds.getBottom(),
                                       false);
            fill.addColour(0.05f, juce::Colour(0xFFFFD0F8));
            fill.addColour(0.35f, juce::Colour(0xFFFFA0E8));
            g.setGradientFill(fill);
            g.fillRoundedRectangle(bounds, radius);

            // 顶部玻璃高光（更强，覆盖上半部分，明显反光）
            auto highlightRect = bounds.reduced(1.0f).withHeight(bounds.getHeight() * 0.65f);
            juce::ColourGradient hl(juce::Colour(0xFFFFFFFF).withAlpha(0.90f),
                                     highlightRect.getX(), highlightRect.getY(),
                                     juce::Colours::transparentWhite,
                                     highlightRect.getX(), highlightRect.getBottom(), false);
            g.setGradientFill(hl);
            g.fillRoundedRectangle(highlightRect, radius - 1.0f);

            // 顶部镜面高光点（增强玻璃感，更大更亮）
            {
                juce::Graphics::ScopedSaveState clip(g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle(bounds, radius);
                g.reduceClipRegion(clipPath);
                auto specular = juce::Rectangle<float>(bounds.getX() + bounds.getWidth() * 0.10f,
                                                       bounds.getY() + bounds.getHeight() * 0.04f,
                                                       bounds.getWidth() * 0.60f,
                                                       bounds.getHeight() * 0.28f);
                juce::ColourGradient spec(
                    juce::Colour(0xFFFFFFFF).withAlpha(0.98f),
                    specular.getTopLeft(),
                    juce::Colours::transparentWhite,
                    specular.getBottomRight(), false);
                g.setGradientFill(spec);
                g.fillEllipse(specular);
            }

            // 边框（深粉，更强）
            g.setColour(juce::Colour(0xFFE01080).withAlpha(0.70f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
        }
        else
        {
            // 普通：玻璃质感（白→淡紫，更通透）
            juce::ColourGradient bg(juce::Colour(0xFFFFFCFF).withAlpha(0.92f),
                                     bounds.getX(), bounds.getY(),
                                     juce::Colour(0xFFF0E8F8).withAlpha(0.88f),
                                     bounds.getX(), bounds.getBottom(), false);
            g.setGradientFill(bg);
            g.fillRoundedRectangle(bounds, radius);

            // 顶部高光（更强）
            auto highlightRect = bounds.reduced(1.0f).withHeight(bounds.getHeight() * 0.45f);
            juce::ColourGradient hl(juce::Colour(0xFFFFFFFF).withAlpha(0.50f),
                                     highlightRect.getX(), highlightRect.getY(),
                                     juce::Colours::transparentWhite,
                                     highlightRect.getX(), highlightRect.getBottom(), false);
            g.setGradientFill(hl);
            g.fillRoundedRectangle(highlightRect, radius - 1.0f);

            // 边框（淡粉）
            if (shouldDrawButtonAsHighlighted)
            {
                g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.45f));
                g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
            }
            else
            {
                g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.30f));
                g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 0.8f);
            }
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

    const auto active = themeId == ThemeId::Overdose && (getToggleState() || shouldDrawButtonAsDown);
    const bool isOverdose = (themeId == ThemeId::Overdose);

    if (!iconPath_.isEmpty() && textIcon_.isNotEmpty())
    {
        // 图标 + 文本组合模式：上半部分图标，下半部分文本
        auto fullBounds = getLocalBounds().toFloat();
        auto iconArea = fullBounds.removeFromTop(fullBounds.getHeight() * 0.55f).reduced(4.0f);
        auto textArea = fullBounds;

        auto iconColor = active ? juce::Colours::white.withAlpha(0.96f)
                                : (isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.85f)
                                              : UIColors::textPrimary);
        ToolbarIcons::drawIcon(g, iconPath_, iconArea, iconColor, 2.0f, fillIcon_);

        if (subTextIcon_.isNotEmpty())
        {
            auto mainTextArea = textArea.removeFromTop(textArea.getHeight() * 0.6f);
            g.setColour(active ? juce::Colours::white.withAlpha(0.96f)
                               : (isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.90f)
                                             : UIColors::textPrimary));
            g.setFont(UIColors::getUIFont(12.0f));
            g.drawText(textIcon_, mainTextArea, juce::Justification::centredBottom);

            g.setColour(UIColors::textSecondary);
            g.setFont(UIColors::getUIFont(9.0f));
            g.drawText(subTextIcon_, textArea, juce::Justification::centredTop);
        }
        else
        {
            g.setColour(active ? juce::Colours::white.withAlpha(0.96f)
                               : (isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.90f)
                                             : UIColors::textPrimary));
            g.setFont(UIColors::getUIFont(12.0f));
            g.drawText(textIcon_, textArea, juce::Justification::centred);
        }
    }
    else if (textIcon_.isNotEmpty())
    {
        if (subTextIcon_.isNotEmpty())
        {
            auto textArea = getLocalBounds().toFloat();
            const float mainFontSize = 16.0f;
            const float subFontSize = 11.0f;

            auto mainArea = textArea.removeFromTop(textArea.getHeight() * 0.55f);
            g.setColour(active ? juce::Colours::white.withAlpha(0.96f)
                               : (isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.90f)
                                             : UIColors::textPrimary));
            g.setFont(UIColors::getUIFont(mainFontSize));
            g.drawText(textIcon_, mainArea, juce::Justification::centredBottom);

            g.setColour(UIColors::textSecondary);
            g.setFont(UIColors::getUIFont(subFontSize));
            g.drawText(subTextIcon_, textArea, juce::Justification::centredTop);
        }
        else
        {
            g.setColour(active ? juce::Colours::white.withAlpha(0.96f)
                               : (isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.90f)
                                             : UIColors::textPrimary));
            g.setFont(UIColors::getUIFont(16.0f));
            g.drawText(textIcon_, getLocalBounds().toFloat(), juce::Justification::centred);
        }
    }
    else if (!iconPath_.isEmpty())
    {
        auto iconArea = getLocalBounds().toFloat().reduced(10.0f);
        auto iconColor = active ? juce::Colours::white.withAlpha(0.96f)
                                : (isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.85f)
                                              : UIColors::textPrimary);

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
                                   rotaryEndAngle,
                                   &slider);
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
                                 rotaryEndAngle,
                                 &slider);
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
                                          rotaryEndAngle,
                                          &slider);
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
    // OpenTune 初态隐藏：OpenDyne 始终可见可选，OpenTune 受 experimental 控制
    addChildComponent(*timeToolButton_);

    // ── OpenDyne 工具按钮（OpenDyne 专属，OpenTune 初态隐藏） ──
    pitchToolButton_ = std::make_unique<ToolIconButton>(6, "Pitch", juce::String::fromUTF8(u8"Pitch 音高编辑\nF2"));
    pitchToolButton_->setRadioGroupId(1001);
    pitchToolButton_->setIcon(ToolbarIcons::getPitchToolIcon(), false);
    pitchToolButton_->onClick = [this] { onToolClicked(6); };
    addChildComponent(*pitchToolButton_);

    volumeEnvelopeToolButton_ = std::make_unique<ToolIconButton>(7, "VolumeEnvelope", juce::String::fromUTF8(u8"Volume Envelope 音量包络\nF4"));
    volumeEnvelopeToolButton_->setRadioGroupId(1001);
    volumeEnvelopeToolButton_->setIcon(ToolbarIcons::getVolumeEnvelopeToolIcon(), false);
    volumeEnvelopeToolButton_->onClick = [this] { onToolClicked(7); };
    addChildComponent(*volumeEnvelopeToolButton_);

    scissorsToolButton_ = std::make_unique<ToolIconButton>(8, "Scissors", juce::String::fromUTF8(u8"Scissors 切割音符\nF6"));
    scissorsToolButton_->setRadioGroupId(1001);
    scissorsToolButton_->setIcon(ToolbarIcons::getScissorsToolIcon(), false);
    scissorsToolButton_->onClick = [this] { onToolClicked(8); };
    addChildComponent(*scissorsToolButton_);

    pitchModulationToolButton_ = std::make_unique<ToolIconButton>(9, "PitchModulation", LOC(kTooltipPitchModulation) + "\nF2x2");
    pitchModulationToolButton_->setRadioGroupId(1001);
    pitchModulationToolButton_->setIcon(ToolbarIcons::getPitchModulationToolIcon(), false);
    pitchModulationToolButton_->onClick = [this] { onToolClicked(9); };
    addChildComponent(*pitchModulationToolButton_);

    pitchDriftToolButton_ = std::make_unique<ToolIconButton>(10, "PitchDrift", LOC(kTooltipPitchDrift) + "\nF2x3");
    pitchDriftToolButton_->setRadioGroupId(1001);
    pitchDriftToolButton_->setIcon(ToolbarIcons::getPitchDriftToolIcon(), false);
    pitchDriftToolButton_->onClick = [this] { onToolClicked(10); };
    addChildComponent(*pitchDriftToolButton_);

    // EQ tool button (visible in both OpenTune and OpenDyne modes)
    eqToolButton_ = std::make_unique<ToolIconButton>(11, "EQ", juce::String::fromUTF8(u8"EQ 频率均衡\nE"));
    eqToolButton_->setRadioGroupId(1001);
    eqToolButton_->setIcon(ToolbarIcons::getEqIcon(), false);
    eqToolButton_->setTextIcon("EQ");
    eqToolButton_->onClick = [this] { onToolClicked(11); };
    addAndMakeVisible(*eqToolButton_);

    // ── Pitch Grid 模式选择器（OpenDyne 专属，初态隐藏） ──
    pitchGridSelector_.addItem(u8"No Snap", 1);
    pitchGridSelector_.addItem(u8"Chromatic", 2);
    pitchGridSelector_.addItem(u8"Key Scale", 3);
    pitchGridSelector_.setSelectedId(3, juce::dontSendNotification);  // 默认 Key Scale
    pitchGridSelector_.onChange = [this] {
        if (onPitchGridModeChanged) {
            int id = pitchGridSelector_.getSelectedId();
            onPitchGridModeChanged(static_cast<PitchGridMode>(id - 1));
        }
    };
    pitchGridSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    pitchGridSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    pitchGridSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);
    pitchGridSelector_.getProperties().set("noArrow", true);
    pitchGridSelector_.getProperties().set("fontHeight", UIColors::navFontHeight);
    pitchGridSelector_.setJustificationType(juce::Justification::centred);
    addChildComponent(pitchGridSelector_);
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
    const int toolButtonSize = 60;
    const int toolButtonGap = 10;
    const int toolButtonHorizontalGap = 12;
    const int toolHeaderGap = 8;
    const int pitchShiftButtonHeight = 28;
    const int rowHeight = labelHeight + knobSize + 8;

    const int contentLeft = mainArea.getX();
    const int contentWidth = mainArea.getWidth();
    const int contentCentre = mainArea.getCentreX();
    const int colWidth = contentWidth / 2;

    // ══════════════════════════════════════════════════════════════════
    // 绝对坐标 y 游标排布 —— 不消费矩形、不 clamp 尺寸
    // 控件 setBounds 永远给完整尺寸，高度不足时超出部分被父组件裁切
    // ══════════════════════════════════════════════════════════════════
    int y = mainArea.getY();

    // ── Pitch Correction 标题 ──
    pitchCorrectionHeader_.setBounds(contentLeft, y, contentWidth, headerHeight);
    y += headerHeight + spacing;

    // ── Row 1: Retune Speed | Vibrato Depth ──
    retuneSpeedLabel_.setBounds(contentLeft, y, colWidth, labelHeight);
    retuneSpeedSlider_.setBounds(contentLeft + 4, y + labelHeight, colWidth - 8, rowHeight - labelHeight);

    vibratoDepthLabel_.setBounds(contentLeft + colWidth, y, contentWidth - colWidth, labelHeight);
    vibratoDepthSlider_.setBounds(contentLeft + colWidth + 4, y + labelHeight, contentWidth - colWidth - 8, rowHeight - labelHeight);

    y += rowHeight + spacing;

    // ── Row 2: Vibrato Rate | Note Split ──
    vibratoRateLabel_.setBounds(contentLeft, y, colWidth, labelHeight);
    vibratoRateSlider_.setBounds(contentLeft + 4, y + labelHeight, colWidth - 8, rowHeight - labelHeight);

    noteSplitLabel_.setBounds(contentLeft + colWidth, y, contentWidth - colWidth, labelHeight);
    noteSplitSlider_.setBounds(contentLeft + colWidth + 4, y + labelHeight, contentWidth - colWidth - 8, rowHeight - labelHeight);

    y += rowHeight + spacing;

    // ── Pitch Shift 按钮 ──
    pitchShiftButton_->setBounds(contentLeft + 4, y, contentWidth - 8, pitchShiftButtonHeight);
    y += pitchShiftButtonHeight + spacing;

    // ── 工具区标题 ──
    toolsHeader_.setBounds(contentLeft, y, contentWidth, headerHeight);
    y += headerHeight + toolHeaderGap;

    // ── 工具按钮网格：2 列居中 ──
    const int totalGridWidth = 2 * toolButtonSize + toolButtonHorizontalGap;
    const int gridStartX = contentCentre - totalGridWidth / 2;

    if (openDyneMode_)
    {
        // OpenDyne：9 个按钮 = 5 行 × 2 列
        // Melodyne 纵向顺序：Select(F1)、Pitch(F2)、Modulation(F2×2)、Drift(F2×3)、
        // VolumeEnvelope(F4)、Time(T)、Scissors(F6)，AUTO 瞬时命令收尾，EQ 收尾
        std::vector<juce::Component*> buttons = {
            selectToolButton_.get(),
            pitchToolButton_.get(),
            pitchModulationToolButton_.get(),
            pitchDriftToolButton_.get(),
            volumeEnvelopeToolButton_.get(),
            timeToolButton_.get(),
            scissorsToolButton_.get(),
            autoTuneToolButton_.get(),
            eqToolButton_.get(),
        };

        for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
        {
            const int row = i / 2;
            const int col = i % 2;
            const int bx = gridStartX + col * (toolButtonSize + toolButtonHorizontalGap);
            const int by = y + row * (toolButtonSize + toolButtonGap);
            buttons[static_cast<size_t>(i)]->setBounds(bx, by, toolButtonSize, toolButtonSize);
        }

        // Pitch Grid 选择器：网格最后一行下方 14px
        const int gridRows = (static_cast<int>(buttons.size()) + 1) / 2;
        const int gridBottom = y + gridRows * toolButtonSize + (gridRows - 1) * toolButtonGap;
        pitchGridSelector_.setBounds(contentLeft + 5, gridBottom + 14, contentWidth - 10, 22);
    }
    else
    {
        // OpenTune：按钮数组构造（与 experimental 过滤逻辑保持现状）
        std::vector<juce::Component*> buttons;
        if (selectToolButton_) buttons.push_back(selectToolButton_.get());
        if (drawNoteToolButton_) buttons.push_back(drawNoteToolButton_.get());
        if (lineAnchorToolButton_) buttons.push_back(lineAnchorToolButton_.get());
        if (handDrawToolButton_) buttons.push_back(handDrawToolButton_.get());
        if (timeToolButton_) buttons.push_back(timeToolButton_.get());
        if (autoTuneToolButton_) buttons.push_back(autoTuneToolButton_.get());
        if (eqToolButton_) buttons.push_back(eqToolButton_.get());

        if (!experimentalFeaturesEnabled_ && timeToolButton_)
        {
            for (auto it = buttons.begin(); it != buttons.end();)
            {
                if (*it == timeToolButton_.get())
                    it = buttons.erase(it);
                else
                    ++it;
            }
        }

        for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
        {
            const int row = i / 2;
            const int col = i % 2;
            const int bx = gridStartX + col * (toolButtonSize + toolButtonHorizontalGap);
            const int by = y + row * (toolButtonSize + toolButtonGap);
            buttons[i]->setBounds(bx, by, toolButtonSize, toolButtonSize);
        }
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

    pitchGridSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    pitchGridSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    pitchGridSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);

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
        selectToolButton_->setTooltip(LOC(kTooltipSelect) + (openDyneMode_ ? "\nF1" : "\n3"));
    if (drawNoteToolButton_)
        drawNoteToolButton_->setTooltip(LOC(kTooltipDrawNote) + "\n2");
    if (lineAnchorToolButton_)
        lineAnchorToolButton_->setTooltip(LOC(kTooltipLineAnchor) + "\n4");
    if (handDrawToolButton_)
        handDrawToolButton_->setTooltip(LOC(kTooltipHandDraw) + "\n5");
    if (timeToolButton_)
        timeToolButton_->setTooltip(LOC(kTooltipTimeTool) + "\nT");
    if (pitchToolButton_)
        pitchToolButton_->setTooltip(juce::String::fromUTF8(u8"Pitch 音高编辑\nF2"));
    if (volumeEnvelopeToolButton_)
        volumeEnvelopeToolButton_->setTooltip(juce::String::fromUTF8(u8"Volume Envelope 音量包络\nF4"));
    if (scissorsToolButton_)
        scissorsToolButton_->setTooltip(juce::String::fromUTF8(u8"Scissors 切割音符\nF6"));
    if (pitchModulationToolButton_)
        pitchModulationToolButton_->setTooltip(LOC(kTooltipPitchModulation) + "\nF2x2");
    if (pitchDriftToolButton_)
        pitchDriftToolButton_->setTooltip(LOC(kTooltipPitchDrift) + "\nF2x3");

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
    if (timeToolButton_) timeToolButton_->setToggleState(toolId == 5, juce::dontSendNotification);
    // OpenDyne 工具
    if (pitchToolButton_) pitchToolButton_->setToggleState(toolId == 6, juce::dontSendNotification);
    if (volumeEnvelopeToolButton_) volumeEnvelopeToolButton_->setToggleState(toolId == 7, juce::dontSendNotification);
    if (scissorsToolButton_) scissorsToolButton_->setToggleState(toolId == 8, juce::dontSendNotification);
    if (pitchModulationToolButton_) pitchModulationToolButton_->setToggleState(toolId == 9, juce::dontSendNotification);
    if (pitchDriftToolButton_) pitchDriftToolButton_->setToggleState(toolId == 10, juce::dontSendNotification);
    if (eqToolButton_) eqToolButton_->setToggleState(toolId == 11, juce::dontSendNotification);
}

void ParameterPanel::setExperimentalFeaturesEnabled(bool enabled)
{
    if (experimentalFeaturesEnabled_ == enabled) {
        return;
    }

    experimentalFeaturesEnabled_ = enabled;
    if (timeToolButton_ != nullptr) {
        // Time 在 OpenDyne 始终可见可选，不受 experimental 影响；OpenTune 继续受 experimental 控制
        if (!enabled && !openDyneMode_) {
            timeToolButton_->setToggleState(false, juce::dontSendNotification);
            timeToolButton_->setBounds({});
        }
        timeToolButton_->setVisible(openDyneMode_ || enabled);
    }
    resized();
    repaint();
}

void ParameterPanel::setOpenDyneMode(bool enabled)
{
    if (openDyneMode_ == enabled) {
        return;
    }

    openDyneMode_ = enabled;

    // Select 与 AUTO 在两种布局均保留，恒可见
    // OpenTune 专属工具：DrawNote/LineAnchor 只在 OpenTune 布局显示；HandDraw 两种模式均可见
    if (drawNoteToolButton_)    drawNoteToolButton_->setVisible(!enabled);
    if (lineAnchorToolButton_)  lineAnchorToolButton_->setVisible(!enabled);

    // OpenDyne 专属工具：Pitch/PitchModulation/PitchDrift/VolumeEnvelope/Scissors 只在 OpenDyne 布局显示
    if (pitchToolButton_)                pitchToolButton_->setVisible(enabled);
    if (pitchModulationToolButton_)      pitchModulationToolButton_->setVisible(enabled);
    if (pitchDriftToolButton_)           pitchDriftToolButton_->setVisible(enabled);
    if (volumeEnvelopeToolButton_)       volumeEnvelopeToolButton_->setVisible(enabled);
    if (scissorsToolButton_)             scissorsToolButton_->setVisible(enabled);
    pitchGridSelector_.setVisible(enabled);

    // Time 在 OpenDyne 始终可见可选；在 OpenTune 受 experimental 开关控制；切换时保持当前选择
    if (timeToolButton_)
        timeToolButton_->setVisible(enabled || experimentalFeaturesEnabled_);

    // Select tooltip 的快捷键随 scheme 切换（OpenDyne F1 / OpenTune 3）
    refreshLocalizedText();
    resized();
    repaint();
}

void ParameterPanel::setAutoButtonPresentation(const AutoButtonPresentation& presentation)
{
    autoButtonPresentation_ = presentation;
    if (autoTuneToolButton_ == nullptr) {
        return;
    }

    // OpenTune（CorrectedF0Primary）显示 AUTO；OpenDyne（NotesPrimary）显示 SNAP。
    // setOpenDyneMode → refreshLocalizedText → setAutoButtonPresentation 自动同步。
    autoTuneToolButton_->setTextIcon(openDyneMode_ ? "SNAP" : "AUTO");
    const bool hasReference = autoButtonPresentation_.mode == AutoButtonPresentation::Mode::ReferenceAuto;
    const auto resolvedTooltip = buildAutoButtonTooltip(autoButtonPresentation_);
    if (hasReference)
    {
        autoTuneToolButton_->setSubTextIcon("(Ref)");
    }
    else
    {
        autoTuneToolButton_->setSubTextIcon({});
    }
    autoTuneToolButton_->setTooltip(resolvedTooltip);
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

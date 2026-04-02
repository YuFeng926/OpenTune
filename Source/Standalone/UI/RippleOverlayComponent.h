#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <deque>
#include "../Utils/MouseTrailConfig.h"
#include "UIColors.h"
#include "ThemeTokens.h"

namespace OpenTune {

class RippleOverlayComponent : public juce::Component, public juce::Timer
{
public:
    struct Ripple
    {
        float x, y;
        float radius;
        float life;
    };

    struct TrailPoint
    {
        juce::Point<float> pos;
        int age;
    };

    RippleOverlayComponent()
    {
        setInterceptsMouseClicks(false, false);
        juce::Desktop::getInstance().addGlobalMouseListener(this);
        startTimerHz(30);
    }

    ~RippleOverlayComponent() override
    {
        stopTimer();
        juce::Desktop::getInstance().removeGlobalMouseListener(this);
    }

    void paint(juce::Graphics& g) override
    {
        if (!MouseTrailConfig::isEnabled())
            return;
            
        paintTrail(g);
        paintRipples(g);
    }

    void paintTrail(juce::Graphics& g)
    {
        if (trailPoints.size() < 2)
            return;

        auto theme = MouseTrailConfig::getTheme();
        auto style = MouseTrailConfig::getThemeStyle(theme);
        
        int totalPoints = static_cast<int>(trailPoints.size());
        for (int i = 0; i < totalPoints - 1; ++i)
        {
            float ageRatio = static_cast<float>(trailPoints[i].age) / 6.0f;
            float alpha = (1.0f - ageRatio) * 0.6f;
            float thickness = style.thickness * (1.0f - ageRatio * 0.7f);

            juce::Colour trailColor;
            
            if (style.useGradient)
            {
                float hue = std::fmod(style.hueShift + ageRatio * 0.15f, 1.0f);
                trailColor = style.accentColor.withHue(hue).withAlpha(alpha);
            }
            else
            {
                trailColor = style.baseColor.withAlpha(alpha);
            }

            juce::Path segment;
            segment.startNewSubPath(trailPoints[i].pos.x, trailPoints[i].pos.y);
            segment.lineTo(trailPoints[i + 1].pos.x, trailPoints[i + 1].pos.y);

            g.setColour(trailColor);
            g.strokePath(segment, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    void paintRipples(juce::Graphics& g)
    {
        auto theme = MouseTrailConfig::getTheme();
        auto style = MouseTrailConfig::getThemeStyle(theme);
        
        for (const auto& r : ripples)
        {
            float alpha = r.life * 0.3f;
            float lineThickness = 1.5f * r.life;

            juce::Colour rippleColor;
            
            if (style.useGradient)
            {
                rippleColor = style.accentColor.withAlpha(alpha);
            }
            else
            {
                rippleColor = style.baseColor.withAlpha(alpha);
            }

            g.setColour(rippleColor);
            g.drawEllipse(r.x - r.radius, r.y - r.radius, r.radius * 2, r.radius * 2, lineThickness);
        }
    }

    void timerCallback() override
    {
        if (!MouseTrailConfig::isEnabled())
        {
            if (!trailPoints.empty() || !ripples.empty())
            {
                trailPoints.clear();
                ripples.clear();
                repaint();
            }
            return;
        }

        bool stateChanged = false;
        
        auto style = MouseTrailConfig::getThemeStyle(MouseTrailConfig::getTheme());

        for (auto it = ripples.begin(); it != ripples.end(); )
        {
            const float prevLife = it->life;
            it->life -= style.fadeSpeed;
            it->radius *= 1.08f;

            if (it->life <= 0.0f)
            {
                it = ripples.erase(it);
                stateChanged = true;
            }
            else
            {
                if (it->life != prevLife)
                    stateChanged = true;
                ++it;
            }
        }

        for (auto& tp : trailPoints)
        {
            tp.age++;
        }

        while (!trailPoints.empty() && trailPoints.front().age >= 6)
        {
            trailPoints.pop_front();
            stateChanged = true;
        }

        auto mousePos = juce::Desktop::getInstance().getMousePosition();
        auto localPos = getLocalPoint(nullptr, mousePos).toFloat();

        bool shouldDrawTrail = true;
        if (auto* componentUnderMouse = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse())
        {
            if (shouldIgnoreComponent(componentUnderMouse))
                shouldDrawTrail = false;
        }

        if (getLocalBounds().toFloat().contains(localPos) && shouldDrawTrail)
        {
            if (localPos.getDistanceFrom(lastPos) > 1.5f)
            {
                trailPoints.push_back({ localPos, 0 });
                lastPos = localPos;
                stateChanged = true;
            }
        }

        if (stateChanged)
            repaint();
    }

    void mouseMove(const juce::MouseEvent&) override {}

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!MouseTrailConfig::isEnabled())
            return;
            
        if (auto* componentUnderMouse = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse())
        {
            if (shouldIgnoreComponent(componentUnderMouse))
                return;
        }

        auto screenPos = e.getScreenPosition();
        auto localPos = getLocalPoint(nullptr, screenPos);

        if (getLocalBounds().contains(localPos.toInt()))
        {
            Ripple r;
            r.x = static_cast<float>(localPos.x);
            r.y = static_cast<float>(localPos.y);
            r.radius = 6.0f;
            r.life = 1.0f;
            ripples.push_back(r);
        }
    }

private:
    bool shouldIgnoreComponent(juce::Component* c)
    {
        while (c != nullptr)
        {
            if (c->getProperties().contains("minimalKnob") && static_cast<bool>(c->getProperties()["minimalKnob"]))
                return true;
            c = c->getParentComponent();
        }
        return false;
    }

    std::deque<TrailPoint> trailPoints;
    std::vector<Ripple> ripples;
    juce::Point<float> lastPos;
};

} // namespace OpenTune
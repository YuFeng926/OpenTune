#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <map>
#include <mutex>

#include "BinaryData.h"

namespace OpenTune {

enum class UiAssetId
{
    BackgroundMain,
    PanelEditorMain,
    PanelParameterSidebar,
    PanelTrackColumn,
    PanelTrackCard,
    PanelDisplayPanel,
    ToolbarTopbarButtonIdle,
    ToolbarTopbarButtonActive,
    TransportButtonShell,
    ToolbarTransportTimeField,
    ToolbarBpmField,
    ToolbarDropdownNarrowField,
    ToolbarDropdownWideField,
    ToolRightPanelButtonShell,
    ToolRightPanelAutoButton,
    ParameterSmallReadout,
    SliderTrack,
    SliderThumb,
    ScrollbarTrack,
    ScrollbarThumb,
    TabSegmentTrack,
    TabSegmentActiveShell,
    PianoKeyAtlas,
    KnobPrimaryFilmstrip,
    KnobLargeParameterFilmstrip
};

struct UiAssets
{
    static juce::Typeface::Ptr createHonorSansTypeface()
    {
        return juce::Typeface::createSystemTypefaceFor(BinaryData::HONORSansCNMedium_ttf,
                                                       BinaryData::HONORSansCNMedium_ttfSize);
    }

    static const juce::Image& get(UiAssetId assetId)
    {
        return get(assetId, getRecommendedScaleFactor());
    }

    static const juce::Image& get(UiAssetId assetId, float scaleFactor)
    {
        const auto scaleBucket = chooseScaleBucket(scaleFactor);
        const auto preferredResourceName = buildNamedResourceId(assetId, scaleBucket);
        const auto fallbackResourceName = buildNamedResourceId(assetId, 1);

        if (hasNamedResource(preferredResourceName))
            return getCachedImage(preferredResourceName);

        return getCachedImage(fallbackResourceName);
    }

    static void drawAssetStretch(juce::Graphics& g,
                                 UiAssetId assetId,
                                 juce::Rectangle<float> bounds,
                                 float opacity = 1.0f)
    {
        const auto& image = get(assetId, getRecommendedScaleFactor());
        if (!image.isValid() || bounds.isEmpty())
        {
            jassertfalse;
            return;
        }

        drawImageSection(g,
                         image,
                         bounds,
                         { 0, 0, image.getWidth(), image.getHeight() },
                         opacity);
    }

    static void drawAssetCover(juce::Graphics& g,
                               UiAssetId assetId,
                               juce::Rectangle<float> bounds,
                               float opacity = 1.0f)
    {
        const auto& image = get(assetId, getRecommendedScaleFactor());
        if (!image.isValid() || bounds.isEmpty())
        {
            jassertfalse;
            return;
        }

        const auto imageBounds = juce::Rectangle<int>(0, 0, image.getWidth(), image.getHeight());
        const float scale = juce::jmax(bounds.getWidth() / static_cast<float>(image.getWidth()),
                                       bounds.getHeight() / static_cast<float>(image.getHeight()));
        const float sourceW = juce::jmin(static_cast<float>(image.getWidth()), bounds.getWidth() / scale);
        const float sourceH = juce::jmin(static_cast<float>(image.getHeight()), bounds.getHeight() / scale);
        const float sourceX = (static_cast<float>(image.getWidth()) - sourceW) * 0.5f;
        const float sourceY = (static_cast<float>(image.getHeight()) - sourceH) * 0.5f;

        auto source = juce::Rectangle<int>(juce::roundToInt(sourceX),
                                           juce::roundToInt(sourceY),
                                           juce::roundToInt(sourceW),
                                           juce::roundToInt(sourceH));
        source = source.getIntersection(imageBounds);
        drawImageSection(g, image, bounds, source, opacity);
    }

    static void drawFilmstripFrame(juce::Graphics& g,
                                   UiAssetId assetId,
                                   juce::Rectangle<float> bounds,
                                   float normalisedValue,
                                   int frameCount,
                                   float opacity = 1.0f)
    {
        const auto& filmstrip = get(assetId, getRecommendedScaleFactor());
        if (!filmstrip.isValid() || bounds.isEmpty() || frameCount <= 0)
        {
            jassertfalse;
            return;
        }

        const int frameHeight = filmstrip.getHeight() / frameCount;
        if (frameHeight <= 0)
        {
            jassertfalse;
            return;
        }

        const int frameIndex = juce::jlimit(0,
                                            frameCount - 1,
                                            juce::roundToInt(juce::jlimit(0.0f, 1.0f, normalisedValue)
                                                * static_cast<float>(frameCount - 1)));
        drawImageSection(g,
                         filmstrip,
                         bounds,
                         { 0, frameIndex * frameHeight, filmstrip.getWidth(), frameHeight },
                         opacity);
    }

    static void drawAssetSliceStretch(juce::Graphics& g,
                                      UiAssetId assetId,
                                      juce::Rectangle<float> bounds,
                                      juce::Rectangle<int> source,
                                      float opacity = 1.0f)
    {
        const auto& image = get(assetId, getRecommendedScaleFactor());
        if (!image.isValid() || bounds.isEmpty())
        {
            jassertfalse;
            return;
        }

        const auto safeSource = source.getIntersection(juce::Rectangle<int>(0, 0, image.getWidth(), image.getHeight()));
        if (safeSource.isEmpty())
        {
            jassertfalse;
            return;
        }

        drawImageSection(g, image, bounds, safeSource, opacity);
    }

private:
    static float getRecommendedScaleFactor()
    {
        return juce::jlimit(1.0f, 3.0f, juce::Desktop::getInstance().getGlobalScaleFactor());
    }

    static int chooseScaleBucket(float scaleFactor)
    {
        if (scaleFactor >= 2.5f)
            return 3;

        if (scaleFactor >= 1.5f)
            return 2;

        return 1;
    }

    static juce::String buildNamedResourceId(UiAssetId assetId, int scaleBucket)
    {
        auto resourceStem = getResourceStem(assetId);
        if (scaleBucket > 1)
            resourceStem << scaleBucket << "x";

        resourceStem << "_png";
        return resourceStem;
    }

    static juce::String getResourceStem(UiAssetId assetId)
    {
        switch (assetId)
        {
            case UiAssetId::BackgroundMain:               return "background_main";
            case UiAssetId::PanelEditorMain:              return "panel_editor_main";
            case UiAssetId::PanelParameterSidebar:        return "panel_parameter_sidebar";
            case UiAssetId::PanelTrackColumn:             return "panel_track_column";
            case UiAssetId::PanelTrackCard:               return "panel_track_card";
            case UiAssetId::PanelDisplayPanel:            return "panel_display_panel";
            case UiAssetId::ToolbarTopbarButtonIdle:      return "toolbar_topbar_button_shell_idle";
            case UiAssetId::ToolbarTopbarButtonActive:    return "toolbar_topbar_button_shell_active";
            case UiAssetId::TransportButtonShell:         return "transport_button_shell";
            case UiAssetId::ToolbarTransportTimeField:    return "toolbar_transport_time_field_shell";
            case UiAssetId::ToolbarBpmField:              return "toolbar_bpm_field_shell";
            case UiAssetId::ToolbarDropdownNarrowField:   return "toolbar_dropdown_narrow_field_shell";
            case UiAssetId::ToolbarDropdownWideField:     return "toolbar_dropdown_wide_field_shell";
            case UiAssetId::ToolRightPanelButtonShell:    return "tool_right_panel_button_shell";
            case UiAssetId::ToolRightPanelAutoButton:     return "tool_right_panel_auto_button";
            case UiAssetId::ParameterSmallReadout:        return "parameter_small_readout";
            case UiAssetId::SliderTrack:                  return "slider_track";
            case UiAssetId::SliderThumb:                  return "slider_thumb";
            case UiAssetId::ScrollbarTrack:               return "scrollbar_track";
            case UiAssetId::ScrollbarThumb:               return "scrollbar_thumb";
            case UiAssetId::TabSegmentTrack:              return "tab_segment_track";
            case UiAssetId::TabSegmentActiveShell:        return "tab_segment_active_shell";
            case UiAssetId::PianoKeyAtlas:                return "piano_key_atlas";
            case UiAssetId::KnobPrimaryFilmstrip:         return "knob_primary_filmstrip";
            case UiAssetId::KnobLargeParameterFilmstrip:  return "knob_large_parameter_filmstrip";
        }

        jassertfalse;
        return {};
    }

    static bool hasNamedResource(const juce::String& resourceName)
    {
        int dataSize = 0;
        return BinaryData::getNamedResource(resourceName.toRawUTF8(), dataSize) != nullptr && dataSize > 0;
    }

    static const juce::Image& getCachedImage(const juce::String& resourceName)
    {
        static std::map<juce::String, juce::Image> cache;
        static std::mutex cacheMutex;

        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto found = cache.find(resourceName);
        if (found != cache.end())
            return found->second;

        const auto [it, inserted] = cache.emplace(resourceName, loadNamedPng(resourceName));
        juce::ignoreUnused(inserted);
        return it->second;
    }

    static juce::Image loadNamedPng(const juce::String& resourceName)
    {
        int dataSize = 0;
        const auto* data = BinaryData::getNamedResource(resourceName.toRawUTF8(), dataSize);
        if (data == nullptr || dataSize <= 0)
        {
            jassertfalse;
            return {};
        }

        auto image = juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(dataSize));
        jassert(image.isValid());
        return image;
    }

    static void drawImageSection(juce::Graphics& g,
                                 const juce::Image& image,
                                 juce::Rectangle<float> bounds,
                                 juce::Rectangle<int> source,
                                 float opacity)
    {
        const auto dest = bounds.toNearestInt();
        if (dest.isEmpty() || source.isEmpty())
            return;

        juce::Graphics::ScopedSaveState state(g);
        g.setOpacity(juce::jlimit(0.0f, 1.0f, opacity));
        g.drawImage(image,
                    dest.getX(),
                    dest.getY(),
                    dest.getWidth(),
                    dest.getHeight(),
                    source.getX(),
                    source.getY(),
                    source.getWidth(),
                    source.getHeight(),
                    false);
    }
};

} // namespace OpenTune

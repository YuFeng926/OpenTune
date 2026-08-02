#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

#include "BinaryData.h"

namespace OpenTune {

enum class CursorStyleId
{
    System = 0,
    Adwaita,
    Capitaine,
    Breeze
};

class CursorThemeManager
{
public:
    static CursorThemeManager& getInstance()
    {
        static CursorThemeManager instance;
        return instance;
    }

    void setStyle(CursorStyleId style)
    {
        style_ = style;
        if (style == CursorStyleId::System)
        {
            cursors_ = {};
            return;
        }
        cursors_ = buildCursors(style);
    }

    CursorStyleId getStyle() const { return style_; }

    juce::MouseCursor resolveCursor(const juce::MouseCursor& cursor) const
    {
        if (style_ == CursorStyleId::System)
            return cursor;

        if (cursor == juce::MouseCursor::NormalCursor) return cursors_[0];
        if (cursor == juce::MouseCursor::DraggingHandCursor) return cursors_[1];
        if (cursor == juce::MouseCursor::LeftRightResizeCursor) return cursors_[2];
        if (cursor == juce::MouseCursor::PointingHandCursor) return cursors_[3];
        if (cursor == juce::MouseCursor::UpDownResizeCursor) return cursors_[4];
        if (cursor == juce::MouseCursor::CrosshairCursor) return cursors_[5];
        if (cursor == juce::MouseCursor::UpDownLeftRightResizeCursor) return cursors_[6];

        return cursor;
    }

private:
    CursorThemeManager() = default;

    // 每主题 7 组热点（48px 坐标）：normal, dragging, left_right, pointing, up_down, crosshair, move_all
    static constexpr std::array<std::array<int, 2>, 7> hotspots(CursorStyleId style)
    {
        switch (style)
        {
            case CursorStyleId::Adwaita:
                return { { { 6, 2 }, { 22, 4 }, { 24, 24 }, { 14, 10 }, { 24, 26 }, { 22, 22 }, { 24, 22 } } };
            case CursorStyleId::Capitaine:
                return { { { 8, 4 }, { 24, 24 }, { 24, 24 }, { 24, 12 }, { 24, 24 }, { 24, 24 }, { 24, 24 } } };
            case CursorStyleId::Breeze:
                return { { { 6, 6 }, { 25, 24 }, { 24, 23 }, { 26, 6 }, { 26, 24 }, { 27, 26 }, { 24, 23 } } };
            case CursorStyleId::System:
                return {};
        }
        return {};
    }

    static juce::MouseCursor makeCursor(const void* data, int dataSize, int hotX, int hotY)
    {
        return { juce::ImageCache::getFromMemory(data, dataSize), hotX, hotY };
    }

    static std::array<juce::MouseCursor, 7> buildCursors(CursorStyleId style)
    {
        const auto hs = hotspots(style);
        switch (style)
        {
            case CursorStyleId::Adwaita:
                return { makeCursor(BinaryData::adwaita_normal_png, BinaryData::adwaita_normal_pngSize, hs[0][0], hs[0][1]),
                         makeCursor(BinaryData::adwaita_dragging_png, BinaryData::adwaita_dragging_pngSize, hs[1][0], hs[1][1]),
                         makeCursor(BinaryData::adwaita_left_right_png, BinaryData::adwaita_left_right_pngSize, hs[2][0], hs[2][1]),
                         makeCursor(BinaryData::adwaita_pointing_png, BinaryData::adwaita_pointing_pngSize, hs[3][0], hs[3][1]),
                         makeCursor(BinaryData::adwaita_up_down_png, BinaryData::adwaita_up_down_pngSize, hs[4][0], hs[4][1]),
                         makeCursor(BinaryData::adwaita_crosshair_png, BinaryData::adwaita_crosshair_pngSize, hs[5][0], hs[5][1]),
                         makeCursor(BinaryData::adwaita_move_all_png, BinaryData::adwaita_move_all_pngSize, hs[6][0], hs[6][1]) };
            case CursorStyleId::Capitaine:
                return { makeCursor(BinaryData::capitaine_normal_png, BinaryData::capitaine_normal_pngSize, hs[0][0], hs[0][1]),
                         makeCursor(BinaryData::capitaine_dragging_png, BinaryData::capitaine_dragging_pngSize, hs[1][0], hs[1][1]),
                         makeCursor(BinaryData::capitaine_left_right_png, BinaryData::capitaine_left_right_pngSize, hs[2][0], hs[2][1]),
                         makeCursor(BinaryData::capitaine_pointing_png, BinaryData::capitaine_pointing_pngSize, hs[3][0], hs[3][1]),
                         makeCursor(BinaryData::capitaine_up_down_png, BinaryData::capitaine_up_down_pngSize, hs[4][0], hs[4][1]),
                         makeCursor(BinaryData::capitaine_crosshair_png, BinaryData::capitaine_crosshair_pngSize, hs[5][0], hs[5][1]),
                         makeCursor(BinaryData::capitaine_move_all_png, BinaryData::capitaine_move_all_pngSize, hs[6][0], hs[6][1]) };
            case CursorStyleId::Breeze:
                return { makeCursor(BinaryData::breeze_normal_png, BinaryData::breeze_normal_pngSize, hs[0][0], hs[0][1]),
                         makeCursor(BinaryData::breeze_dragging_png, BinaryData::breeze_dragging_pngSize, hs[1][0], hs[1][1]),
                         makeCursor(BinaryData::breeze_left_right_png, BinaryData::breeze_left_right_pngSize, hs[2][0], hs[2][1]),
                         makeCursor(BinaryData::breeze_pointing_png, BinaryData::breeze_pointing_pngSize, hs[3][0], hs[3][1]),
                         makeCursor(BinaryData::breeze_up_down_png, BinaryData::breeze_up_down_pngSize, hs[4][0], hs[4][1]),
                         makeCursor(BinaryData::breeze_crosshair_png, BinaryData::breeze_crosshair_pngSize, hs[5][0], hs[5][1]),
                         makeCursor(BinaryData::breeze_move_all_png, BinaryData::breeze_move_all_pngSize, hs[6][0], hs[6][1]) };
            case CursorStyleId::System:
                return {};
        }
        return {};
    }

    CursorStyleId style_ = CursorStyleId::System;
    std::array<juce::MouseCursor, 7> cursors_;
};

} // namespace OpenTune

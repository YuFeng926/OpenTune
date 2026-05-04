#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

/**
 * @brief SVG icon paths for toolbar tools
 *
 * All paths are designed for a 24x24 viewBox and will be scaled to fit button size.
 */
class ToolbarIcons {
public:
    /**
     * @brief Get file/document icon path
     */
    static juce::Path getFileIcon() {
        juce::Path path;
        // Document shape with folded corner
        path.startNewSubPath(6.0f, 3.0f);
        path.lineTo(14.0f, 3.0f);
        path.lineTo(18.0f, 7.0f);
        path.lineTo(18.0f, 21.0f);
        path.lineTo(6.0f, 21.0f);
        path.closeSubPath();
        
        // Fold line
        path.startNewSubPath(14.0f, 3.0f);
        path.lineTo(14.0f, 7.0f);
        path.lineTo(18.0f, 7.0f);
        return path;
    }

    /**
     * @brief Get edit/pen icon path
     */
    static juce::Path getEditIcon() {
        juce::Path path;
        // Pen body
        path.startNewSubPath(18.0f, 4.0f);
        path.lineTo(20.0f, 6.0f);
        path.lineTo(8.0f, 18.0f);
        path.lineTo(4.0f, 20.0f); // Tip
        path.lineTo(6.0f, 16.0f);
        path.closeSubPath();
        
        // Cap/Detail line
        path.startNewSubPath(17.0f, 5.0f);
        path.lineTo(19.0f, 7.0f);
        return path;
    }

    /**
     * @brief Get view/eye icon path
     */
    static juce::Path getEyeIcon() {
        juce::Path path;
        // Eye shape
        path.startNewSubPath(2.0f, 12.0f);
        path.quadraticTo(12.0f, 4.0f, 22.0f, 12.0f);
        path.quadraticTo(12.0f, 20.0f, 2.0f, 12.0f);
        path.closeSubPath();
        
        // Pupil
        path.addEllipse(9.0f, 9.0f, 6.0f, 6.0f);
        return path;
    }

    /**
     * @brief Get cursor/select icon path
     * Arrow pointer for selection tool
     */
    static juce::Path getSelectIcon() {
        juce::Path path;
        // Standard mouse cursor arrow
        path.startNewSubPath(6.0f, 3.0f);
        path.lineTo(6.0f, 19.0f);
        path.lineTo(10.0f, 15.0f);
        path.lineTo(14.0f, 23.0f);
        path.lineTo(17.0f, 21.0f);
        path.lineTo(13.0f, 13.0f);
        path.lineTo(19.0f, 13.0f);
        path.closeSubPath();
        return path;
    }

    /**
     * @brief Get pencil/draw icon path
     * Pencil for drawing notes
     */
    static juce::Path getDrawIcon() {
        juce::Path path;
        // Pencil shape
        path.startNewSubPath(18.0f, 3.0f);
        path.lineTo(21.0f, 6.0f);
        path.lineTo(8.0f, 19.0f);
        path.lineTo(3.0f, 21.0f);
        path.lineTo(5.0f, 16.0f);
        path.closeSubPath();
        // Pencil tip
        path.addLineSegment(juce::Line<float>(18.0f, 3.0f, 15.0f, 6.0f), 1.0f);
        return path;
    }

    /**
     * @brief Get draw note/brush icon path
     * Brush with note blocks for drawing notes
     */
    static juce::Path getDrawNoteIcon() {
        juce::Path path;
        // Brush handle (tilted)
        path.startNewSubPath(16.0f, 2.0f);
        path.lineTo(18.0f, 4.0f);
        path.lineTo(10.0f, 12.0f);
        path.lineTo(8.0f, 10.0f);
        path.closeSubPath();

        // Note blocks painted by brush
        path.addRoundedRectangle(3.0f, 14.0f, 4.0f, 3.0f, 1.0f);
        path.addRoundedRectangle(9.0f, 16.0f, 5.0f, 3.0f, 1.0f);
        path.addRoundedRectangle(16.0f, 13.0f, 4.0f, 3.0f, 1.0f);

        return path;
    }

    /**
     * @brief Get draw line icon path
     * Two points connected by a straight line
     */
    static juce::Path getDrawLineIcon() {
        juce::Path path;
        // Start point (circle)
        path.addEllipse(3.0f, 16.0f, 4.0f, 4.0f);

        // End point (circle)
        path.addEllipse(17.0f, 4.0f, 4.0f, 4.0f);

        // Connecting line
        path.startNewSubPath(5.0f, 18.0f);
        path.lineTo(19.0f, 6.0f);

        return path;
    }

    static juce::Path getLineAnchorIcon() {
        juce::Path path;
        path.addEllipse(3.0f, 16.0f, 4.0f, 4.0f);
        path.addEllipse(17.0f, 4.0f, 4.0f, 4.0f);
        path.startNewSubPath(5.0f, 18.0f);
        path.lineTo(19.0f, 6.0f);
        return path;
    }

    /**
     * @brief Get track view icon (stacked tracks)
     */
    static juce::Path getTrackViewIcon() {
        juce::Path path;
        // 3 stacked horizontal bars representing tracks
        path.addRoundedRectangle(3.0f, 5.0f, 18.0f, 3.5f, 1.0f);
        path.addRoundedRectangle(3.0f, 10.5f, 18.0f, 3.5f, 1.0f);
        path.addRoundedRectangle(3.0f, 16.0f, 18.0f, 3.5f, 1.0f);
        return path;
    }

    /**
     * @brief Get piano roll view icon (keys + notes)
     */
    static juce::Path getPianoViewIcon() {
        juce::Path path;
        // Piano keys (left strip)
        path.addRectangle(3.0f, 3.0f, 5.0f, 18.0f);
        
        // Black keys overlay
        path.startNewSubPath(3.0f, 6.0f); path.lineTo(6.0f, 6.0f);
        path.startNewSubPath(3.0f, 10.0f); path.lineTo(6.0f, 10.0f);
        path.startNewSubPath(3.0f, 14.0f); path.lineTo(6.0f, 14.0f);

        // Notes (right side)
        path.addRoundedRectangle(10.0f, 5.0f, 6.0f, 3.0f, 1.0f);
        path.addRoundedRectangle(15.0f, 10.0f, 6.0f, 3.0f, 1.0f);
        path.addRoundedRectangle(12.0f, 15.0f, 6.0f, 3.0f, 1.0f);
        
        return path;
    }

    /**
     * @brief Get scissors/cut icon path
     * Scissors for cutting/splitting notes
     */
    static juce::Path getCutIcon() {
        juce::Path path;
        // Left blade
        path.addEllipse(3.0f, 3.0f, 4.0f, 4.0f);
        path.startNewSubPath(5.0f, 7.0f);
        path.lineTo(12.0f, 12.0f);

        // Right blade
        path.addEllipse(3.0f, 17.0f, 4.0f, 4.0f);
        path.startNewSubPath(5.0f, 17.0f);
        path.lineTo(12.0f, 12.0f);

        // Top line
        path.startNewSubPath(12.0f, 12.0f);
        path.lineTo(21.0f, 6.0f);

        return path;
    }

    /**
     * @brief Get eraser icon path
     * Eraser for removing notes or corrections
     */
    static juce::Path getEraseIcon() {
        juce::Path path;
        // Eraser rectangle (tilted)
        path.startNewSubPath(4.0f, 12.0f);
        path.lineTo(12.0f, 4.0f);
        path.lineTo(20.0f, 12.0f);
        path.lineTo(12.0f, 20.0f);
        path.closeSubPath();
        // Eraser bottom line
        path.addLineSegment(juce::Line<float>(4.0f, 20.0f, 20.0f, 20.0f), 2.0f);
        return path;
    }

    /**
     * @brief Get curve/wave icon path
     * Sine wave for pitch curve editing
     */
    static juce::Path getCurveIcon() {
        juce::Path path;
        // Smooth sine wave
        path.startNewSubPath(2.0f, 12.0f);
        path.cubicTo(6.0f, 6.0f, 10.0f, 6.0f, 12.0f, 12.0f);
        path.cubicTo(14.0f, 18.0f, 18.0f, 18.0f, 22.0f, 12.0f);
        return path;
    }

    static juce::Path getHandDrawIcon() {
        juce::Path path;
        path.startNewSubPath(2.0f, 15.0f);
        path.cubicTo(6.0f, 7.0f, 10.0f, 19.0f, 14.0f, 11.0f);
        path.cubicTo(16.0f, 7.0f, 19.0f, 6.0f, 22.0f, 9.0f);

        juce::Path nib;
        nib.addTriangle(14.5f, 18.5f, 18.5f, 14.5f, 20.0f, 20.0f);
        path.addPath(nib);
        return path;
    }

    /**
     * @brief Get waveform/track icon path
     * Bars representing audio waveform
     */
    static juce::Path getWaveformIcon() {
        juce::Path path;
        // 4 bars of varying height centered vertically (24x24 box)
        float centerY = 12.0f;
        path.addRoundedRectangle(3.0f, centerY - 3.0f, 3.0f, 6.0f, 1.0f);
        path.addRoundedRectangle(8.0f, centerY - 6.0f, 3.0f, 12.0f, 1.0f);
        path.addRoundedRectangle(13.0f, centerY - 8.0f, 3.0f, 16.0f, 1.0f);
        path.addRoundedRectangle(18.0f, centerY - 4.0f, 3.0f, 8.0f, 1.0f);
        return path;
    }

    /**
     * @brief Get auto-tune/music note icon path
     * Musical note for auto-tune function
     */
    static juce::Path getAutoTuneIcon() {
        juce::Path path;
        // Note head (filled circle)
        path.addEllipse(4.0f, 14.0f, 6.0f, 6.0f);
        // Note stem
        path.addRectangle(9.0f, 4.0f, 2.0f, 12.0f);
        // Note flag
        path.startNewSubPath(11.0f, 4.0f);
        path.cubicTo(15.0f, 4.0f, 18.0f, 6.0f, 18.0f, 9.0f);
        path.cubicTo(18.0f, 12.0f, 15.0f, 14.0f, 11.0f, 14.0f);
        return path;
    }

    // Blue Breeze 主题图标

    /**
     * @brief Get tracks icon path (List/Tracks view)
     */
    static juce::Path getTracksIcon() {
        juce::Path path;
        // Three horizontal lines with varying lengths, simulating tracks
        path.startNewSubPath(4.0f, 6.0f);  path.lineTo(20.0f, 6.0f);
        path.startNewSubPath(4.0f, 12.0f); path.lineTo(16.0f, 12.0f);
        path.startNewSubPath(4.0f, 18.0f); path.lineTo(18.0f, 18.0f);
        
        // Small dots at the start
        path.addEllipse(2.0f, 5.0f, 2.0f, 2.0f);
        path.addEllipse(2.0f, 11.0f, 2.0f, 2.0f);
        path.addEllipse(2.0f, 17.0f, 2.0f, 2.0f);
        return path;
    }

    /**
     * @brief Get properties/settings icon path (Sliders)
     */
    static juce::Path getPropsIcon() {
        juce::Path path;
        // Three vertical sliders with knobs
        // Slider tracks
        path.startNewSubPath(6.0f, 4.0f); path.lineTo(6.0f, 20.0f);
        path.startNewSubPath(12.0f, 4.0f); path.lineTo(12.0f, 20.0f);
        path.startNewSubPath(18.0f, 4.0f); path.lineTo(18.0f, 20.0f);

        // Knobs (circles)
        path.addEllipse(4.0f, 14.0f, 4.0f, 4.0f); // Left knob low
        path.addEllipse(10.0f, 8.0f, 4.0f, 4.0f); // Middle knob high
        path.addEllipse(16.0f, 12.0f, 4.0f, 4.0f); // Right knob mid
        return path;
    }

    /**
     * @brief Get left panel toggle icon (arrow pointing left to collapse left panel)
     * Shows a vertical bar with left-pointing arrow
     */
    static juce::Path getPanelLeftIcon() {
        juce::Path path;
        // Vertical bar on the right side (panel edge)
        path.addRectangle(18.0f, 4.0f, 2.0f, 16.0f);
        // Left-pointing arrow (chevron)
        path.startNewSubPath(14.0f, 12.0f);
        path.lineTo(6.0f, 12.0f);
        path.lineTo(10.0f, 8.0f);
        path.startNewSubPath(6.0f, 12.0f);
        path.lineTo(10.0f, 16.0f);
        return path;
    }

    /**
     * @brief Get right panel toggle icon (arrow pointing right to collapse right panel)
     * Shows a vertical bar with right-pointing arrow
     */
    static juce::Path getPanelRightIcon() {
        juce::Path path;
        // Vertical bar on the left side (panel edge)
        path.addRectangle(4.0f, 4.0f, 2.0f, 16.0f);
        // Right-pointing arrow (chevron)
        path.startNewSubPath(10.0f, 12.0f);
        path.lineTo(18.0f, 12.0f);
        path.lineTo(14.0f, 8.0f);
        path.startNewSubPath(18.0f, 12.0f);
        path.lineTo(14.0f, 16.0f);
        return path;
    }

    static juce::Path getPlayIcon() {
        juce::Path path;
        path.addTriangle(7.0f, 4.0f, 7.0f, 20.0f, 19.0f, 12.0f);
        return path;
    }

    static juce::Path getPauseIcon() {
        juce::Path path;
        path.addRoundedRectangle(6.0f, 4.0f, 4.0f, 16.0f, 1.0f);
        path.addRoundedRectangle(14.0f, 4.0f, 4.0f, 16.0f, 1.0f);
        return path;
    }

    static juce::Path getStopIcon() {
        juce::Path path;
        path.addRoundedRectangle(6.0f, 6.0f, 12.0f, 12.0f, 2.0f);
        return path;
    }

    static juce::Path getLoopIcon() {
        juce::Path path;
        // Circular arrows
        path.addArc(4.0f, 4.0f, 16.0f, 16.0f, 0.5f, 2.8f, true); // Top arc
        path.addArc(4.0f, 4.0f, 16.0f, 16.0f, 3.6f, 5.9f, true); // Bottom arc
        
        // Arrow heads
        path.startNewSubPath(16.0f, 4.0f); path.lineTo(20.0f, 7.0f); path.lineTo(16.0f, 10.0f); // Top arrow
        path.startNewSubPath(8.0f, 14.0f); path.lineTo(4.0f, 17.0f); path.lineTo(8.0f, 20.0f); // Bottom arrow
        return path;
    }

    static juce::Path getViewIcon() {
        juce::Path path;
        // Grid layout icon
        path.addRoundedRectangle(4.0f, 4.0f, 7.0f, 7.0f, 1.0f);
        path.addRoundedRectangle(13.0f, 4.0f, 7.0f, 7.0f, 1.0f);
        path.addRoundedRectangle(4.0f, 13.0f, 7.0f, 7.0f, 1.0f);
        path.addRoundedRectangle(13.0f, 13.0f, 7.0f, 7.0f, 1.0f);
        return path;
    }

    static juce::Path getBpmIcon() {
        juce::Path path;
        // Metronome shape
        path.startNewSubPath(12.0f, 2.0f); // Top
        path.lineTo(20.0f, 22.0f); // Bottom Right
        path.lineTo(4.0f, 22.0f); // Bottom Left
        path.closeSubPath();
        
        // Pendulum
        path.startNewSubPath(12.0f, 18.0f);
        path.lineTo(16.0f, 8.0f);
        path.addEllipse(15.0f, 6.0f, 2.0f, 2.0f); // Weight
        return path;
    }

    static juce::Path getTapIcon() {
        juce::Path path;
        // Finger tap concentric circles
        path.addEllipse(8.0f, 8.0f, 8.0f, 8.0f); // Inner
        path.addArc(2.0f, 2.0f, 20.0f, 20.0f, 0.5f, 5.8f, true); // Outer ring
        return path;
    }

    static juce::Path getRecordIcon() {
        juce::Path path;
        path.addEllipse(6.0f, 6.0f, 12.0f, 12.0f);
        return path;
    }

    static juce::Path getKeyIcon() {
        juce::Path path;
        // Music sharp/flat symbol style abstract
        // Circle with stem (Key)
        path.addEllipse(6.0f, 12.0f, 6.0f, 6.0f);
        path.startNewSubPath(12.0f, 12.0f); path.lineTo(12.0f, 4.0f);
        // Sharp symbol
        path.startNewSubPath(14.0f, 8.0f); path.lineTo(22.0f, 6.0f);
        path.startNewSubPath(14.0f, 12.0f); path.lineTo(22.0f, 10.0f);
        path.startNewSubPath(16.0f, 5.0f); path.lineTo(16.0f, 15.0f);
        path.startNewSubPath(20.0f, 4.0f); path.lineTo(20.0f, 14.0f);
        return path;
    }

    static juce::Path getScaleIcon() {
        juce::Path path;
        // Steps going up
        path.startNewSubPath(4.0f, 20.0f);
        path.lineTo(8.0f, 20.0f); path.lineTo(8.0f, 16.0f);
        path.lineTo(12.0f, 16.0f); path.lineTo(12.0f, 12.0f);
        path.lineTo(16.0f, 12.0f); path.lineTo(16.0f, 8.0f);
        path.lineTo(20.0f, 8.0f); path.lineTo(20.0f, 4.0f);
        
        // Notes on steps
        path.addEllipse(5.0f, 17.0f, 2.0f, 2.0f);
        path.addEllipse(9.0f, 13.0f, 2.0f, 2.0f);
        path.addEllipse(13.0f, 9.0f, 2.0f, 2.0f);
        path.addEllipse(17.0f, 5.0f, 2.0f, 2.0f);
        return path;
    }



    /**
     * @brief Create an icon image for PopupMenu (16x16 pixels)
     * @param path Icon path to render
     * @param color Icon color
     * @return juce::Image The rendered icon image
     */
    static juce::Image createIconImage(const juce::Path& path, juce::Colour color) {
        juce::Image image(juce::Image::ARGB, 16, 16, true);
        juce::Graphics g(image);
        drawIcon(g, path, juce::Rectangle<float>(0, 0, 16, 16), color, 1.5f, false);
        return image;
    }

    /**
     * @brief Create select tool icon image for context menu
     */
    static juce::Image createSelectIconImage() {
        return createIconImage(getSelectIcon(), juce::Colours::white);
    }

    /**
     * @brief Create draw note tool icon image for context menu
     */
    static juce::Image createDrawNoteIconImage() {
        return createIconImage(getDrawNoteIcon(), juce::Colours::white);
    }

    /**
     * @brief Create line anchor tool icon image for context menu
     */
    static juce::Image createLineAnchorIconImage() {
        return createIconImage(getLineAnchorIcon(), juce::Colours::white);
    }

    /**
     * @brief Create hand draw tool icon image for context menu
     */
    static juce::Image createHandDrawIconImage() {
        return createIconImage(getHandDrawIcon(), juce::Colours::white);
    }

    /**
     * @brief Draw an icon path scaled to fit within bounds
     * @param g Graphics context
     * @param path Icon path to draw
     * @param bounds Target bounds to fit icon
     * @param color Icon color
     * @param strokeThickness Line thickness for stroked paths
     * @param filled Whether to fill the path (true) or stroke it (false)
     */
    static void drawIcon(juce::Graphics& g, const juce::Path& path, juce::Rectangle<float> bounds,
                        juce::Colour color, float strokeThickness = 2.0f, bool filled = false) {
        // Calculate scaling to fit 24x24 viewBox into bounds
        auto pathBounds = path.getBounds();
        float scale = juce::jmin(bounds.getWidth() / 24.0f, bounds.getHeight() / 24.0f) * 0.8f;

        // Center the icon
        juce::AffineTransform transform = juce::AffineTransform::scale(scale)
            .translated(bounds.getCentreX() - (pathBounds.getCentreX() * scale),
                       bounds.getCentreY() - (pathBounds.getCentreY() * scale));

        juce::Path transformedPath = path;
        transformedPath.applyTransform(transform);

        g.setColour(color);
        if (filled) {
            g.fillPath(transformedPath);
        } else {
            g.strokePath(transformedPath, juce::PathStrokeType(strokeThickness));
        }
    }

private:
    ToolbarIcons() = delete;
};

} // namespace OpenTune

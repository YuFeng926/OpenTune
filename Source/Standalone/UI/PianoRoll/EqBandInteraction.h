/**
 * EQ Band Interaction — anchor drag interaction logic
 * 
 * Handles mouse interaction for EQ band anchors:
 * - Drag to adjust gain (Y axis)
 * - Shift+drag to adjust frequency (X axis)
 * - Double-click to reset band to default
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Utils/NoteEqSettings.h"
#include "EqGraphRenderer.h"

namespace OpenTune {

class EqBandInteraction {
public:
    EqBandInteraction() = default;
    
    // Set the renderer for coordinate mapping
    void setRenderer(const EqGraphRenderer* renderer) {
        renderer_ = renderer;
    }
    
    // Start dragging a band
    void startDrag(int bandIndex, juce::Point<float> startPos, const EqSettings& currentSettings) {
        if (bandIndex < 0 || bandIndex >= EqSettings::kNumBands)
            return;
        
        dragBandIndex_ = bandIndex;
        dragStartPos_ = startPos;
        dragStartSettings_ = currentSettings;
        isDragging_ = true;
    }
    
    // Update drag position and return updated settings
    EqSettings updateDrag(juce::Point<float> currentPos, bool shiftHeld) const {
        if (!isDragging_ || !renderer_)
            return dragStartSettings_;
        
        EqSettings newSettings = dragStartSettings_;
        auto& band = newSettings.bands[dragBandIndex_];
        
        if (shiftHeld) {
            // Shift+drag: adjust frequency (X axis)
            const double newFreq = renderer_->xToFreq(currentPos.x);
            
            // Clamp to band-specific frequency range
            if (dragBandIndex_ == EqSettings::kLowCut) {
                band.frequency = std::clamp(static_cast<float>(newFreq),
                    EqSettings::kLowCutMinFreq, EqSettings::kLowCutMaxFreq);
            } else if (dragBandIndex_ == EqSettings::kHighCut) {
                band.frequency = std::clamp(static_cast<float>(newFreq),
                    EqSettings::kHighCutMinFreq, EqSettings::kHighCutMaxFreq);
            } else if (dragBandIndex_ == EqSettings::kPeak) {
                band.frequency = std::clamp(static_cast<float>(newFreq),
                    EqSettings::kPeakMinFreq, EqSettings::kPeakMaxFreq);
            }
            // Shelves have fixed frequency, no adjustment
        } else {
            // Normal drag: adjust gain (Y axis)
            const double newGain = renderer_->yToGain(currentPos.y);
            band.setGainDb(static_cast<float>(newGain));
        }
        
        return newSettings;
    }
    
    // End dragging
    void endDrag() {
        isDragging_ = false;
        dragBandIndex_ = -1;
    }
    
    // Reset a band to default settings
    static EqSettings resetBand(const EqSettings& current, int bandIndex) {
        EqSettings newSettings = current;
        auto& band = newSettings.bands[bandIndex];
        
        band.gainDb = 0.0f;
        
        // Reset frequency to default based on band type
        switch (bandIndex) {
            case EqSettings::kLowCut:
                band.frequency = 100.0f;
                break;
            case EqSettings::kLowShelf:
                band.frequency = EqSettings::kLowShelfFreq;
                break;
            case EqSettings::kPeak:
                band.frequency = EqSettings::kDefaultPeakFreq;
                break;
            case EqSettings::kHighShelf:
                band.frequency = EqSettings::kHighShelfFreq;
                break;
            case EqSettings::kHighCut:
                band.frequency = 8000.0f;
                break;
        }
        
        return newSettings;
    }
    
    bool isDragging() const { return isDragging_; }
    int getDragBandIndex() const { return dragBandIndex_; }

private:
    const EqGraphRenderer* renderer_ = nullptr;
    int dragBandIndex_ = -1;
    juce::Point<float> dragStartPos_;
    EqSettings dragStartSettings_;
    bool isDragging_ = false;
};

} // namespace OpenTune
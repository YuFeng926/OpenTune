#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "UI/ToolIds.h"
#include "InteractionState.h"
#include <vector>
#include <functional>
#include <cstdint>

namespace OpenTune {

class PianoRollToolHandler
{
public:
    struct ManualCorrectionOp
    {
        int startFrame = 0;
        int endFrameExclusive = 0;
        std::vector<float> f0Data;
        CorrectedSegment::Source source = CorrectedSegment::Source::HandDraw;
        float retuneSpeed = -1.0f;
    };

    struct Context
    {
        std::function<InteractionState&()> getState;

        std::function<double(int)> xToTime;
        std::function<int(double)> timeToX;
        std::function<float(float)> yToFreq;
        std::function<float(float)> freqToY;

        std::function<std::vector<Note>&()> getNotes;
        std::function<std::vector<Note*>()> getSelectedNotes;
        std::function<Note*(double, float, float)> findNoteAt;
        std::function<void()> deselectAllNotes;
        std::function<void()> selectAllNotes;
        std::function<void(const Note&)> insertNoteSorted;

        std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;
        std::function<int()> getCurveSize;
        std::function<int()> getCurveHopSize;
        std::function<double()> getCurveSampleRate;
        std::function<void(int, int)> clearCorrectionRange;
        std::function<void()> clearAllCorrections;
        std::function<void(const CorrectedSegment&)> restoreCorrectedSegment;

        std::function<int()> getPianoKeyWidth;
        std::function<double()> getTrackOffsetSeconds;
        std::function<double()> getAudioSampleRate;
        std::function<const juce::AudioBuffer<float>*()> getAudioBuffer;

        std::function<float()> getMinMidi;
        std::function<float()> getMaxMidi;
        std::function<float()> getRetuneSpeed;
        std::function<float()> getVibratoDepth;
        std::function<float()> getVibratoRate;
        std::function<float(Note&)> recalculatePIP;

        std::function<double()> getDirtyStartTime;
        std::function<void(double)> setDirtyStartTime;
        std::function<double()> getDirtyEndTime;
        std::function<void(double)> setDirtyEndTime;

        std::function<double()> getDrawingNoteStartTime;
        std::function<void(double)> setDrawingNoteStartTime;
        std::function<double()> getDrawingNoteEndTime;
        std::function<void(double)> setDrawingNoteEndTime;
        std::function<float()> getDrawingNotePitch;
        std::function<void(float)> setDrawingNotePitch;
        std::function<int()> getDrawingNoteIndex;
        std::function<void(int)> setDrawingNoteIndex;

        std::function<bool()> getDrawNoteToolPendingDrag;
        std::function<void(bool)> setDrawNoteToolPendingDrag;
        std::function<juce::Point<int>()> getDrawNoteToolMouseDownPos;
        std::function<void(juce::Point<int>)> setDrawNoteToolMouseDownPos;
        std::function<int()> getDragThreshold;

        std::function<double()> getNoteDragManualStartTime;
        std::function<void(double)> setNoteDragManualStartTime;
        std::function<double()> getNoteDragManualEndTime;
        std::function<void(double)> setNoteDragManualEndTime;
        std::function<std::vector<std::pair<double, float>>&()> getNoteDragInitialManualTargets;

        std::function<void()> requestRepaint;
        std::function<void(const juce::MouseCursor&)> setMouseCursor;
        std::function<void()> grabKeyboardFocus;
        std::function<void(ToolId)> setCurrentTool;
        std::function<void()> showToolSelectionMenu;

        std::function<void(double)> notifyPlayheadChange;
        std::function<void(int, int)> notifyPitchCurveEdited;
        std::function<void()> notifyAutoTuneRequested;
        std::function<void()> notifyPlayPauseToggle;
        std::function<void()> notifyStopPlayback;
        std::function<void()> notifyEscapeKey;
        std::function<void(size_t, float, float)> notifyNoteOffsetChanged;

        std::function<void(const juce::String&)> beginEditTransaction;
        std::function<void()> commitEditTransaction;
        std::function<bool()> isTransactionActive;

        std::function<void(std::vector<ManualCorrectionOp>, int, int, bool)> applyManualCorrection;
        std::function<void(int, int, float, float, float)> enqueueNoteBasedCorrection;
        std::function<std::vector<float>()> getOriginalF0;
    };

    explicit PianoRollToolHandler(Context context);

    void setTool(ToolId tool) { currentTool_ = tool; }
    ToolId getTool() const { return currentTool_; }

    void mouseMove(const juce::MouseEvent& e);
    void mouseDown(const juce::MouseEvent& e);
    void mouseDrag(const juce::MouseEvent& e);
    void mouseUp(const juce::MouseEvent& e);
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel);

    bool keyPressed(const juce::KeyPress& key);

    void cancelDrag();

private:
    void handleSelectTool(const juce::MouseEvent& e);
    void handleDrawCurveTool(const juce::MouseEvent& e);
    void handleDrawNoteTool(const juce::MouseEvent& e);
    void handleDrawNoteMouseDown(const juce::MouseEvent& e);
    void handleAutoTuneTool(const juce::MouseEvent& e);
    void handleLineAnchorMouseDown(const juce::MouseEvent& e);
    void handleLineAnchorMouseDrag(const juce::MouseEvent& e);
    void handleLineAnchorMouseUp(const juce::MouseEvent& e);
    void commitLineAnchorOperation();

    void handleSelectDrag(const juce::MouseEvent& e);
    void handleDrawCurveDrag(const juce::MouseEvent& e);
    void handleDrawNoteDrag(const juce::MouseEvent& e);

    void handleSelectUp(const juce::MouseEvent& e);
    void handleDrawCurveUp(const juce::MouseEvent& e);
    void handleDrawNoteUp(const juce::MouseEvent& e);

    void showToolContextMenu(const juce::MouseEvent& e);

    void deleteSelectedNotes();
    void handleDeleteKey();
    
    Note* findLastSelectedNote();
    void selectNotesBetween(Note* start, Note* end);
    void updateF0SelectionFromNotes();

    /** Find note indices whose time range overlaps with the given frame range.
     *  Used by HandDraw and LineAnchor for post-operation note highlighting. */
    std::vector<int> findOverlappingNoteIndices(
        int drawStartFrame, int drawEndFrameExclusive) const;

    Context ctx_;
    ToolId currentTool_ = ToolId::Select;

    juce::Point<int> dragStartPos_;
    juce::Point<float> lastDrawPoint_;
    bool isDraggingTimeline_ = false;
};

} // namespace OpenTune

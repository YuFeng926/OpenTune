#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <unordered_map>

#include <juce_gui_basics/juce_gui_basics.h>

#include "TimelineRenderingDiagnostics.h"

namespace OpenTune {

/**
 * TimelineFrameCoordinator semantics for timeline UI repaint work.
 *
 * In addition to generic requestInvalidate(), provides timeline-specific
 * request types so repaint work is coalesced by component, priority, and
 * reason before the JUCE repaint request is dispatched.
 */
class FrameScheduler final : private juce::AsyncUpdater
{
public:
    enum class Priority : int
    {
        Background = 0,
        Normal = 1,
        Interactive = 2
    };

    /** Repaint reason for timeline content. Values are used as bit positions. */
    enum class TimelineReason {
        PlayheadOverlay = 0,
        ContentModelInvalid = 2,
        LowPriorityAnimation = 3,
        FullContent = 4
    };

    static FrameScheduler& instance()
    {
        static FrameScheduler scheduler;
        return scheduler;
    }

    struct DiagnosticsSnapshot
    {
        uint64_t playheadOverlayRequests = 0;
        uint64_t contentRepaintRequests = 0;
        uint64_t contentInvalidationRequests = 0;
        uint64_t renderModelRebuilds = 0;
        uint64_t fullRepaintPromotions = 0;
        uint64_t waveformTileHits = 0;
        uint64_t waveformTileMisses = 0;
        uint64_t lowPriorityAnimationRequests = 0;
        uint64_t lowPriorityAnimationDropped = 0;
        uint64_t coalescedRequests = 0;
        uint64_t dispatchedFrames = 0;
        uint64_t dirtyRectPixels = 0;
        std::size_t pendingComponentCount = 0;
    };

    void setTimelinePlaybackActive(bool isActive) noexcept
    {
        timelinePlaybackActive_.store(isActive, std::memory_order_release);
    }

    void setPlaybackActiveForTimeline(bool isActive) noexcept
    {
        setTimelinePlaybackActive(isActive);
    }

    bool isTimelinePlaybackActive() const noexcept
    {
        return timelinePlaybackActive_.load(std::memory_order_acquire);
    }

    void resetDiagnosticsForTests()
    {
        auto& diagnostics = TimelineRenderingDiagnostics::instance();
        diagnostics.setEnabled(true);
        diagnostics.reset();
        pending_.clear();
    }

    DiagnosticsSnapshot diagnosticsSnapshot() const
    {
        const auto raw = TimelineRenderingDiagnostics::instance().snapshot();
        DiagnosticsSnapshot snapshot;
        snapshot.playheadOverlayRequests = raw.overlayRepaintRequests;
        snapshot.contentRepaintRequests = raw.contentRepaintRequests;
        snapshot.contentInvalidationRequests = raw.contentRepaintRequests;
        snapshot.renderModelRebuilds = raw.renderModelRebuilds;
        snapshot.fullRepaintPromotions = raw.fullRepaintPromotions;
        snapshot.waveformTileHits = raw.tileCacheHits;
        snapshot.waveformTileMisses = raw.tileCacheMisses;
        snapshot.lowPriorityAnimationRequests = raw.lowPriorityAnimationRequests;
        snapshot.lowPriorityAnimationDropped = raw.lowPriorityAnimationDrops;
        snapshot.coalescedRequests = raw.coalescedRequests;
        snapshot.dispatchedFrames = raw.dispatchedFrames;
        snapshot.dirtyRectPixels = raw.dirtyRectPixels;
        snapshot.pendingComponentCount = pending_.size();
        return snapshot;
    }

    void recordRenderModelRebuild(TimelineReason)
    {
        TimelineRenderingDiagnostics::instance().recordRenderModelRebuild();
    }

    void recordWaveformTileHit()
    {
        TimelineRenderingDiagnostics::instance().recordTileCacheHit();
    }

    void recordWaveformTileMiss()
    {
        TimelineRenderingDiagnostics::instance().recordTileCacheMiss();
    }

    // ---- Generic invalidate (existing API) ----

    void requestInvalidate(juce::Component& component,
                           const juce::Rectangle<int>& dirtyArea,
                           Priority priority = Priority::Normal)
    {
        requestTimelineInvalidate(component,
                                  TimelineReason::ContentModelInvalid,
                                  dirtyArea,
                                  priority,
                                  false);
    }

    void requestInvalidate(juce::Component& component,
                           Priority priority = Priority::Normal)
    {
        requestTimelineInvalidate(component,
                                  TimelineReason::FullContent,
                                  {},
                                  priority,
                                  true);
    }

    // ---- Timeline-specific requests ----

    /**
     * Request playhead overlay repaint only.
     * Narrow rect: just the playhead line area (old ∪ new).
     */
    void requestPlayheadOverlay(juce::Component& component,
                                const juce::Rectangle<int>& overlayDirty)
    {
        requestTimelineInvalidate(component,
                                  TimelineReason::PlayheadOverlay,
                                  overlayDirty,
                                  Priority::Interactive,
                                  false);
    }

    /**
     * Request content model rebuild + repaint of the relevant area.
     */
    void requestContentInvalidation(juce::Component& component,
                                    const juce::Rectangle<int>& dirtyArea = {},
                                    Priority priority = Priority::Normal)
    {
        requestTimelineInvalidate(component,
                                  dirtyArea.isEmpty() ? TimelineReason::FullContent
                                                      : TimelineReason::ContentModelInvalid,
                                  dirtyArea,
                                  priority,
                                  dirtyArea.isEmpty());
    }

    /**
     * Request cosmetic animation work that may be dropped while playback is
     * active. Callers must provide a bounded dirty area.
     */
    void requestLowPriorityAnimation(juce::Component& component,
                                     const juce::Rectangle<int>& dirtyArea)
    {
        auto& diagnostics = TimelineRenderingDiagnostics::instance();
        diagnostics.recordLowPriorityAnimationRequest();

        if (isTimelinePlaybackActive()) {
            diagnostics.recordLowPriorityAnimationDrop();
            return;
        }

        requestTimelineInvalidate(component,
                                  TimelineReason::LowPriorityAnimation,
                                  dirtyArea,
                                  Priority::Background,
                                  false);
    }

private:
    struct PendingInvalidate
    {
        juce::Component::SafePointer<juce::Component> component;
        juce::Rectangle<int> dirty;
        bool hasDirty = false;
        bool fullRepaint = false;
        int priority = static_cast<int>(Priority::Background);
        uint32_t reasonMask = 0;
        std::array<juce::Rectangle<int>, 5> dirtyByReason;
    };

    std::unordered_map<juce::Component*, PendingInvalidate> pending_;
    std::atomic<bool> timelinePlaybackActive_ { false };

    FrameScheduler() = default;

    static uint32_t reasonBit(TimelineReason reason) noexcept
    {
        return static_cast<uint32_t>(1u) << static_cast<uint32_t>(reason);
    }

    static bool hasOnlyReason(uint32_t mask, TimelineReason reason) noexcept
    {
        return mask == reasonBit(reason);
    }

    static std::size_t reasonIndex(TimelineReason reason) noexcept
    {
        return static_cast<std::size_t>(reason);
    }

    void requestTimelineInvalidate(juce::Component& component,
                                   TimelineReason reason,
                                   const juce::Rectangle<int>& dirtyArea,
                                   Priority priority,
                                   bool fullRepaint)
    {
        if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            juce::MessageManager::callAsync([this,
                                             safe = juce::Component::SafePointer<juce::Component>(&component),
                                             reason,
                                             dirtyArea,
                                             priority,
                                             fullRepaint]() {
                if (safe != nullptr)
                    requestTimelineInvalidate(*safe, reason, dirtyArea, priority, fullRepaint);
            });
            return;
        }

        if (!fullRepaint && dirtyArea.isEmpty())
            return;

        enqueueRequest(component, reason, dirtyArea, priority, fullRepaint);
    }

    void enqueueRequest(juce::Component& component,
                        TimelineReason reason,
                        const juce::Rectangle<int>& dirtyArea,
                        Priority priority,
                        bool fullRepaint)
    {
        auto& diagnostics = TimelineRenderingDiagnostics::instance();
        recordRequestDiagnostics(reason, dirtyArea, fullRepaint);

        auto it = pending_.find(&component);
        if (it == pending_.end()) {
            auto& entry = pending_[&component];
            entry.component = juce::Component::SafePointer<juce::Component>(&component);
            entry.fullRepaint = fullRepaint;
            entry.hasDirty = !fullRepaint;
            entry.dirty = fullRepaint ? juce::Rectangle<int>() : dirtyArea;
            entry.priority = static_cast<int>(priority);
            entry.reasonMask = reasonBit(reason);
            if (!fullRepaint)
                entry.dirtyByReason[reasonIndex(reason)] = dirtyArea;
        } else {
            auto& entry = it->second;
            diagnostics.recordCoalescedRequest();

            entry.priority = std::max(entry.priority, static_cast<int>(priority));
            entry.reasonMask |= reasonBit(reason);

            if (fullRepaint) {
                entry.fullRepaint = true;
                entry.hasDirty = false;
                entry.dirty = {};
            } else if (!entry.fullRepaint) {
                auto& reasonDirty = entry.dirtyByReason[reasonIndex(reason)];
                reasonDirty = reasonDirty.isEmpty() ? dirtyArea : reasonDirty.getUnion(dirtyArea);

                if (entry.hasDirty)
                    entry.dirty = entry.dirty.getUnion(dirtyArea);
                else {
                    entry.dirty = dirtyArea;
                    entry.hasDirty = true;
                }
            }
        }

        triggerAsyncUpdate();
    }

    void recordRequestDiagnostics(TimelineReason reason,
                                  const juce::Rectangle<int>& dirtyArea,
                                  bool fullRepaint)
    {
        auto& diagnostics = TimelineRenderingDiagnostics::instance();

        if (reason == TimelineReason::PlayheadOverlay)
            diagnostics.recordOverlayRepaintRequest(dirtyArea);
        else if (reason == TimelineReason::ContentModelInvalid
                 || reason == TimelineReason::FullContent)
            diagnostics.recordContentRepaintRequest(dirtyArea);

        if (fullRepaint)
            diagnostics.recordFullRepaintPromotion();
    }

    juce::Rectangle<int> dirtyForDispatch(const PendingInvalidate& request) const noexcept
    {
        juce::Rectangle<int> dirty;
        const bool dropAnimation = isTimelinePlaybackActive();

        for (std::size_t i = 0; i < request.dirtyByReason.size(); ++i) {
            const auto reason = static_cast<TimelineReason>(i);
            if ((request.reasonMask & reasonBit(reason)) == 0)
                continue;

            if (dropAnimation && reason == TimelineReason::LowPriorityAnimation)
                continue;

            const auto& reasonDirty = request.dirtyByReason[i];
            if (!reasonDirty.isEmpty())
                dirty = dirty.isEmpty() ? reasonDirty : dirty.getUnion(reasonDirty);
        }

        return dirty;
    }

    void handleAsyncUpdate() override
    {
        for (int p = static_cast<int>(Priority::Interactive); p >= static_cast<int>(Priority::Background); --p)
        {
            for (auto it = pending_.begin(); it != pending_.end();)
            {
                const PendingInvalidate request = it->second;

                if (request.priority != p)
                {
                    ++it;
                    continue;
                }

                it = pending_.erase(it);

                if (isTimelinePlaybackActive()
                    && hasOnlyReason(request.reasonMask, TimelineReason::LowPriorityAnimation))
                {
                    TimelineRenderingDiagnostics::instance().recordLowPriorityAnimationDrop();
                    continue;
                }

                auto safeComp = request.component;
                if (safeComp == nullptr || !safeComp->isShowing())
                    continue;

                auto* comp = safeComp.getComponent();
                TimelineRenderingDiagnostics::instance().recordDispatchedFrame();

                if (request.fullRepaint)
                {
                    comp->repaint();
                    continue;
                }

                if (request.hasDirty)
                {
                    const auto dirty = dirtyForDispatch(request).getIntersection(comp->getLocalBounds());
                    if (!dirty.isEmpty())
                        comp->repaint(dirty);
                }
            }
        }
    }
};

} // namespace OpenTune

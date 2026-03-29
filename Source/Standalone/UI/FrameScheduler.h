#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <unordered_map>

namespace OpenTune {

class FrameScheduler final : private juce::AsyncUpdater
{
public:
    enum class Priority : int
    {
        Background = 0,
        Normal = 1,
        Interactive = 2
    };

    static FrameScheduler& instance()
    {
        static FrameScheduler scheduler;
        return scheduler;
    }

    void requestInvalidate(juce::Component& component,
                           const juce::Rectangle<int>& dirtyArea,
                           Priority priority = Priority::Normal)
    {
        if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            juce::MessageManager::callAsync([this, safe = juce::Component::SafePointer<juce::Component>(&component), dirtyArea, priority]() {
                if (safe != nullptr)
                    requestInvalidate(*safe, dirtyArea, priority);
            });
            return;
        }

        auto it = pending_.find(&component);
        if (it == pending_.end()) {
            auto& entry = pending_[&component];
            entry.component = juce::Component::SafePointer<juce::Component>(&component);
            entry.fullRepaint = false;
            entry.dirty = dirtyArea;
            entry.hasDirty = true;
            entry.priority = static_cast<int>(priority);
        } else {
            it->second.fullRepaint = false;
            if (it->second.hasDirty)
                it->second.dirty = it->second.dirty.getUnion(dirtyArea);
            else {
                it->second.dirty = dirtyArea;
                it->second.hasDirty = true;
            }
            it->second.priority = std::max(it->second.priority, static_cast<int>(priority));
        }
        triggerAsyncUpdate();
    }

    void requestInvalidate(juce::Component& component,
                           Priority priority = Priority::Normal)
    {
        if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            juce::MessageManager::callAsync([this, safe = juce::Component::SafePointer<juce::Component>(&component), priority]() {
                if (safe != nullptr)
                    requestInvalidate(*safe, priority);
            });
            return;
        }

        auto it = pending_.find(&component);
        if (it == pending_.end()) {
            auto& entry = pending_[&component];
            entry.component = juce::Component::SafePointer<juce::Component>(&component);
            entry.fullRepaint = true;
            entry.hasDirty = false;
            entry.priority = static_cast<int>(priority);
        } else {
            it->second.fullRepaint = true;
            it->second.hasDirty = false;
            it->second.priority = std::max(it->second.priority, static_cast<int>(priority));
        }
        triggerAsyncUpdate();
    }

private:
    struct PendingInvalidate
    {
        juce::Component::SafePointer<juce::Component> component;
        juce::Rectangle<int> dirty;
        bool hasDirty = false;
        bool fullRepaint = false;
        int priority = static_cast<int>(Priority::Background);
    };

    std::unordered_map<juce::Component*, PendingInvalidate> pending_;

    FrameScheduler() = default;

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

                auto safeComp = request.component;
                if (safeComp == nullptr || !safeComp->isShowing())
                    continue;

                auto* comp = safeComp.getComponent();

                if (request.fullRepaint)
                {
                    comp->repaint();
                    continue;
                }

                if (request.hasDirty)
                {
                    const auto dirty = request.dirty.getIntersection(comp->getLocalBounds());
                    if (!dirty.isEmpty())
                        comp->repaint(dirty);
                    else
                        comp->repaint();
                }
            }
        }
    }
};

} // namespace OpenTune

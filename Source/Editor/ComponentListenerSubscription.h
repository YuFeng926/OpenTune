#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

class ComponentListenerSubscription final
{
public:
    explicit ComponentListenerSubscription(juce::ComponentListener& listener) noexcept
        : listener_(listener)
    {
    }

    ~ComponentListenerSubscription()
    {
        reset();
    }

    ComponentListenerSubscription(const ComponentListenerSubscription&) = delete;
    ComponentListenerSubscription& operator=(const ComponentListenerSubscription&) = delete;
    ComponentListenerSubscription(ComponentListenerSubscription&&) = delete;
    ComponentListenerSubscription& operator=(ComponentListenerSubscription&&) = delete;

    void watch(juce::Component& component)
    {
        assertMessageThread();
        if (target_.getComponent() == &component)
            return;

        reset();
        target_ = &component;
        component.addComponentListener(&listener_);
    }

    void reset()
    {
        assertMessageThread();
        auto* component = target_.getComponent();
        target_ = nullptr;
        if (component != nullptr)
            component->removeComponentListener(&listener_);
    }

    bool watches(juce::Component& component) const
    {
        assertMessageThread();
        return target_.getComponent() == &component;
    }

    juce::Component* target() const
    {
        assertMessageThread();
        return target_.getComponent();
    }

    bool resetIfWatching(juce::Component& component)
    {
        assertMessageThread();
        if (target_.getComponent() != &component)
            return false;

        target_ = nullptr;
        component.removeComponentListener(&listener_);
        return true;
    }

private:
    static void assertMessageThread()
    {
#if JUCE_ASSERTIONS_ENABLED_OR_LOGGED
        const auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
#endif
    }

    juce::ComponentListener& listener_;
    juce::Component::SafePointer<juce::Component> target_;
};

} // namespace OpenTune

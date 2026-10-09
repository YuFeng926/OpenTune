#include <juce_gui_basics/juce_gui_basics.h>

#include "Editor/ComponentListenerSubscription.h"

#include <cstdio>
#include <memory>
#include <type_traits>

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct Listener final : juce::ComponentListener
{
    OpenTune::ComponentListenerSubscription subscription{*this};
    int visibilityChanges = 0;
    int deletionCallbacks = 0;
    bool deletionWasUnsubscribed = false;

    void componentVisibilityChanged(juce::Component&) override
    {
        ++visibilityChanges;
    }

    void componentBeingDeleted(juce::Component& component) override
    {
        ++deletionCallbacks;
        deletionWasUnsubscribed = subscription.resetIfWatching(component);
    }

    void componentParentHierarchyChanged(juce::Component&) override {}
};

void testSubscriptionContract()
{
    static_assert(!std::is_copy_constructible_v<OpenTune::ComponentListenerSubscription>);
    static_assert(!std::is_copy_assignable_v<OpenTune::ComponentListenerSubscription>);
    static_assert(!std::is_move_constructible_v<OpenTune::ComponentListenerSubscription>);
    static_assert(!std::is_move_assignable_v<OpenTune::ComponentListenerSubscription>);

    juce::Component first;
    juce::Component second;
    Listener listener;
    first.setVisible(true);
    second.setVisible(true);

    listener.subscription.watch(first);
    listener.subscription.watch(first);
    check(listener.subscription.watches(first), "watching the same target is idempotent");
    first.setVisible(false);
    check(listener.visibilityChanges == 1, "same-target watch registers one listener");

    listener.subscription.watch(second);
    check(!listener.subscription.watches(first) && listener.subscription.watches(second),
          "watch switches to the new target");
    first.setVisible(true);
    check(listener.visibilityChanges == 1, "switch removes the old listener");
    second.setVisible(false);
    check(listener.visibilityChanges == 2, "switch registers the new listener");

    listener.subscription.reset();
    listener.subscription.reset();
    check(listener.subscription.target() == nullptr, "repeated reset clears the target");
    second.setVisible(true);
    check(listener.visibilityChanges == 2, "reset removes the listener");

    auto target = std::make_unique<juce::Component>();
    listener.subscription.watch(*target);
    target.reset();
    check(listener.deletionCallbacks == 1 && listener.deletionWasUnsubscribed,
          "target deletion resets the subscription from its callback");
    check(listener.subscription.target() == nullptr, "deleted target is no longer tracked");
}

void testListenerDiesFirst()
{
    juce::Component target;
    {
        Listener listener;
        listener.subscription.watch(target);
    }

    target.setVisible(false);
    target.setVisible(true);
}

struct OwnedContent final : juce::Component, juce::ComponentListener
{
    struct State
    {
        int contentDestructions = 0;
        int ownerDeletionCallbacks = 0;
    };

    OwnedContent(juce::Component& owner, State& state)
        : state_(state), subscription_(*this)
    {
        subscription_.watch(owner);
    }

    ~OwnedContent() override
    {
        ++state_.contentDestructions;
    }

    void componentBeingDeleted(juce::Component& component) override
    {
        if (subscription_.resetIfWatching(component))
            ++state_.ownerDeletionCallbacks;
    }

    void componentParentHierarchyChanged(juce::Component&) override {}

    State& state_;
    OpenTune::ComponentListenerSubscription subscription_;
};

void testOwnedContentDestructionOrder()
{
    {
        OwnedContent::State state;
        {
            juce::ResizableWindow window("Resizable test", juce::Colours::black, false);
            window.setContentOwned(new OwnedContent(window, state), true);
        }
        check(state.contentDestructions == 1, "ResizableWindow destroys its owned content");
        check(state.ownerDeletionCallbacks == 0,
              "owned content unsubscribes before ResizableWindow Component teardown");
    }

    {
        OwnedContent::State state;
        {
            juce::DialogWindow window("Dialog test", juce::Colours::black, false, false);
            window.setContentOwned(new OwnedContent(window, state), true);
        }
        check(state.contentDestructions == 1, "DialogWindow destroys its owned content");
        check(state.ownerDeletionCallbacks == 0,
              "owned content unsubscribes before DialogWindow Component teardown");
    }
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    testSubscriptionContract();
    testListenerDiesFirst();
    testOwnedContentDestructionOrder();

    if (failures == 0)
    {
        std::puts("ComponentListenerSubscriptionTests: all checks passed.");
        return 0;
    }

    std::fprintf(stderr, "ComponentListenerSubscriptionTests: %d check(s) failed.\n", failures);
    return 1;
}

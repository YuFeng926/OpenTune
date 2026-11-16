#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>
#include <vector>

#include "Utils/LocalizationManager.h"
#include "Utils/ScreenReaderAnnouncer.h"
#include "Standalone/UI/AuroraLookAndFeel.h"
#include "Standalone/UI/OpenTuneLookAndFeel.h"
#include "Standalone/UI/UIColors.h"

namespace OpenTune {

// 包装页面使其可滚动：Viewport 填满 tab 区域，内部页面保持内容高度
class ScrollablePage : public juce::Viewport
{
public:
    explicit ScrollablePage(std::unique_ptr<juce::Component> content, int contentHeight)
    {
        setScrollBarThickness(8);
        // Accessibility: the viewport itself took Tab focus (NVDA: "unknown")
        // and the scroll bars are mouse-only. Page controls stay reachable.
        setWantsKeyboardFocus(false);
        getVerticalScrollBar().setAccessible(false);
        getHorizontalScrollBar().setAccessible(false);
        // setViewedComponent 将 content 放入内部 contentHolder
        // content 保持自身高度，viewport 自动提供滚动
        contentHeight_ = contentHeight;
        content_ = std::move(content);
        setViewedComponent(content_.get(), false);
        content_->setSize(600, contentHeight); // 初始宽度，resized 时会更新
    }

    void resized() override
    {
        juce::Viewport::resized();
        // viewport 宽度变化时，同步更新内容宽度（高度保持不变以支持滚动）
        if (content_ != nullptr)
            content_->setSize(getWidth(), contentHeight_);
    }

private:
    std::unique_ptr<juce::Component> content_;
    int contentHeight_ = 0;
};

class TabbedPreferencesDialog : public juce::Component
{
public:
    struct PageSpec {
        juce::String title;
        std::unique_ptr<juce::Component> content;
        int totalHeight = 0; // 内容总高度，用于判断是否需要滚动
    };

    explicit TabbedPreferencesDialog(std::vector<PageSpec> pages)
    {
        applyCurrentLookAndFeel();

        addAndMakeVisible(tabbedComponent_);
        tabbedComponent_.setColour(juce::TabbedComponent::backgroundColourId, UIColors::backgroundDark);
        tabbedComponent_.setColour(juce::TabbedButtonBar::tabOutlineColourId, UIColors::panelBorder);
        tabbedComponent_.setTabBarDepth(32);

        for (auto& page : pages) {
            jassert(page.content != nullptr);
            if (page.content == nullptr) {
                continue;
            }

            nameControlsByPrecedingLabels(*page.content);

            // 统一包装为可滚动：内容比视口矮时滚动条不出现，比视口高时自动出现
            auto* scrollable = new ScrollablePage(std::move(page.content), page.totalHeight);
            tabbedComponent_.addTab(page.title, UIColors::backgroundDark, scrollable, true);
        }

        // Accessibility: tab buttons do not take focus by default, so Tab skipped them.
        // Only the current tab is a Tab stop; arrows move between tabs (see keyPressed).
        auto& tabBar = tabbedComponent_.getTabbedButtonBar();
        updateTabStops();
        // The tab bar acted as a keyboard focus container: Tab cycled through the tab buttons
        // only and never reached the page or the Close button.
        tabBar.setFocusContainerType(juce::Component::FocusContainerType::none);
        tabbedComponent_.setFocusContainerType(juce::Component::FocusContainerType::none);
        // On a tab change JUCE moves focus into the new page (toFront(true)), and the tab name
        // announced after that cut off NVDA reading the page's first control, so it sounded as
        // if focus stayed on the tab (user test). Keep focus on the tab button instead, as in
        // Windows tab dialogs: its focus speech names the tab, and Tab goes on into the page.
        tabbedComponent_.onTabChanged = [this](const juce::String& name) {
            updateTabStops();
            auto& bar = tabbedComponent_.getTabbedButtonBar();
            auto* tabButton = bar.getTabButton(bar.getCurrentTabIndex());
            if (tabButton == nullptr || !tabButton->isShowing())
                return;
            if (tabButton->hasKeyboardFocus(false))
                a11y::announce(*this, name);   // focus did not move: no focus speech to rely on
            else
                tabButton->grabKeyboardFocus();
        };

        // Sliders on the pages: Tab lands in the value box editor, which has no name.
        for (int i = 0; i < tabbedComponent_.getNumTabs(); ++i)
            if (auto* page = tabbedComponent_.getTabContentComponent(i))
                addSliderEditorNamers(*page);

        closeButton_.setButtonText(LOC(kClose));
        closeButton_.setColour(juce::TextButton::buttonColourId, UIColors::accent);
        closeButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        closeButton_.onClick = [this] { closeDialog(); };
        addAndMakeVisible(closeButton_);
    }

    ~TabbedPreferencesDialog() override
    {
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(16);
        tabbedComponent_.setBounds(bounds.removeFromTop(bounds.getHeight() - 40));
        closeButton_.setBounds(bounds.removeFromRight(96).withTrimmedTop(6));
    }

    /** 非原生标题栏时移除 DialogWindow 默认标题栏，仅保留主题内容 */
    void parentHierarchyChanged() override
    {
        if (auto* dialogWindow = findParentComponentOfClass<juce::DialogWindow>())
        {
            if (! dialogWindow->isUsingNativeTitleBar())
            {
                const int contentWidth = getWidth();
                const int contentHeight = getHeight();
                dialogWindow->setTitleBarHeight(0);
                dialogWindow->setContentComponentSize(contentWidth, contentHeight);
            }

            // Accessibility: on open, focus sat on the bare window rather than a control.
            // Move it to the selected tab once the window is on screen.
            juce::Component::SafePointer<TabbedPreferencesDialog> safeThis(this);
            juce::Timer::callAfterDelay(100, [safeThis] {
                if (safeThis == nullptr)
                    return;
                auto& bar = safeThis->tabbedComponent_.getTabbedButtonBar();
                if (auto* tabButton = bar.getTabButton(bar.getCurrentTabIndex()))
                    if (tabButton->isShowing())
                        tabButton->grabKeyboardFocus();
            });
        }
    }

    // Ctrl+Tab / Ctrl+Shift+Tab switch to the next / previous tab, as in Windows dialogs.
    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto mods = key.getModifiers();
        // JUCE buttons react to Space through key-state polling, which does not work inside
        // REAPER (NVDA user: Space did not toggle a check box, Enter did). Press the focused
        // button ourselves.
        if (key.getKeyCode() == juce::KeyPress::spaceKey && !mods.isAnyModifierKeyDown()) {
            auto* focused = juce::Component::getCurrentlyFocusedComponent();
            if (auto* button = dynamic_cast<juce::Button*>(focused); button != nullptr && isParentOf(button)) {
                button->triggerClick();
                return true;
            }
        }
        // On a tab button: Up/Left go to the previous tab, Down/Right to the next one (Up/Down
        // as in REAPER's preferences tree, which the user expects), Home/End to the first /
        // last one. No wrap-around: at the ends nothing happens, as in other lists.
        if (!mods.isAnyModifierKeyDown()) {
            auto& bar = tabbedComponent_.getTabbedButtonBar();
            auto* focused = juce::Component::getCurrentlyFocusedComponent();
            const int keyCode = key.getKeyCode();
            const bool previousKey = keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::leftKey;
            const bool nextKey = keyCode == juce::KeyPress::downKey || keyCode == juce::KeyPress::rightKey;
            if (focused != nullptr && bar.isParentOf(focused)
                && (previousKey || nextKey
                    || keyCode == juce::KeyPress::homeKey || keyCode == juce::KeyPress::endKey)) {
                const int count = tabbedComponent_.getNumTabs();
                const int current = tabbedComponent_.getCurrentTabIndex();
                int target = current;
                if (previousKey)                              target = current - 1;
                else if (nextKey)                             target = current + 1;
                else if (keyCode == juce::KeyPress::homeKey)  target = 0;
                else                                          target = count - 1;
                if (target >= 0 && target < count && target != current)
                    tabbedComponent_.setCurrentTabIndex(target);
                return true;
            }
        }
        if (key.getKeyCode() == juce::KeyPress::tabKey && mods.isCtrlDown() && !mods.isAltDown()) {
            const int count = tabbedComponent_.getNumTabs();
            if (count > 0) {
                const int step = mods.isShiftDown() ? count - 1 : 1;
                tabbedComponent_.setCurrentTabIndex((tabbedComponent_.getCurrentTabIndex() + step) % count);
            }
            return true;
        }
        return false;
    }

private:
    // Only the current tab button takes Tab focus, so Tab goes from it straight into the page.
    void updateTabStops()
    {
        auto& bar = tabbedComponent_.getTabbedButtonBar();
        for (int i = 0; i < bar.getNumTabs(); ++i)
            if (auto* tabButton = bar.getTabButton(i))
                tabButton->setWantsKeyboardFocus(i == bar.getCurrentTabIndex());
    }

    // Reports tab changes made by the user, for the focus handling above.
    class AnnouncingTabs final : public juce::TabbedComponent
    {
    public:
        AnnouncingTabs() : juce::TabbedComponent(juce::TabbedButtonBar::TabsAtTop) {}
        std::function<void(const juce::String&)> onTabChanged;

        void currentTabChanged(int, const juce::String& newTabName) override
        {
            if (onTabChanged != nullptr && isShowing())
                onTabChanged(newTabName);
        }
    };

    // Names the TextEditor that a slider's value box opens with the slider's title.
    // The value box is recreated on look-and-feel changes (e.g. theme switch), so the hook
    // is re-installed whenever the slider's children change.
    class SliderEditorNamer final : private juce::ComponentListener
    {
    public:
        explicit SliderEditorNamer(juce::Slider& slider) : slider_(&slider)
        {
            slider.addComponentListener(this);
            hookValueBox();
        }

        ~SliderEditorNamer() override
        {
            if (slider_ != nullptr)
                slider_->removeComponentListener(this);
        }

    private:
        void componentChildrenChanged(juce::Component&) override { hookValueBox(); }

        void hookValueBox()
        {
            if (slider_ == nullptr)
                return;
            for (auto* child : slider_->getChildren()) {
                if (auto* valueBox = dynamic_cast<juce::Label*>(child)) {
                    valueBox->onEditorShow = [valueBox] {
                        auto* editor = valueBox->getCurrentTextEditor();
                        auto* owner = valueBox->getParentComponent();
                        if (editor != nullptr && owner != nullptr)
                            editor->setTitle(owner->getTitle());
                    };
                }
            }
        }

        juce::Component::SafePointer<juce::Slider> slider_;
    };

    void addSliderEditorNamers(juce::Component& parent)
    {
        for (auto* child : parent.getChildren()) {
            if (auto* slider = dynamic_cast<juce::Slider*>(child))
                sliderEditorNamers_.push_back(std::make_unique<SliderEditorNamer>(*slider));
            else
                addSliderEditorNamers(*child);
        }
    }

    // Preference pages are laid out as "caption label, then control". JUCE exposes a label's
    // text twice (as name and value), while combo boxes and sliders get no name at all.
    // Move the caption into the control's title and hide the caption. Buttons with their own
    // changing text (shortcut binding, colour) get the caption as description instead.
    static void nameControlsByPrecedingLabels(juce::Component& page)
    {
        juce::Label* pendingLabel = nullptr;
        for (auto* child : page.getChildren()) {
            if (auto* label = dynamic_cast<juce::Label*>(child)) {
                pendingLabel = label;
                continue;
            }
            if (pendingLabel != nullptr && child->getTitle().isEmpty()) {
                const auto caption = pendingLabel->getText();
                auto* button = dynamic_cast<juce::Button*>(child);
                if (button != nullptr && button->getButtonText().isNotEmpty()) {
                    button->setDescription(caption);
                    pendingLabel->setAccessible(false);
                } else if (button != nullptr
                           || dynamic_cast<juce::ComboBox*>(child) != nullptr
                           || dynamic_cast<juce::Slider*>(child) != nullptr
                           || dynamic_cast<juce::TextEditor*>(child) != nullptr) {
                    child->setTitle(caption);
                    pendingLabel->setAccessible(false);
                }
            }
            pendingLabel = nullptr;
        }
    }

    void closeDialog()
    {
        if (closing_)
            return;

        closing_ = true;
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    }

    void applyCurrentLookAndFeel()
    {
        if (UIColors::currentThemeId() == ThemeId::Aurora) {
            setLookAndFeel(&auroraLookAndFeel_);
            return;
        }

        setLookAndFeel(&openTuneLookAndFeel_);
    }

    AnnouncingTabs tabbedComponent_;
    // Declared after tabbedComponent_ so they are destroyed first, while the sliders still exist.
    std::vector<std::unique_ptr<SliderEditorNamer>> sliderEditorNamers_;
    juce::TextButton closeButton_;
    OpenTuneLookAndFeel openTuneLookAndFeel_;
    AuroraLookAndFeel auroraLookAndFeel_;
    bool closing_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TabbedPreferencesDialog)
};

} // namespace OpenTune

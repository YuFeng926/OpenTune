#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>
#include <vector>

#include "Utils/LocalizationManager.h"
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

            // 统一包装为可滚动：内容比视口矮时滚动条不出现，比视口高时自动出现
            auto* scrollable = new ScrollablePage(std::move(page.content), page.totalHeight);
            tabbedComponent_.addTab(page.title, UIColors::backgroundDark, scrollable, true);
        }

        closeButton_.setButtonText(LOC(kClose));
        closeButton_.setColour(juce::TextButton::buttonColourId, UIColors::accent);
        closeButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        closeButton_.onClick = [this] {
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
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

private:
    void applyCurrentLookAndFeel()
    {
        if (UIColors::currentThemeId() == ThemeId::Aurora) {
            setLookAndFeel(&auroraLookAndFeel_);
            return;
        }

        setLookAndFeel(&openTuneLookAndFeel_);
    }

    juce::TabbedComponent tabbedComponent_{juce::TabbedButtonBar::TabsAtTop};
    juce::TextButton closeButton_;
    OpenTuneLookAndFeel openTuneLookAndFeel_;
    AuroraLookAndFeel auroraLookAndFeel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TabbedPreferencesDialog)
};

} // namespace OpenTune

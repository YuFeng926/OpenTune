#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <memory>
#include <vector>
#include "BinaryData.h"
#include "ComponentListenerSubscription.h"
#include "../Standalone/UI/UIColors.h"
#include "../Utils/LocalizationManager.h"

namespace OpenTune {

/**
 * “关于”弹窗：渲染内嵌的 Resources/About/about.md。
 *
 * 支持的 Markdown 子集：`# ` 一级标题、`## ` 二级标题、普通文本行、空行分段，
 * 以及行内链接 `[文字](地址)`。链接显示为按钮（不显示地址），点击后用系统浏览器打开。
 * 背景、字体与配色沿用 ConfirmDialogContent 的主题样式。
 */
class AboutDialogContent : public juce::Component,
                           private juce::ComponentListener
{
public:
    static void show(juce::Component* parent)
    {
        const auto markdown = juce::String::fromUTF8(BinaryData::about_md, BinaryData::about_mdSize);
        auto* content = new AboutDialogContent(markdown);

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(content);
        options.dialogTitle = {};
        options.dialogBackgroundColour = UIColors::backgroundDark;
        options.escapeKeyTriggersCloseButton = false;
        options.useNativeTitleBar = false;
        options.resizable = false;
        options.componentToCentreAround = parent;

        if (parent != nullptr)
            content->parentSubscription_.watch(*parent);

        options.launchAsync();
    }

    explicit AboutDialogContent(const juce::String& markdown)
        : parentSubscription_(*this)
    {
        setWantsKeyboardFocus(true);

        buildItems(markdown);
        viewport_.setViewedComponent(&body_, false);
        viewport_.setScrollBarsShown(true, false);
        viewport_.setScrollBarThickness(8);
        addAndMakeVisible(viewport_);

        closeButton_.setButtonText(LOC(kClose));
        styleButton(closeButton_, true);
        closeButton_.onClick = [this] { closeDialog(); };
        addAndMakeVisible(closeButton_);

        const int bodyHeight = layoutBody(kWidth - kMargin * 2);
        const int viewportHeight = juce::jmin(bodyHeight, kMaxBodyHeight);
        setSize(kWidth, kMargin + viewportHeight + kContentSpacing + kButtonHeight + kMargin);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(kMargin);
        auto buttonRow = bounds.removeFromBottom(kButtonHeight);
        bounds.removeFromBottom(kContentSpacing);

        const int closeWidth = juce::jmax(kCloseMinWidth, closeButton_.getBestWidthForHeight(kButtonHeight));
        closeButton_.setBounds(buttonRow.removeFromRight(closeWidth));

        viewport_.setBounds(bounds);
        const bool scrolls = layoutBody(bounds.getWidth()) > bounds.getHeight();
        if (scrolls)
            layoutBody(bounds.getWidth() - viewport_.getScrollBarThickness() - 4);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey)
        {
            closeDialog();
            return true;
        }
        return false;
    }

    void parentHierarchyChanged() override
    {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>();
            dialog != nullptr && !dialog->isUsingNativeTitleBar())
        {
            const int preferredWidth = getWidth();
            const int preferredHeight = getHeight();
            dialog->setTitleBarHeight(0);
            dialog->setContentComponentSize(preferredWidth, preferredHeight);
        }

        if (isShowing())
            takeKeyboardFocus();
    }

    void visibilityChanged() override
    {
        if (isShowing())
            takeKeyboardFocus();
    }

private:
    enum class Style
    {
        heading1,
        heading2,
        body
    };

    /** 按 TextLayout 排版的一段文字，宽度不足时自动换行。 */
    class TextRun final : public juce::Component
    {
    public:
        TextRun(const juce::String& text, Style style) : text_(text), style_(style)
        {
            setInterceptsMouseClicks(false, false);
        }

        int preferredWidth() const
        {
            juce::TextLayout layout;
            layout.createLayout(attributed(), 100000.0f);
            return juce::roundToInt(std::ceil(layout.getWidth()));
        }

        int heightForWidth(int width) const
        {
            juce::TextLayout layout;
            layout.createLayout(attributed(), static_cast<float>(width));
            return juce::roundToInt(std::ceil(layout.getHeight()));
        }

        void paint(juce::Graphics& g) override
        {
            juce::TextLayout layout;
            layout.createLayout(attributed(), static_cast<float>(getWidth()));
            layout.draw(g, getLocalBounds().toFloat());
        }

    private:
        juce::AttributedString attributed() const
        {
            juce::AttributedString as;
            as.setWordWrap(juce::AttributedString::byWord);
            as.append(text_, fontFor(style_), UIColors::textPrimary);
            return as;
        }

        juce::String text_;
        Style style_;
    };

    struct Item
    {
        juce::Component* component = nullptr;
        TextRun* text = nullptr;
        juce::TextButton* link = nullptr;
    };

    struct Line
    {
        Style style = Style::body;
        bool paragraphBreak = false;
        std::vector<Item> items;
    };

    static constexpr int kWidth = 480;
    static constexpr int kMargin = 24;
    static constexpr int kContentSpacing = 16;
    static constexpr int kButtonHeight = 32;
    static constexpr int kCloseMinWidth = 80;
    static constexpr int kLinkHeight = 26;
    static constexpr int kItemGap = 8;
    static constexpr int kLineGap = 6;
    static constexpr int kParagraphGap = 14;
    static constexpr int kMaxBodyHeight = 460;

    static juce::Font fontFor(Style style)
    {
        switch (style)
        {
            case Style::heading1: return UIColors::getDisplayFont(22.0f);
            case Style::heading2: return UIColors::getHeaderFont(18.0f);
            case Style::body:     return UIColors::getUIFont();
        }
        return UIColors::getUIFont();
    }

    static void styleButton(juce::TextButton& button, bool accent)
    {
        button.setColour(juce::TextButton::buttonColourId, accent ? UIColors::accent : UIColors::backgroundLight);
        button.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        button.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
        button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void addText(Line& line, const juce::String& text)
    {
        const auto trimmed = text.trim();
        if (trimmed.isEmpty())
            return;

        auto& run = texts_.emplace_back(std::make_unique<TextRun>(trimmed, line.style));
        body_.addAndMakeVisible(*run);
        line.items.push_back({ run.get(), run.get(), nullptr });
    }

    void addLink(Line& line, const juce::String& label, const juce::String& url)
    {
        auto& button = links_.emplace_back(std::make_unique<juce::TextButton>(label.trim()));
        styleButton(*button, false);

        const auto target = url.trim();
        button->onClick = [target] {
            if (target.startsWithIgnoreCase("https://") || target.startsWithIgnoreCase("http://")
                || target.startsWithIgnoreCase("mailto:"))
            {
                // juce::URL 会拆开 fragment，再用 addEscapeChars 拼回去，
                // `#/artist?id=…` 会变成 `#%2Fartist%3Fid%3D…`，哈希路由打不开。
                // 不解析，把链接原文交给系统浏览器。
                juce::URL::createWithoutParsing(target).launchInDefaultBrowser();
            }
        };

        body_.addAndMakeVisible(*button);
        line.items.push_back({ button.get(), nullptr, button.get() });
    }

    /** 拆分一行中的普通文字与 [文字](地址) 链接。 */
    void parseInline(Line& line, const juce::String& text)
    {
        int position = 0;
        while (position < text.length())
        {
            const int open = text.indexOfChar(position, '[');
            const int close = open >= 0 ? text.indexOfChar(open + 1, ']') : -1;
            const bool isLink = close > open && close + 1 < text.length() && text[close + 1] == '(';
            const int end = isLink ? text.indexOfChar(close + 2, ')') : -1;

            if (open < 0 || !isLink || end < 0)
            {
                addText(line, text.substring(position));
                return;
            }

            addText(line, text.substring(position, open));
            addLink(line, text.substring(open + 1, close), text.substring(close + 2, end));
            position = end + 1;
        }
    }

    void buildItems(const juce::String& markdown)
    {
        bool pendingBreak = false;
        for (auto raw : juce::StringArray::fromLines(markdown))
        {
            const auto text = raw.trimEnd();
            if (text.trim().isEmpty())
            {
                pendingBreak = !lines_.empty();
                continue;
            }

            Line line;
            auto content = text.trimStart();
            if (content.startsWith("## "))
            {
                line.style = Style::heading2;
                content = content.substring(3);
            }
            else if (content.startsWith("# "))
            {
                line.style = Style::heading1;
                content = content.substring(2);
            }

            // 标题前总是留出段落间距
            line.paragraphBreak = !lines_.empty() && (pendingBreak || line.style != Style::body);
            pendingBreak = false;

            parseInline(line, content);
            if (!line.items.empty())
                lines_.push_back(std::move(line));
        }
    }

    /** 按给定宽度自左向右排布每行元素，放不下时换行；返回内容总高度。 */
    int layoutBody(int width)
    {
        int y = 0;
        for (size_t lineIndex = 0; lineIndex < lines_.size(); ++lineIndex)
        {
            const auto& line = lines_[lineIndex];
            if (lineIndex > 0)
                y += line.paragraphBreak ? kParagraphGap : kLineGap;

            const bool hasLink = std::any_of(line.items.begin(), line.items.end(),
                                             [](const Item& item) { return item.link != nullptr; });
            int x = 0;
            int rowHeight = 0;
            for (const auto& item : line.items)
            {
                int itemWidth = 0;
                int itemHeight = 0;
                if (item.link != nullptr)
                {
                    itemWidth = juce::jmin(width, item.link->getBestWidthForHeight(kLinkHeight) + 8);
                    itemHeight = kLinkHeight;
                }
                else
                {
                    itemWidth = juce::jmin(width, item.text->preferredWidth() + 2);
                    itemHeight = item.text->heightForWidth(itemWidth);
                }

                if (x > 0 && x + itemWidth > width)
                {
                    y += rowHeight + kLineGap;
                    x = 0;
                    rowHeight = 0;
                }

                // 与链接按钮同行的单行文字按按钮高度垂直居中
                const int offset = hasLink && itemHeight < kLinkHeight ? (kLinkHeight - itemHeight) / 2 : 0;
                item.component->setBounds(x, y + offset, itemWidth, itemHeight);
                rowHeight = juce::jmax(rowHeight, offset + itemHeight);
                x += itemWidth + kItemGap;
            }
            y += rowHeight;
        }

        body_.setSize(width, y);
        return y;
    }

    void closeDialog()
    {
        if (closing_)
            return;
        closing_ = true;

        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState(0);
    }

    void componentBeingDeleted(juce::Component& component) override
    {
        if (parentSubscription_.resetIfWatching(component))
            closeDialog();
    }

    void takeKeyboardFocus()
    {
        juce::Component::SafePointer<AboutDialogContent> safeThis(this);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr && safeThis->isShowing())
                safeThis->grabKeyboardFocus();
        });
    }

    juce::Component body_;
    std::vector<std::unique_ptr<TextRun>> texts_;
    std::vector<std::unique_ptr<juce::TextButton>> links_;
    std::vector<Line> lines_;
    juce::Viewport viewport_;
    juce::TextButton closeButton_;
    bool closing_ = false;
    ComponentListenerSubscription parentSubscription_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutDialogContent)
};

} // namespace OpenTune

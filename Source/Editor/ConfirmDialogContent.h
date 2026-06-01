#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <functional>
#include <vector>
#include "../Standalone/UI/UIColors.h"

namespace OpenTune {

/**
 * @brief 通用确认/消息弹窗内容组件
 *
 * 复用设置页样式（UIColors 主题色），替代 JUCE 原生 AlertWindow。
 * 通过 DialogWindow::LaunchOptions 弹出。
 *
 * 用法：
 *   auto* content = new ConfirmDialogContent(u8"标题", u8"正文消息",
 *       { {u8"保存", [=]{ ... }}, {u8"不保存", [=]{ ... }}, {u8"取消", nullptr} });
 *   ConfirmDialogContent::launch(content, parentComponent);
 *
 * 便捷用法：
 *   ConfirmDialogContent::showMessage(parent, u8"标题", u8"消息");
 *   ConfirmDialogContent::showChoice(parent, u8"标题", u8"确认吗？", u8"保存", []{...});
 *   ConfirmDialogContent::showTriple(parent, u8"标题", u8"请选择", []{...}, []{...}, []{...});
 */
class ConfirmDialogContent : public juce::Component
{
public:
    struct ButtonSpec
    {
        juce::String text;
        std::function<void()> onClick; // nullptr = 仅关闭
        bool isAccent = false;         // 使用 accent 色（主操作按钮）
    };

    // ============================================================================
    // Construction
    // ============================================================================

    ConfirmDialogContent(const juce::String& title,
                         const juce::String& message,
                         std::vector<ButtonSpec> buttons)
        : title_(title), message_(message), buttonSpecs_(std::move(buttons))
    {
        // 计算合适尺寸
        constexpr int dialogWidth = 380;
        constexpr int margin = 24;
        constexpr int titleHeight = 24;
        constexpr int buttonRowHeight = 36;
        constexpr int spacing = 16;
        constexpr int maxMessageHeight = 200;
        // 动态计算消息高度
        juce::Font msgFont = UIColors::getUIFont(14.0f);
        const int messageWidth = dialogWidth - margin * 2;
        const int lineHeight = static_cast<int>(std::ceil(msgFont.getHeight()));
        const int requiredHeight = msgFont.getStringWidth(message_) > 0
            ? juce::jmax(lineHeight, (msgFont.getStringWidth(message_) / messageWidth + 1) * lineHeight)
            : lineHeight;
        const int messageAreaHeight = juce::jmin(juce::jmax(lineHeight, requiredHeight), maxMessageHeight);

        const int totalHeight = margin + titleHeight + spacing + messageAreaHeight + spacing + buttonRowHeight + margin;

        setSize(dialogWidth, totalHeight);

        // 创建按钮并确定默认按钮
        int defaultButtonIdx = 0;
        for (size_t i = 0; i < buttonSpecs_.size(); ++i) {
            if (buttonSpecs_[i].isAccent) {
                defaultButtonIdx = static_cast<int>(i);
                break;
            }
        }

        for (size_t i = 0; i < buttonSpecs_.size(); ++i) {
            auto* btn = buttons_.add(new juce::TextButton(buttonSpecs_[i].text));
            btn->setWantsKeyboardFocus(false);
            if (buttonSpecs_[i].isAccent)
                btn->setColour(juce::TextButton::buttonColourId, UIColors::accent);
            else
                btn->setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
            btn->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
            btn->setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);

            const size_t idx = i;
            btn->onClick = [this, idx] {
                if (buttonSpecs_[idx].onClick)
                    buttonSpecs_[idx].onClick();
                closeParentDialog();
            };
            addAndMakeVisible(btn);
        }

        // 记录默认按钮索引供 Enter 键使用
        defaultButtonIdx_ = defaultButtonIdx;
        resized();
    }

    // ============================================================================
    // Static convenience methods
    // ============================================================================

    /** 便捷弹出方法，居中于 parent */
    static void launch(ConfirmDialogContent* content, juce::Component* parent)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(content);
        options.dialogTitle = {};
        options.dialogBackgroundColour = UIColors::backgroundDark;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = false;
        options.resizable = false;
        options.componentToCentreAround = parent;
        options.launchAsync();
    }

    /** 便捷方法：单按钮消息提示 */
    static void showMessage(juce::Component* parent, const juce::String& title,
                            const juce::String& message, const juce::String& buttonText = {})
    {
        auto* content = new ConfirmDialogContent(title, message,
            { { buttonText.isEmpty() ? juce::String::fromUTF8(u8"确定") : buttonText, nullptr, true } });
        launch(content, parent);
    }

    /** 便捷方法：双按钮选择 */
    static void showChoice(juce::Component* parent, const juce::String& title,
                           const juce::String& message, const juce::String& confirmText,
                           std::function<void()> onConfirm)
    {
        auto* content = new ConfirmDialogContent(title, message,
            { { confirmText, std::move(onConfirm), true }, { juce::String::fromUTF8(u8"取消"), nullptr, false } });
        launch(content, parent);
    }

    /** 便捷方法：三按钮选择 */
    static void showTriple(juce::Component* parent, const juce::String& title,
                           const juce::String& message,
                           std::function<void()> onPrimary,
                           std::function<void()> onSecondary,
                           std::function<void()> onTertiary,
                           const juce::String& primaryText = {},
                           const juce::String& secondaryText = {},
                           const juce::String& tertiaryText = {})
    {
        auto* content = new ConfirmDialogContent(title, message,
            {
                { primaryText.isEmpty() ? juce::String::fromUTF8(u8"确定") : primaryText, std::move(onPrimary), true },
                { secondaryText.isEmpty() ? juce::String::fromUTF8(u8"取消") : secondaryText, std::move(onSecondary), false },
                { tertiaryText.isEmpty() ? juce::String::fromUTF8(u8"更多") : tertiaryText, std::move(onTertiary), false }
            });
        launch(content, parent);
    }

    // ============================================================================
    // Component overrides
    // ============================================================================

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);

        constexpr int margin = 24;
        auto bounds = getLocalBounds().reduced(margin);

        // Title
        g.setColour(UIColors::textPrimary);
        g.setFont(UIColors::getUIFont(16.0f));
        g.drawText(title_, bounds.removeFromTop(24), juce::Justification::centredLeft, true);

        bounds.removeFromTop(12);

        // Message - 动态高度计算
        g.setColour(UIColors::textSecondary);
        g.setFont(UIColors::getUIFont(14.0f));
        const int messageHeight = getHeight() - margin * 2 - 24 - 12 - 36 - 16;
        g.drawFittedText(message_, bounds.removeFromTop(juce::jmax(24, messageHeight)),
                         juce::Justification::centredLeft, 5);
    }

    void resized() override
    {
        constexpr int margin = 24;
        constexpr int buttonHeight = 32;
        constexpr int buttonGap = 10;
        constexpr int buttonMinWidth = 80;

        auto bounds = getLocalBounds().reduced(margin);

        // 按钮行 — 右对齐
        auto buttonRow = bounds.removeFromBottom(buttonHeight);

        int totalButtonWidth = 0;
        for (auto* btn : buttons_)
            totalButtonWidth += juce::jmax(buttonMinWidth, btn->getBestWidthForHeight(buttonHeight));
        totalButtonWidth += buttonGap * juce::jmax(0, static_cast<int>(buttons_.size()) - 1);

        int x = buttonRow.getRight() - totalButtonWidth;
        for (auto* btn : buttons_) {
            const int w = juce::jmax(buttonMinWidth, btn->getBestWidthForHeight(buttonHeight));
            btn->setBounds(x, buttonRow.getY(), w, buttonHeight);
            x += w + buttonGap;
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey) {
            closeParentDialog();
            return true;
        }
        if (key == juce::KeyPress::returnKey) {
            // 按 Enter 触发默认按钮
            if (defaultButtonIdx_ >= 0 && defaultButtonIdx_ < static_cast<int>(buttons_.size())) {
                buttons_[defaultButtonIdx_]->triggerClick();
                return true;
            }
        }
        return false;
    }

private:
    juce::String title_;
    juce::String message_;
    std::vector<ButtonSpec> buttonSpecs_;
    juce::OwnedArray<juce::TextButton> buttons_;
    int defaultButtonIdx_ = 0;

    void closeParentDialog()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->closeButtonPressed();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ConfirmDialogContent)
};

} // namespace OpenTune

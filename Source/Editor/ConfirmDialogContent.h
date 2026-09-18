#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <functional>
#include <vector>
#include "../Standalone/UI/UIColors.h"
#include "../Utils/LocalizationManager.h"

namespace OpenTune {

/**
 * @brief 通用确认/消息弹窗内容组件
 *
 * 复用设置页样式（UIColors 主题色），替代 JUCE 默认消息框。
 * 无 JUCE 标题栏（标题由内容自绘），通过 DialogWindow::LaunchOptions 弹出。
 *
 * 键盘行为：
 * - Enter：内容聚焦时触发 accent 默认按钮；按钮聚焦时由按钮自身触发。
 * - Tab：在按钮之间切换焦点。
 * - Esc：走 dismiss 路径（onDismissed 回调）。
 * 按钮动作与 onDismissed 互斥且最多执行一次：任一按钮被点击后不会再调用 onDismissed；
 * 窗口关闭（Esc / Alt+F4 / 关闭按钮）走 dismiss 路径触发 onDismissed；
 * 父组件销毁只关闭窗口，不触发任何业务回调。
 *
 * 用法：
 *   auto* content = new ConfirmDialogContent(u8"标题", u8"正文消息",
 *       { {u8"保存", [=]{ ... }}, {u8"不保存", [=]{ ... }}, {LOC(kCancel), nullptr} });
 *   ConfirmDialogContent::launch(content, parentComponent);
 *
 * 便捷用法：
 *   ConfirmDialogContent::showMessage(parent, u8"标题", u8"消息");
 */
class ConfirmDialogContent : public juce::Component,
                             private juce::ComponentListener
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
                         std::vector<ButtonSpec> buttons,
                         std::function<void()> onDismissed = {})
        : title_(title),
          message_(message),
          buttonSpecs_(std::move(buttons)),
          onDismissed_(std::move(onDismissed))
    {
        setWantsKeyboardFocus(true);

        // 创建按钮并确定默认按钮（先有按钮才能按最佳宽度决定弹窗宽度）
        for (size_t i = 0; i < buttonSpecs_.size(); ++i) {
            if (buttonSpecs_[i].isAccent) {
                defaultButtonIdx_ = static_cast<int>(i);
                break;
            }
        }

        for (size_t i = 0; i < buttonSpecs_.size(); ++i) {
            auto* btn = buttons_.add(new juce::TextButton(buttonSpecs_[i].text));
            btn->setWantsKeyboardFocus(true);
            if (buttonSpecs_[i].isAccent)
                btn->setColour(juce::TextButton::buttonColourId, UIColors::accent);
            else
                btn->setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
            btn->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
            btn->setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);

            const size_t idx = i;
            btn->onClick = [this, idx] { triggerButton(idx); };
            addAndMakeVisible(btn);
        }

        // 按按钮最佳宽度扩大弹窗，长文案不溢出；上限 kMaxWidth
        const int preferredWidth = kMargin * 2 + getButtonsTotalWidth();
        const int dialogWidth = juce::jlimit(kMinWidth, kMaxWidth, preferredWidth);

        // 消息高度按最终宽度计算
        const juce::Font msgFont = UIColors::getUIFont(14.0f);
        const int messageWidth = dialogWidth - kMargin * 2;
        const int lineHeight = static_cast<int>(std::ceil(msgFont.getHeight()));
        const float textWidth = juce::TextLayout::getStringWidth(msgFont, message_);
        const int requiredHeight = textWidth > 0.0f
            ? juce::jmax(lineHeight, (static_cast<int>(textWidth / messageWidth) + 1) * lineHeight)
            : lineHeight;
        const int messageAreaHeight = juce::jmin(juce::jmax(lineHeight, requiredHeight), kMaxMessageHeight);

        const int totalHeight = kMargin + kTitleHeight + kContentSpacing + messageAreaHeight
                              + kContentSpacing + kButtonRowHeight + kMargin;

        setSize(dialogWidth, totalHeight);
        resized();
    }

    ~ConfirmDialogContent() override
    {
        unwatchAll();
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
        options.escapeKeyTriggersCloseButton = false; // Esc 由内容处理（dismiss 路径）
        options.useNativeTitleBar = false;
        options.resizable = false;
        options.componentToCentreAround = parent;

        if (parent != nullptr)
            content->watchParent(parent);

        options.launchAsync();
    }

    /** 便捷方法：单按钮消息提示 */
    static void showMessage(juce::Component* parent, const juce::String& title,
                            const juce::String& message, const juce::String& buttonText = {})
    {
        auto* content = new ConfirmDialogContent(title, message,
            { { buttonText.isEmpty() ? LOC(kOK) : buttonText, nullptr, true } });
        launch(content, parent);
    }

    // ============================================================================
    // Component overrides
    // ============================================================================

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);

        auto bounds = getLocalBounds().reduced(kMargin);

        // Title
        g.setColour(UIColors::textPrimary);
        g.setFont(UIColors::getUIFont(16.0f));
        g.drawText(title_, bounds.removeFromTop(kTitleHeight), juce::Justification::centredLeft, true);

        bounds.removeFromTop(kTitleGap);

        // Message - 动态高度计算
        g.setColour(UIColors::textSecondary);
        g.setFont(UIColors::getUIFont(14.0f));
        const int messageHeight = getHeight() - kMargin * 2 - kTitleHeight - kTitleGap
                                - kButtonRowHeight - kContentSpacing;
        g.drawFittedText(message_, bounds.removeFromTop(juce::jmax(24, messageHeight)),
                         juce::Justification::centredLeft, 5);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(kMargin);

        // 按钮行 — 右对齐
        auto buttonRow = bounds.removeFromBottom(kButtonHeight);

        const int count = buttons_.size();
        if (count <= 0)
            return;

        const int gapTotal = kButtonGap * (count - 1);
        const int available = juce::jmax(0, buttonRow.getWidth() - gapTotal);

        // 期望宽度：不低于最小宽度，优先容纳完整文案
        std::vector<int> widths(static_cast<size_t>(count));
        int desiredTotal = 0;
        for (int i = 0; i < count; ++i) {
            widths[static_cast<size_t>(i)] = juce::jmax(kButtonMinWidth,
                                                        buttons_[i]->getBestWidthForHeight(kButtonHeight));
            desiredTotal += widths[static_cast<size_t>(i)];
        }

        // 总宽超出内容宽度时压缩：先削高于最小宽度的按钮，仍不够则继续均分，
        // 保证按钮行不越过左右 margin
        int excess = desiredTotal - available;
        for (int floorWidth : { kButtonMinWidth, 0 })
        {
            while (excess > 0)
            {
                bool progressed = false;
                for (int i = 0; i < count && excess > 0; ++i)
                {
                    auto& w = widths[static_cast<size_t>(i)];
                    if (w > floorWidth)
                    {
                        --w;
                        --excess;
                        progressed = true;
                    }
                }
                if (!progressed)
                    break;
            }
        }

        int totalWidth = gapTotal;
        for (int w : widths)
            totalWidth += w;

        int x = buttonRow.getRight() - totalWidth;
        for (int i = 0; i < count; ++i) {
            const int w = widths[static_cast<size_t>(i)];
            buttons_[i]->setBounds(x, buttonRow.getY(), w, kButtonHeight);
            x += w + kButtonGap;
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey) {
            dismiss();
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

    void visibilityChanged() override
    {
        if (isShowing())
            takeKeyboardFocus();
    }

    void parentHierarchyChanged() override
    {
        auto* dialog = findParentComponentOfClass<juce::DialogWindow>();

        // 跟踪所在 DialogWindow：其被隐藏（Alt+F4 / 关闭按钮）时需要走 dismiss 路径
        if (watchedDialog_ != dialog)
        {
            if (watchedDialog_ != nullptr)
                watchedDialog_->removeComponentListener(this);

            watchedDialog_ = dialog;

            if (watchedDialog_ != nullptr)
                watchedDialog_->addComponentListener(this);
        }

        if (dialog != nullptr)
        {
            if (!dialog->isUsingNativeTitleBar())
            {
                // 此刻窗口尚未按内容尺寸展开，先记住期望尺寸：
                // setTitleBarHeight 会触发窗口 resized，把内容重排到当前窗口大小。
                const int preferredWidth = getWidth();
                const int preferredHeight = getHeight();

                // 移除 JUCE 标题栏（标题由内容自绘），并让窗口重新贴合内容尺寸
                dialog->setTitleBarHeight(0);
                dialog->setContentComponentSize(preferredWidth, preferredHeight);
            }
        }

        if (isShowing())
            takeKeyboardFocus();
    }

private:
    // ============================================================================
    // Layout constants
    // ============================================================================

    static constexpr int kMargin = 24;
    static constexpr int kTitleHeight = 24;
    static constexpr int kTitleGap = 12;
    static constexpr int kContentSpacing = 16;
    static constexpr int kButtonRowHeight = 36;
    static constexpr int kButtonHeight = 32;
    static constexpr int kButtonGap = 10;
    static constexpr int kButtonMinWidth = 80;
    static constexpr int kMinWidth = 380;
    static constexpr int kMaxWidth = 560;
    static constexpr int kMaxMessageHeight = 200;

    // ============================================================================
    // State
    // ============================================================================

    juce::String title_;
    juce::String message_;
    std::vector<ButtonSpec> buttonSpecs_;
    std::function<void()> onDismissed_;
    juce::OwnedArray<juce::TextButton> buttons_;
    int defaultButtonIdx_ = 0;

    // 父组件 / DialogWindow 生命周期监听（SafePointer，不保存裸指针跨异步）
    juce::Component::SafePointer<juce::Component> watchedParent_;
    juce::Component::SafePointer<juce::DialogWindow> watchedDialog_;
    bool closing_ = false;

    // ============================================================================
    // Helpers
    // ============================================================================

    int getButtonsTotalWidth() const
    {
        int total = 0;
        for (auto* btn : buttons_)
            total += juce::jmax(kButtonMinWidth, btn->getBestWidthForHeight(kButtonHeight));
        total += kButtonGap * juce::jmax(0, static_cast<int>(buttons_.size()) - 1);
        return total;
    }

    void triggerButton(size_t idx)
    {
        // 先取出按钮动作并清空 dismiss 回调：两者互斥且只执行一次；
        // 再 exitModalState 结束模态，最后执行动作（动作可能销毁本组件）。
        auto action = std::move(buttonSpecs_[idx].onClick);
        onDismissed_ = {};

        closeDialog();

        if (action)
            action();
    }

    void dismiss()
    {
        // 取出并清空 dismiss 回调，保证与按钮动作互斥且最多执行一次
        auto callback = std::move(onDismissed_);
        closeDialog();

        if (callback)
            callback();
    }

    void closeDialog()
    {
        // 主动关闭标记：DialogWindow 隐藏时不再触发二次 dismiss 回调
        closing_ = true;

        // launchAsync 建立了 modal 状态（deleteWhenDismissed），
        // 用 exitModalState 结束，避免 DefaultDialogWindow::closeButtonPressed 仅隐藏窗口。
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState(0);
    }

    // ============================================================================
    // Parent / DialogWindow lifecycle
    // ============================================================================

    void watchParent(juce::Component* parent)
    {
        watchedParent_ = parent;

        if (watchedParent_ != nullptr)
            watchedParent_->addComponentListener(this);
    }

    void unwatchAll()
    {
        if (watchedParent_ != nullptr)
            watchedParent_->removeComponentListener(this);

        if (watchedDialog_ != nullptr)
            watchedDialog_->removeComponentListener(this);
    }

    void componentBeingDeleted(juce::Component& comp) override
    {
        if (watchedParent_ == nullptr || &comp != watchedParent_.getComponent())
            return;

        // 父组件正在销毁：只关闭窗口，绝不执行业务回调
        onDismissed_ = {};

        for (auto& spec : buttonSpecs_)
            spec.onClick = nullptr;

        closeDialog();
    }

    void componentVisibilityChanged(juce::Component& comp) override
    {
        if (closing_ || watchedDialog_ == nullptr || &comp != watchedDialog_.getComponent())
            return;

        // DialogWindow 被隐藏（Alt+F4 / 关闭按钮）而非主动关闭：走 dismiss 路径
        if (!comp.isShowing())
            dismiss();
    }

    /** launchAsync 会先把键盘焦点交给 DialogWindow，因此延后一拍取回内容焦点。 */
    void takeKeyboardFocus()
    {
        juce::Component::SafePointer<ConfirmDialogContent> safeThis(this);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->grabKeyboardFocus();
        });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ConfirmDialogContent)
};

} // namespace OpenTune

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../Standalone/UI/UIColors.h"
#include "../Utils/LocalizationManager.h"

namespace OpenTune
{

/**
 * @brief 快捷键捕获对话框内容组件
 *
 * 普通 juce::Component（不依赖 JUCE 默认消息框，也不依赖 KeyShortcutConfig），
 * 标题 / 说明 / 取消按钮全部使用 UIColors 主题绘制。
 * 任意按键（含 Esc、Tab）都会被捕获为绑定并回调 onCaptured，不会移焦或取消；
 * 取消（显式取消按钮 / Alt+F4 / 关闭按钮）回调 onCancelled，且最多执行一次；
 * 父组件销毁只关闭窗口，不回调 onCancelled。
 *
 * 用法：
 *   ShortcutCaptureDialogContent::launch (parent, u8"设置快捷键", u8"按下要绑定的按键",
 *       [this] (const juce::KeyPress& key) { ... },
 *       [this] { ... });
 */
class ShortcutCaptureDialogContent final : public juce::Component,
                                           private juce::ComponentListener
{
public:
    /** 捕获回调：返回用户按下的完整 KeyPress（含修饰键）。 */
    using CaptureCallback = std::function<void (const juce::KeyPress&)>;

    /** 取消回调：仅在点击取消按钮时调用。 */
    using CancelCallback = std::function<void()>;

    // ============================================================================
    // Construction
    // ============================================================================

    ShortcutCaptureDialogContent (const juce::String& title,
                                  const juce::String& message,
                                  CaptureCallback onCaptured,
                                  CancelCallback onCancelled = {})
        : title_ (title),
          message_ (message),
          onCaptured_ (std::move (onCaptured)),
          onCancelled_ (std::move (onCancelled))
    {
        setWantsKeyboardFocus (true);

        // 消息区按实际文案测量，至少 4 行（buildCaptureMessage 固定 4 行），
        // 避免俄/西语等较长译文把“当前绑定”一行挤掉。
        measureMessage();

        setSize (kWidth, kChromeHeight + messageHeight_);

        cancelButton_.setButtonText (LOC (kCancel));
        cancelButton_.setColour (juce::TextButton::buttonColourId, UIColors::backgroundLight);
        cancelButton_.setColour (juce::TextButton::buttonOnColourId, UIColors::backgroundLight);
        cancelButton_.setColour (juce::TextButton::textColourOffId, UIColors::textPrimary);
        cancelButton_.setColour (juce::TextButton::textColourOnId, UIColors::textPrimary);
        cancelButton_.onClick = [this] { cancel(); };
        addAndMakeVisible (cancelButton_);
    }

    ~ShortcutCaptureDialogContent() override
    {
        unwatchAll();
    }

    // ============================================================================
    // Async launch API
    // ============================================================================

    /** 异步弹出快捷键捕获对话框；parent 为空时按主显示器居中。 */
    static void launch (juce::Component* parent,
                        const juce::String& title,
                        const juce::String& message,
                        CaptureCallback onCaptured,
                        CancelCallback onCancelled = {})
    {
        auto* content = new ShortcutCaptureDialogContent (title, message,
                                                          std::move (onCaptured),
                                                          std::move (onCancelled));

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (content);
        options.dialogTitle = {};
        options.dialogBackgroundColour = UIColors::backgroundDark;
        options.escapeKeyTriggersCloseButton = false; // Esc 也作为绑定被内容组件捕获
        options.useNativeTitleBar = false;
        options.resizable = false;
        options.componentToCentreAround = parent;

        if (parent != nullptr)
            content->watchParent(parent);

        options.launchAsync();
    }

    // ============================================================================
    // Component overrides
    // ============================================================================

    void paint (juce::Graphics& g) override
    {
        g.fillAll (UIColors::backgroundDark);

        auto bounds = getLocalBounds().reduced (kMargin);

        // 标题（窗口无 JUCE 标题栏，标题在内容里自绘）
        g.setColour (UIColors::textPrimary);
        g.setFont (UIColors::getUIFont (16.0f));
        g.drawText (title_, bounds.removeFromTop (kTitleHeight), juce::Justification::centredLeft, true);

        bounds.removeFromTop (kTitleGap);

        // 说明：按测量高度完整显示，至少 4 行（含“当前绑定”）
        g.setColour (UIColors::textSecondary);
        g.setFont (UIColors::getUIFont (static_cast<float> (kMessageFontHeight)));
        g.drawFittedText (message_, bounds.removeFromTop (messageHeight_),
                          juce::Justification::centredLeft, messageLines_);

        bounds.removeFromTop (kRowGap);

        // 捕获区：聚焦时用 accent 边框
        const bool focused = hasKeyboardFocus (true);
        auto well = bounds.removeFromTop (kWellHeight).toFloat();

        juce::Path wellShape;
        wellShape.addRoundedRectangle (well, 4.0f);
        g.setColour (UIColors::backgroundMedium);
        g.fillPath (wellShape);
        g.setColour (focused ? UIColors::accent : UIColors::panelBorder);
        g.strokePath (wellShape, juce::PathStrokeType (focused ? 1.6f : 1.0f));

        g.setColour (focused ? UIColors::textPrimary : UIColors::textDisabled);
        g.setFont (UIColors::getUIFont (15.0f));
        g.drawText (getCaptureHintText(), well, juce::Justification::centred, true);

        bounds.removeFromTop (kRowGap);

        // 底部提示
        g.setColour (UIColors::textSecondary.withAlpha (0.75f));
        g.setFont (UIColors::getUIFont (12.0f));
        g.drawText (LOC (kShortcutCaptureCancelHint), bounds, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        cancelButton_.setBounds (getLocalBounds().reduced (kMargin)
                                     .removeFromBottom (kBottomHeight)
                                     .removeFromRight (kButtonWidth));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // 任意键（含 Esc、Tab）都作为绑定捕获；取消只能通过取消按钮
        capture (key);
        return true;
    }

    bool keyStateChanged (bool) override
    {
        // 按住修饰键时实时刷新捕获区提示
        repaint();
        return false;
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        grabKeyboardFocus();
    }

    void focusGained (FocusChangeType) override
    {
        repaint();
    }

    void focusLost (FocusChangeType) override
    {
        repaint();
    }

    void visibilityChanged() override
    {
        if (isShowing())
            takeKeyboardFocus();
    }

    void parentHierarchyChanged() override
    {
        auto* dialog = findParentComponentOfClass<juce::DialogWindow>();

        // 跟踪所在 DialogWindow：其被隐藏（Alt+F4 / 关闭按钮）时需要走取消路径
        if (watchedDialog_ != dialog)
        {
            if (watchedDialog_ != nullptr)
                watchedDialog_->removeComponentListener(this);

            watchedDialog_ = dialog;

            if (watchedDialog_ != nullptr)
                watchedDialog_->addComponentListener(this);
        }

        if (dialog != nullptr && ! dialog->isUsingNativeTitleBar())
        {
            // 此刻窗口尚未按内容尺寸展开，先记住期望尺寸：
            // setTitleBarHeight 会触发窗口 resized，把内容重排到当前窗口大小。
            const int preferredWidth = getWidth();
            const int preferredHeight = getHeight();

            // 移除 JUCE 标题栏（标题由内容自绘），并让窗口重新贴合内容尺寸
            dialog->setTitleBarHeight (0);
            dialog->setContentComponentSize (preferredWidth, preferredHeight);
        }

        if (isShowing())
            takeKeyboardFocus();
    }

private:
    // ============================================================================
    // Layout
    // ============================================================================

    static constexpr int kWidth = 440;
    static constexpr int kMargin = 20;
    static constexpr int kTitleHeight = 26;
    static constexpr int kTitleGap = 10;
    static constexpr int kMessageFontHeight = 14;
    static constexpr int kMessageLineHeight = 20;
    static constexpr int kMessageMinLines = 4; // buildCaptureMessage：名称 / 提示 / 空行 / 当前绑定
    static constexpr int kRowGap = 12;
    static constexpr int kWellHeight = 54;
    static constexpr int kBottomHeight = 32;
    static constexpr int kButtonWidth = 92;

    // 除消息区外的固定高度：上下边距 + 标题 + 间隔 + 捕获区 + 间隔 + 底部按钮
    static constexpr int kChromeHeight = kMargin * 2 + kTitleHeight + kTitleGap
                                       + kRowGap + kWellHeight + kRowGap + kBottomHeight;

    // ============================================================================
    // Helpers
    // ============================================================================

    /** launchAsync 会先把键盘焦点交给 DialogWindow，因此延后一拍取回内容焦点。 */
    void takeKeyboardFocus()
    {
        juce::Component::SafePointer<ShortcutCaptureDialogContent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->grabKeyboardFocus();
        });
    }

    /** 按当前字体与对话框宽度测量消息区，保证至少 4 行完整显示。 */
    void measureMessage()
    {
        juce::AttributedString attributed (message_);
        attributed.setFont (UIColors::getUIFont (static_cast<float> (kMessageFontHeight)));

        juce::TextLayout layout;
        layout.createLayout (attributed, static_cast<float> (kWidth - kMargin * 2));

        messageLines_ = juce::jmax (kMessageMinLines, layout.getNumLines());
        messageHeight_ = juce::jmax (kMessageMinLines * kMessageLineHeight,
                                     juce::roundToInt (layout.getHeight()));
    }

    static juce::String getHeldModifiersText()
    {
        const auto modifiers = juce::ModifierKeys::getCurrentModifiersRealtime();
        juce::StringArray parts;

        if (modifiers.isCtrlDown())  parts.add ("Ctrl");
        if (modifiers.isAltDown())   parts.add ("Alt");
        if (modifiers.isShiftDown()) parts.add ("Shift");
       #if JUCE_MAC
        if (modifiers.isCommandDown()) parts.add ("Cmd");
       #endif

        return parts.joinIntoString (" + ");
    }

    juce::String getCaptureHintText() const
    {
        if (! hasKeyboardFocus (true))
            return LOC (kShortcutCaptureClickHint);

        const auto modifiers = getHeldModifiersText();

        return modifiers.isEmpty() ? LOC (kShortcutCapturePressKey)
                                   : modifiers + juce::String::fromUTF8 (u8" + …");
    }

    void capture (const juce::KeyPress& key)
    {
        // 先移出回调并结束 modal，再在局部副本上调用，避免组件销毁后访问成员
        auto callback = std::move (onCaptured_);
        closeDialog();

        if (callback)
            callback (key);
    }

    void cancel()
    {
        auto callback = std::move (onCancelled_);
        closeDialog();

        if (callback)
            callback();
    }

    void closeDialog()
    {
        // 主动关闭标记：DialogWindow 隐藏时不再触发二次取消回调
        closing_ = true;

        // launchAsync 建立了 modal 状态（deleteWhenDismissed），
        // 用 exitModalState 结束，避免 DefaultDialogWindow::closeButtonPressed 仅隐藏窗口。
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState (0);
    }

    // ============================================================================
    // Parent / DialogWindow lifecycle
    // ============================================================================

    void watchParent (juce::Component* parent)
    {
        watchedParent_ = parent;

        if (watchedParent_ != nullptr)
            watchedParent_->addComponentListener (this);
    }

    void unwatchAll()
    {
        if (watchedParent_ != nullptr)
            watchedParent_->removeComponentListener (this);

        if (watchedDialog_ != nullptr)
            watchedDialog_->removeComponentListener (this);
    }

    void componentBeingDeleted (juce::Component& comp) override
    {
        if (watchedParent_ == nullptr || &comp != watchedParent_.getComponent())
            return;

        // 父组件正在销毁：不能调用 onCancelled（回调会访问已销毁的父对象），只关闭窗口
        onCaptured_ = {};
        onCancelled_ = {};
        closeDialog();
    }

    void componentVisibilityChanged (juce::Component& comp) override
    {
        if (closing_ || watchedDialog_ == nullptr || &comp != watchedDialog_.getComponent())
            return;

        // DialogWindow 被隐藏（Alt+F4 / 关闭按钮）而非主动关闭：走取消路径，清 currentEditingId_
        if (! comp.isShowing())
            cancel();
    }

    // ============================================================================
    // State
    // ============================================================================

    juce::String title_;
    juce::String message_;
    CaptureCallback onCaptured_;
    CancelCallback onCancelled_;
    juce::TextButton cancelButton_;

    // 父组件 / DialogWindow 生命周期监听（SafePointer，不保存裸指针跨异步）
    juce::Component::SafePointer<juce::Component> watchedParent_;
    juce::Component::SafePointer<juce::DialogWindow> watchedDialog_;
    bool closing_ = false;

    int messageLines_ = kMessageMinLines;
    int messageHeight_ = kMessageMinLines * kMessageLineHeight;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShortcutCaptureDialogContent)
};

} // namespace OpenTune

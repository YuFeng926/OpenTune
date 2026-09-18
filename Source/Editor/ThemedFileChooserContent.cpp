#include "ThemedFileChooserContent.h"

namespace OpenTune
{

namespace
{
    constexpr int kMargin = 20;
    constexpr int kTitleHeight = 26;
    constexpr int kTitleGap = 16;
    constexpr int kRowGap = 12;
    constexpr int kControlHeight = 32;
    constexpr int kButtonWidth = 92;
    constexpr int kButtonGap = 10;
    constexpr int kSaveLabelGap = 12;          // 标签文字与文件名输入框的间距
    constexpr int kMinSaveEditorWidth = 120;   // 极窄窗口下为文件名输入框保留的最小宽度
    constexpr int kMinStatusWidth = 80;        // 剩余宽度不足时不再显示状态文字
    constexpr int kIconSize = 18;
    constexpr int kRowHeight = 28;

    constexpr int kPreferredWidth = 800;
    constexpr int kPreferredHeight = 520;
    constexpr int kMinWidth = 560;
    constexpr int kMinHeight = 400;
    constexpr int kDisplayMargin = 48;

    // 主题文件夹图标（自绘，不使用系统图标）
    void drawFolderIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
    {
        const float tabHeight = area.getHeight() * 0.24f;
        const float tabWidth = area.getWidth() * 0.46f;

        juce::Path folder;
        folder.addRoundedRectangle (area.getX(), area.getY() + tabHeight * 0.55f,
                                    tabWidth, tabHeight * 1.5f, 1.5f);
        folder.addRoundedRectangle (area.getX(), area.getY() + tabHeight,
                                    area.getWidth(), area.getHeight() - tabHeight, 2.0f);

        g.setColour (colour);
        g.fillPath (folder);
    }

    // 主题文件图标（自绘折角纸张，不使用系统图标）
    void drawDocumentIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
    {
        const float fold = area.getWidth() * 0.32f;

        juce::Path page;
        page.startNewSubPath (area.getX(), area.getY());
        page.lineTo (area.getRight() - fold, area.getY());
        page.lineTo (area.getRight(), area.getY() + fold);
        page.lineTo (area.getRight(), area.getBottom());
        page.lineTo (area.getX(), area.getBottom());
        page.closeSubPath();

        g.setColour (colour.withAlpha (0.85f));
        g.fillPath (page);

        juce::Path corner;
        corner.startNewSubPath (area.getRight() - fold, area.getY());
        corner.lineTo (area.getRight(), area.getY() + fold);
        corner.lineTo (area.getRight() - fold, area.getY() + fold);
        corner.closeSubPath();

        g.setColour (colour.brighter (0.45f));
        g.fillPath (corner);

        g.setColour (UIColors::backgroundDark.withAlpha (0.6f));
        const float lineX = area.getX() + area.getWidth() * 0.22f;
        const float lineWidth = area.getWidth() * 0.56f;

        for (int i = 0; i < 3; ++i)
        {
            const float y = area.getY() + area.getHeight() * (0.46f + 0.16f * static_cast<float> (i));
            g.fillRect (lineX, y, lineWidth, 1.4f);
        }
    }

    // 起始目录：优先使用传入路径，路径不存在时向上回溯到最近的可用目录
    juce::File resolveStartDirectory (const juce::File& initial)
    {
        auto directory = initial;

        if (directory.getFullPathName().isEmpty())
            return juce::File::getSpecialLocation (juce::File::userMusicDirectory);

        if (! directory.isDirectory())
            directory = directory.getParentDirectory();

        while (! directory.isDirectory() && directory.getParentDirectory() != directory)
            directory = directory.getParentDirectory();

        if (! directory.isDirectory())
            return juce::File::getSpecialLocation (juce::File::userMusicDirectory);

        return directory;
    }
}

// ============================================================================
// Construction
// ============================================================================

ThemedFileChooserContent::ThemedFileChooserContent (Mode mode,
                                                    const juce::String& title,
                                                    const juce::File& initialFileOrDirectory,
                                                    const juce::String& wildcard,
                                                    ResultCallback onResult)
    : mode_ (mode),
      title_ (title),
      onResult_ (std::move (onResult))
{
    setWantsKeyboardFocus (true);

    // ---- 后台目录模型（仅数据扫描，不含 JUCE 文件浏览器 UI）----
    const auto pattern = wildcard.trim();
    if (pattern.isNotEmpty())
        filter_ = std::make_unique<juce::WildcardFileFilter> (pattern, "*", juce::String());

    scanThread_.startThread();
    list_ = std::make_unique<juce::DirectoryContentsList> (filter_.get(), scanThread_);
    list_->setIgnoresHiddenFiles (true);
    list_->addChangeListener (this);

    // ---- 路径行 ----
    upButton_.setButtonText (juce::String::fromUTF8 (u8"↑"));
    upButton_.setColour (juce::TextButton::buttonColourId, UIColors::backgroundLight);
    upButton_.setColour (juce::TextButton::buttonOnColourId, UIColors::backgroundLight);
    upButton_.setColour (juce::TextButton::textColourOffId, UIColors::textPrimary);
    upButton_.setColour (juce::TextButton::textColourOnId, UIColors::textPrimary);
    upButton_.onClick = [this] { navigateToParent(); };
    addAndMakeVisible (upButton_);

    refreshButton_.setButtonText (LOC (kFileChooserRefresh));
    refreshButton_.setColour (juce::TextButton::buttonColourId, UIColors::backgroundLight);
    refreshButton_.setColour (juce::TextButton::buttonOnColourId, UIColors::backgroundLight);
    refreshButton_.setColour (juce::TextButton::textColourOffId, UIColors::textPrimary);
    refreshButton_.setColour (juce::TextButton::textColourOnId, UIColors::textPrimary);
    refreshButton_.onClick = [this] { refreshList(); };
    addAndMakeVisible (refreshButton_);

    pathEditor_.setMultiLine (false, false);
    pathEditor_.setReturnKeyStartsNewLine (false);
    pathEditor_.setEscapeAndReturnKeysConsumed (true);
    pathEditor_.setFont (UIColors::getUIFont (14.0f));
    pathEditor_.setColour (juce::TextEditor::backgroundColourId, UIColors::backgroundDark);
    pathEditor_.setColour (juce::TextEditor::textColourId, UIColors::textPrimary);
    pathEditor_.setColour (juce::TextEditor::highlightColourId, UIColors::accent.withAlpha (0.45f));
    pathEditor_.setColour (juce::TextEditor::highlightedTextColourId, UIColors::textPrimary);
    pathEditor_.setColour (juce::TextEditor::outlineColourId, UIColors::panelBorder);
    pathEditor_.setColour (juce::TextEditor::focusedOutlineColourId, UIColors::accent);
    pathEditor_.onReturnKey = [this] { navigateToTypedPath(); };
    pathEditor_.onEscapeKey = [this] { cancel(); };
    addAndMakeVisible (pathEditor_);

    // ---- 目录/文件列表 ----
    listBox_.setModel (this);
    listBox_.setRowHeight (kRowHeight);
    listBox_.setMultipleSelectionEnabled (mode_ == Mode::OpenMultiple);
    listBox_.setColour (juce::ListBox::backgroundColourId, UIColors::backgroundMedium);
    listBox_.setColour (juce::ListBox::outlineColourId, UIColors::panelBorder);
    listBox_.setOutlineThickness (1);
    listBox_.addMouseListener (this, true);
    addAndMakeVisible (listBox_);

    // ---- 保存模式文件名 ----
    if (mode_ == Mode::Save)
    {
        saveNameEditor_.setMultiLine (false, false);
        saveNameEditor_.setReturnKeyStartsNewLine (false);
        saveNameEditor_.setEscapeAndReturnKeysConsumed (true);
        saveNameEditor_.setFont (UIColors::getUIFont (14.0f));
        saveNameEditor_.setColour (juce::TextEditor::backgroundColourId, UIColors::backgroundDark);
        saveNameEditor_.setColour (juce::TextEditor::textColourId, UIColors::textPrimary);
        saveNameEditor_.setColour (juce::TextEditor::highlightColourId, UIColors::accent.withAlpha (0.45f));
        saveNameEditor_.setColour (juce::TextEditor::highlightedTextColourId, UIColors::textPrimary);
        saveNameEditor_.setColour (juce::TextEditor::outlineColourId, UIColors::panelBorder);
        saveNameEditor_.setColour (juce::TextEditor::focusedOutlineColourId, UIColors::accent);
        saveNameEditor_.onTextChange = [this] { updateAcceptButton(); };
        saveNameEditor_.onReturnKey = [this] { accept(); };
        saveNameEditor_.onEscapeKey = [this] { cancel(); };
        addAndMakeVisible (saveNameEditor_);
    }

    // ---- 底部按钮 ----
    cancelButton_.setButtonText (LOC (kCancel));
    cancelButton_.setColour (juce::TextButton::buttonColourId, UIColors::backgroundLight);
    cancelButton_.setColour (juce::TextButton::buttonOnColourId, UIColors::backgroundLight);
    cancelButton_.setColour (juce::TextButton::textColourOffId, UIColors::textPrimary);
    cancelButton_.setColour (juce::TextButton::textColourOnId, UIColors::textPrimary);
    cancelButton_.onClick = [this] { cancel(); };
    addAndMakeVisible (cancelButton_);

    acceptButton_.setButtonText (mode_ == Mode::Save ? LOC (kFileChooserSave)
                                                     : LOC (kFileChooserOpen));
    acceptButton_.setColour (juce::TextButton::buttonOnColourId, UIColors::accent);
    acceptButton_.setColour (juce::TextButton::textColourOnId, UIColors::textPrimary);
    acceptButton_.onClick = [this] { accept(); };
    addAndMakeVisible (acceptButton_);

    // ---- 初始目录 / 初始文件名 ----
    const bool initialIsFile = initialFileOrDirectory.getFullPathName().isNotEmpty()
                                 && ! initialFileOrDirectory.isDirectory();
    const auto initialName = initialIsFile ? initialFileOrDirectory.getFileName() : juce::String();

    if (mode_ == Mode::Save)
        setSaveFileName (initialName);
    else
        pendingSelectName_ = initialName;

    setSize (kPreferredWidth, kPreferredHeight);
    setCurrentDirectory (resolveStartDirectory (initialFileOrDirectory));
}

ThemedFileChooserContent::~ThemedFileChooserContent()
{
    unwatchParent();

    list_->removeChangeListener (this);
    list_->clear();
}

// ============================================================================
// Async launch API
// ============================================================================

void ThemedFileChooserContent::launch (juce::Component* parent,
                                       Mode mode,
                                       const juce::String& title,
                                       const juce::File& initialFileOrDirectory,
                                       const juce::String& wildcard,
                                       ResultCallback onResult)
{
    auto* content = new ThemedFileChooserContent (mode, title, initialFileOrDirectory,
                                                  wildcard, std::move (onResult));
    content->fitToDisplay (parent);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content);
    options.dialogTitle = {};
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = false; // Esc 由内容组件处理（见 keyPressed）
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = parent;

    if (parent != nullptr)
        content->watchParent (parent);

    options.launchAsync();
}

void ThemedFileChooserContent::openFile (juce::Component* parent,
                                         const juce::String& title,
                                         const juce::File& initialFileOrDirectory,
                                         const juce::String& wildcard,
                                         ResultCallback onResult)
{
    launch (parent, Mode::OpenSingle, title, initialFileOrDirectory, wildcard, std::move (onResult));
}

void ThemedFileChooserContent::openFiles (juce::Component* parent,
                                          const juce::String& title,
                                          const juce::File& initialFileOrDirectory,
                                          const juce::String& wildcard,
                                          ResultCallback onResult)
{
    launch (parent, Mode::OpenMultiple, title, initialFileOrDirectory, wildcard, std::move (onResult));
}

void ThemedFileChooserContent::saveFile (juce::Component* parent,
                                         const juce::String& title,
                                         const juce::File& initialFileOrDirectory,
                                         const juce::String& wildcard,
                                         ResultCallback onResult)
{
    launch (parent, Mode::Save, title, initialFileOrDirectory, wildcard, std::move (onResult));
}

// ============================================================================
// Layout
// ============================================================================

ThemedFileChooserContent::Layout ThemedFileChooserContent::makeLayout() const
{
    Layout layout;

    auto bounds = getLocalBounds().reduced (kMargin);

    layout.title = bounds.removeFromTop (kTitleHeight);
    bounds.removeFromTop (kTitleGap);
    layout.pathRow = bounds.removeFromTop (kControlHeight);
    bounds.removeFromTop (kRowGap);

    if (mode_ == Mode::Save)
    {
        layout.saveRow = bounds.removeFromBottom (kControlHeight);
        bounds.removeFromBottom (kRowGap);
    }

    auto bottomRow = bounds.removeFromBottom (kControlHeight);
    bounds.removeFromBottom (kRowGap);
    layout.list = bounds;

    layout.buttons = bottomRow.removeFromRight (kButtonWidth * 2 + kButtonGap);

    // 状态文字只占用按钮之外的剩余空间；窗口过窄时整块隐藏，保证按钮和输入框可用
    layout.status = bottomRow.getWidth() >= kMinStatusWidth ? bottomRow
                                                            : juce::Rectangle<int>();

    return layout;
}

void ThemedFileChooserContent::fitToDisplay (juce::Component* parent)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    const auto* display = parent != nullptr
        ? displays.getDisplayForRect (parent->getScreenBounds())
        : displays.getPrimaryDisplay();

    const auto available = display != nullptr
        ? display->userArea
        : juce::Rectangle<int> (0, 0, kPreferredWidth, kPreferredHeight);

    // 先减掉屏幕边距得到真正可用的最大尺寸；屏幕比基本布局还小时，屏幕优先
    const int maxWidth = juce::jmax (1, available.getWidth() - kDisplayMargin);
    const int maxHeight = juce::jmax (1, available.getHeight() - kDisplayMargin);

    const int width = juce::jmax (juce::jmin (kPreferredWidth, maxWidth),
                                  juce::jmin (kMinWidth, maxWidth));
    const int height = juce::jmax (juce::jmin (kPreferredHeight, maxHeight),
                                   juce::jmin (kMinHeight, maxHeight));

    setSize (width, height);
}

void ThemedFileChooserContent::resized()
{
    const auto layout = makeLayout();

    auto pathRow = layout.pathRow;
    upButton_.setBounds (pathRow.removeFromLeft (pathRow.getHeight()));
    pathRow.removeFromLeft (kRowGap);

    // 刷新按钮按当前字体与译文取最佳宽度；窗口极窄时最多占半行，为路径输入框留空间
    const int bestRefreshWidth = refreshButton_.getBestWidthForHeight (kControlHeight);
    const int refreshWidth = juce::jmin (bestRefreshWidth, pathRow.getWidth() / 2);
    refreshButton_.setBounds (pathRow.removeFromRight (refreshWidth));
    pathRow.removeFromRight (kRowGap);
    pathEditor_.setBounds (pathRow);

    if (mode_ == Mode::Save)
    {
        auto saveRow = layout.saveRow;
        saveRow.removeFromLeft (getSaveLabelWidth (saveRow.getWidth()));
        saveNameEditor_.setBounds (saveRow);
    }

    listBox_.setBounds (layout.list);

    auto buttons = layout.buttons;
    acceptButton_.setBounds (buttons.removeFromRight (kButtonWidth));
    buttons.removeFromRight (kButtonGap);
    cancelButton_.setBounds (buttons.removeFromRight (kButtonWidth));
}

int ThemedFileChooserContent::getSaveLabelWidth (int rowWidth) const
{
    const auto font = UIColors::getUIFont (14.0f);
    const int textWidth = juce::roundToInt (juce::TextLayout::getStringWidth (font, LOC (kFileChooserFileName)));
    const int labelWidth = textWidth + kSaveLabelGap;

    // 窗口过窄时优先保住文件名输入框
    return juce::jmin (labelWidth, juce::jmax (0, rowWidth - kMinSaveEditorWidth));
}

void ThemedFileChooserContent::paint (juce::Graphics& g)
{
    g.fillAll (UIColors::backgroundDark);

    const auto layout = makeLayout();

    // 标题：窗口无 JUCE 标题栏，标题在内容里自绘
    g.setColour (UIColors::textPrimary);
    g.setFont (UIColors::getUIFont (16.0f));
    g.drawText (title_, layout.title, juce::Justification::centredLeft, true);

    g.setColour (UIColors::panelBorder.withAlpha (0.5f));
    g.drawHorizontalLine (layout.title.getBottom() + kTitleGap / 2,
                          static_cast<float> (kMargin),
                          static_cast<float> (getWidth() - kMargin));

    // 保存模式文件名标签
    if (mode_ == Mode::Save)
    {
        g.setColour (UIColors::textSecondary);
        g.setFont (UIColors::getUIFont (14.0f));
        g.drawText (LOC (kFileChooserFileName),
                    layout.saveRow.withWidth (getSaveLabelWidth (layout.saveRow.getWidth())),
                    juce::Justification::centredLeft, true);
    }

    // 状态：正在读取 / 空目录 / 选中数量
    g.setColour (errorText_.isNotEmpty() ? UIColors::statusError : UIColors::textSecondary);
    g.setFont (UIColors::getUIFont (12.0f));
    g.drawText (getStatusText(), layout.status, juce::Justification::centredLeft, true);
}

// ============================================================================
// Keyboard
// ============================================================================

bool ThemedFileChooserContent::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        cancel();
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        accept();
        return true;
    }

    return false;
}

void ThemedFileChooserContent::takeKeyboardFocus()
{
    if (! isShowing())
        return;

    // launchAsync 会在内容可见之后再把键盘焦点交给 DialogWindow，
    // 因此延后一拍把焦点取回内容（列表或保存文件名输入框）。
    juce::Component::SafePointer<ThemedFileChooserContent> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr)
            return;

        if (safeThis->mode_ == Mode::Save)
            safeThis->saveNameEditor_.grabKeyboardFocus();
        else
            safeThis->listBox_.grabKeyboardFocus();
    });
}

void ThemedFileChooserContent::visibilityChanged()
{
    takeKeyboardFocus();
}

void ThemedFileChooserContent::parentHierarchyChanged()
{
    if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
    {
        if (! dialog->isUsingNativeTitleBar())
        {
            // 此刻窗口尚未按内容尺寸展开，先记住期望尺寸：
            // setTitleBarHeight 会触发窗口 resized，把内容重排到当前窗口大小。
            const int preferredWidth = getWidth();
            const int preferredHeight = getHeight();

            // 移除 JUCE 标题栏（标题由内容自绘），并让窗口重新贴合内容尺寸
            dialog->setTitleBarHeight (0);
            dialog->setContentComponentSize (preferredWidth, preferredHeight);
        }
    }

    // 与 visibilityChanged 相同的延迟取焦，确保 launchAsync 后列表/文件名框拿到焦点
    takeKeyboardFocus();
}

// ============================================================================
// ListBoxModel
// ============================================================================

int ThemedFileChooserContent::getNumRows()
{
    return list_->getNumFiles();
}

void ThemedFileChooserContent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                                 int width, int height, bool rowIsSelected)
{
    juce::DirectoryContentsList::FileInfo info;
    if (! list_->getFileInfo (rowNumber, info))
        return;

    const auto rowBounds = juce::Rectangle<int> (0, 0, width, height);

    if (rowIsSelected)
    {
        g.setColour (UIColors::accent.withAlpha (0.30f));
        g.fillRect (rowBounds);
        g.setColour (UIColors::accent);
        g.fillRect (rowBounds.withWidth (2));
    }
    else if (rowNumber == hoveredRow_)
    {
        g.setColour (UIColors::backgroundLight.withAlpha (0.35f));
        g.fillRect (rowBounds);
    }

    auto content = rowBounds.reduced (10, 0);

    // 主题图标（文件夹 / 文件），不使用系统图标
    const auto iconArea = content.removeFromLeft (kIconSize).toFloat()
                                 .withSizeKeepingCentre (static_cast<float> (kIconSize),
                                                         static_cast<float> (kIconSize));
    if (info.isDirectory)
        drawFolderIcon (g, iconArea, UIColors::accent);
    else
        drawDocumentIcon (g, iconArea, UIColors::textSecondary);

    content.removeFromLeft (10);

    if (! info.isDirectory)
    {
        auto sizeArea = content.removeFromRight (90);
        g.setColour (UIColors::textSecondary.withAlpha (0.7f));
        g.setFont (UIColors::getUIFont (12.0f));
        g.drawText (juce::File::descriptionOfSizeInBytes (info.fileSize), sizeArea,
                    juce::Justification::centredRight, false);
    }

    g.setColour (UIColors::textPrimary);
    g.setFont (UIColors::getUIFont (14.0f));
    g.drawText (info.filename, content, juce::Justification::centredLeft, true);
}

void ThemedFileChooserContent::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (mode_ != Mode::Save)
        return;

    juce::DirectoryContentsList::FileInfo info;
    if (list_->getFileInfo (row, info) && ! info.isDirectory)
        setSaveFileName (info.filename);
}

void ThemedFileChooserContent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    activateRow (row);
}

void ThemedFileChooserContent::returnKeyPressed (int lastRowSelected)
{
    activateRow (lastRowSelected);
}

void ThemedFileChooserContent::selectedRowsChanged (int)
{
    if (restoringSelection_)
        return;

    updateSelectedPathsFromRows();
    updateStatusText();
    updateAcceptButton();
}

// ============================================================================
// ChangeListener / MouseListener
// ============================================================================

void ThemedFileChooserContent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source != list_.get())
        return;

    // 扫描更新可能重排行号：期间禁止 selectedRowsChanged 用旧行号覆盖稳定路径
    restoringSelection_ = true;
    listBox_.updateContent();
    restoringSelection_ = false;

    restoreSelectionFromPaths();

    selectPendingFile();
    updateStatusText();
    updateAcceptButton();
}

void ThemedFileChooserContent::mouseMove (const juce::MouseEvent& e)
{
    const auto position = e.getEventRelativeTo (&listBox_).getPosition();
    setHoveredRow (listBox_.getRowContainingPosition (position.x, position.y));
}

void ThemedFileChooserContent::mouseExit (const juce::MouseEvent& e)
{
    const auto position = e.getEventRelativeTo (&listBox_).getPosition();

    setHoveredRow (listBox_.getLocalBounds().contains (position)
                       ? listBox_.getRowContainingPosition (position.x, position.y)
                       : -1);
}

void ThemedFileChooserContent::setHoveredRow (int row)
{
    if (row == hoveredRow_)
        return;

    const int previous = hoveredRow_;
    hoveredRow_ = row;

    if (previous >= 0)
        listBox_.repaintRow (previous);

    if (hoveredRow_ >= 0)
        listBox_.repaintRow (hoveredRow_);
}

// ============================================================================
// Navigation
// ============================================================================

void ThemedFileChooserContent::setCurrentDirectory (const juce::File& directory)
{
    if (! directory.isDirectory())
        return;

    currentDirectory_ = directory;
    errorText_.clear();
    selectedFullPaths_.clear();

    list_->setDirectory (directory, true, true);
    listBox_.deselectAllRows();
    listBox_.updateContent();

    pathEditor_.setText (directory.getFullPathName(), juce::dontSendNotification);

    updateUpButton();
    updateStatusText();
    updateAcceptButton();
}

void ThemedFileChooserContent::navigateToParent()
{
    const auto parent = currentDirectory_.getParentDirectory();

    if (parent != currentDirectory_)
    {
        pendingSelectName_.clear();
        setCurrentDirectory (parent);
    }
}

void ThemedFileChooserContent::navigateToTypedPath()
{
    auto typed = pathEditor_.getText().trim().unquoted();

    if (typed.isEmpty())
    {
        pathEditor_.setText (currentDirectory_.getFullPathName(), juce::dontSendNotification);
        return;
    }

   #if JUCE_WINDOWS
    // 盘符路径规范化：单独的 "D:" 或 "D:foo" 会落到该盘的工作目录，统一补上根分隔符
    if (typed.length() >= 2 && typed[1] == ':' && juce::CharacterFunctions::isLetter (typed[0])
         && (typed.length() == 2 || (typed[2] != '\\' && typed[2] != '/')))
    {
        typed = typed.substring (0, 2) + "\\" + typed.substring (2);
    }
   #endif

    // 相对路径一律按当前目录解析，绝不落到进程工作目录
    const juce::File target = juce::File::isAbsolutePath (typed)
                                  ? juce::File (typed)
                                  : currentDirectory_.getChildFile (typed);

    if (target.isDirectory())
    {
        pendingSelectName_.clear();
        setCurrentDirectory (target);
        return;
    }

    const auto parent = target.getParentDirectory();
    if (parent.isDirectory())
    {
        if (mode_ == Mode::Save)
            setSaveFileName (target.getFileName());
        else
            pendingSelectName_ = target.getFileName();

        setCurrentDirectory (parent);
        selectPendingFile(); // 目录未变化时不会收到扫描回调，立即尝试一次
        return;
    }

    errorText_ = Loc::format (LOC (kFileChooserInvalidPath), typed);
    pathEditor_.setText (currentDirectory_.getFullPathName(), juce::dontSendNotification);
    updateStatusText();
}

void ThemedFileChooserContent::refreshList()
{
    pendingSelectName_.clear();
    errorText_.clear();
    selectedFullPaths_.clear();

    list_->refresh();
    listBox_.updateContent();
    updateStatusText();
    updateAcceptButton();
}

void ThemedFileChooserContent::setSaveFileName (const juce::String& name)
{
    saveNameEditor_.setText (name, juce::dontSendNotification);
    updateAcceptButton();
}

void ThemedFileChooserContent::activateRow (int row)
{
    juce::DirectoryContentsList::FileInfo info;
    if (! list_->getFileInfo (row, info))
        return;

    if (info.isDirectory)
    {
        pendingSelectName_.clear();
        setCurrentDirectory (currentDirectory_.getChildFile (info.filename));
        return;
    }

    if (mode_ == Mode::Save)
        setSaveFileName (info.filename);

    accept();
}

void ThemedFileChooserContent::selectPendingFile()
{
    if (pendingSelectName_.isEmpty())
        return;

    for (int i = 0; i < list_->getNumFiles(); ++i)
    {
        juce::DirectoryContentsList::FileInfo info;
        if (list_->getFileInfo (i, info) && ! info.isDirectory && info.filename == pendingSelectName_)
        {
            pendingSelectName_.clear();
            listBox_.selectRow (i);
            listBox_.scrollToEnsureRowIsOnscreen (i);
            return;
        }
    }

    if (! list_->isStillLoading())
        pendingSelectName_.clear();
}

// ============================================================================
// Stable selection（按 full path 恢复，避免异步扫描后行号漂移）
// ============================================================================

juce::String ThemedFileChooserContent::getRowFullPath (int row) const
{
    juce::DirectoryContentsList::FileInfo info;

    if (! list_->getFileInfo (row, info))
        return {};

    return currentDirectory_.getChildFile (info.filename).getFullPathName();
}

void ThemedFileChooserContent::updateSelectedPathsFromRows()
{
    selectedFullPaths_.clear();

    const auto rows = listBox_.getSelectedRows();

    for (int i = 0; i < rows.size(); ++i)
    {
        const auto path = getRowFullPath (rows[i]);

        if (path.isNotEmpty())
            selectedFullPaths_.add (path);
    }
}

void ThemedFileChooserContent::restoreSelectionFromPaths()
{
    if (selectedFullPaths_.isEmpty())
        return;

    // 当前行选择已与稳定路径一致时无需重建
    const auto rows = listBox_.getSelectedRows();
    juce::StringArray currentPaths;

    for (int i = 0; i < rows.size(); ++i)
        currentPaths.add (getRowFullPath (rows[i]));

    if (currentPaths == selectedFullPaths_)
        return;

    restoringSelection_ = true;
    listBox_.deselectAllRows();

    int firstRestored = -1;

    for (int i = 0; i < list_->getNumFiles(); ++i)
    {
        const auto path = getRowFullPath (i);

        if (path.isEmpty() || ! selectedFullPaths_.contains (path))
            continue;

        listBox_.selectRow (i, true, false);

        if (firstRestored < 0)
            firstRestored = i;
    }

    restoringSelection_ = false;

    if (firstRestored >= 0)
        listBox_.scrollToEnsureRowIsOnscreen (firstRestored);
}

// ============================================================================
// Status / buttons
// ============================================================================

void ThemedFileChooserContent::updateUpButton()
{
    upButton_.setEnabled (currentDirectory_.getParentDirectory() != currentDirectory_);
}

void ThemedFileChooserContent::updateAcceptButton()
{
    const bool valid = (mode_ == Mode::Save) ? isSaveTargetValid() : ! getSelectedFiles().isEmpty();

    acceptButton_.setEnabled (valid);
    acceptButton_.setColour (juce::TextButton::buttonColourId,
                             valid ? UIColors::accent : UIColors::buttonNormal);
    acceptButton_.setColour (juce::TextButton::textColourOffId,
                             valid ? UIColors::textPrimary : UIColors::textDisabled);
}

void ThemedFileChooserContent::updateStatusText()
{
    repaint (makeLayout().status);
}

juce::String ThemedFileChooserContent::getStatusText() const
{
    if (errorText_.isNotEmpty())
        return errorText_;

    if (list_->isStillLoading())
        return LOC (kFileChooserLoading);

    const int total = list_->getNumFiles();

    if (total == 0)
        return filter_ != nullptr ? LOC (kFileChooserNoMatches)
                                  : LOC (kFileChooserEmptyDirectory);

    const juce::String totalText (total);

    if (mode_ == Mode::Save)
        return Loc::format (LOC (kFileChooserItemCount), totalText);

    const int selected = listBox_.getNumSelectedRows();

    if (selected <= 0)
        return Loc::format (LOC (kFileChooserItemCount), totalText);

    return Loc::format (LOC (kFileChooserSelectionCount), totalText, juce::String (selected));
}

bool ThemedFileChooserContent::isSaveTargetValid() const
{
    const auto name = saveNameEditor_.getText().trim();

    if (name.isEmpty() || ! currentDirectory_.isDirectory())
        return false;

    return ! currentDirectory_.getChildFile (name).isDirectory();
}

juce::File ThemedFileChooserContent::getSaveTargetFile() const
{
    return currentDirectory_.getChildFile (saveNameEditor_.getText().trim());
}

juce::Array<juce::File> ThemedFileChooserContent::getSelectedFiles()
{
    restoreSelectionFromPaths();

    juce::Array<juce::File> files;
    const auto rows = listBox_.getSelectedRows();

    for (int i = 0; i < rows.size(); ++i)
    {
        juce::DirectoryContentsList::FileInfo info;
        if (list_->getFileInfo (rows[i], info) && ! info.isDirectory)
            files.add (currentDirectory_.getChildFile (info.filename));
    }

    return files;
}

// ============================================================================
// Accept / cancel
// ============================================================================

void ThemedFileChooserContent::accept()
{
    juce::Array<juce::File> result;

    if (mode_ == Mode::Save)
    {
        if (! isSaveTargetValid())
            return;

        result.add (getSaveTargetFile());
    }
    else
    {
        result = getSelectedFiles();

        if (result.isEmpty())
            return;
    }

    auto callback = std::move (onResult_);
    closeDialog();

    if (callback)
        callback (result);
}

void ThemedFileChooserContent::cancel()
{
    closeDialog();
}

void ThemedFileChooserContent::closeDialog()
{
    // launchAsync 建立了 modal 状态（deleteWhenDismissed），
    // 用 exitModalState 结束，避免 DefaultDialogWindow::closeButtonPressed 仅隐藏窗口。
    if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
        dialog->exitModalState (0);
}

// ============================================================================
// Parent lifecycle
// ============================================================================

void ThemedFileChooserContent::watchParent (juce::Component* parent)
{
    watchedParent_ = parent;

    if (watchedParent_ != nullptr)
        watchedParent_->addComponentListener (this);
}

void ThemedFileChooserContent::unwatchParent()
{
    if (watchedParent_ != nullptr)
        watchedParent_->removeComponentListener (this);
}

void ThemedFileChooserContent::componentBeingDeleted (juce::Component& comp)
{
    if (watchedParent_ == nullptr || &comp != watchedParent_.getComponent())
        return;

    // 父组件正在销毁：只关闭窗口，不调用结果回调
    onResult_ = {};
    closeDialog();
}

} // namespace OpenTune

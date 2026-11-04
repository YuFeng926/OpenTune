#include "PlatformFileDialogs.h"

#include "Editor/ConfirmDialogContent.h"
#include "Utils/AppLogger.h"
#include "Utils/IosDocumentPicker.h"
#include "Utils/LocalizationManager.h"

#include <memory>

namespace OpenTune::PlatformFileDialogs {

namespace {

#if ! JUCE_IOS
juce::File withExtension(const juce::File& file, const juce::String& extension)
{
    return file.hasFileExtension(extension) ? file : file.withFileExtension(extension);
}
#else
juce::String sanitiseBaseName(const juce::String& rawName, const juce::String& extension)
{
    auto name = rawName.trim();
    if (name.endsWithIgnoreCase(extension))
        name = name.dropLastCharacters(extension.length());
    return juce::File::createLegalFileName(name.trim());
}

void promptForFileName(const juce::String& title,
                       const juce::String& defaultBaseName,
                       std::function<void(const juce::String&)> onAccepted)
{
    auto* window = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("name", defaultBaseName, LOC_RAW("File name"));
    window->addButton(LOC(kOK), 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton(LOC(kCancel), 0, juce::KeyPress(juce::KeyPress::escapeKey));

    // ModalComponentManager 先执行回调再删除 deleteWhenDismissed 的组件，回调里读取输入框是安全的。
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([window, accept = std::move(onAccepted)](int result) {
            if (result == 1)
                accept(window->getTextEditorContents("name"));
        }),
        true);
}

juce::StringArray extensionsFromWildcard(const juce::String& wildcard)
{
    juce::StringArray tokens;
    tokens.addTokens(wildcard, ";,", "\"");

    juce::StringArray extensions;
    for (auto token : tokens) {
        token = token.trim().fromLastOccurrenceOf(".", false, false).toLowerCase();
        if (token.isNotEmpty() && token != "*")
            extensions.addIfNotAlreadyThere(token);
    }
    return extensions;
}
#endif

} // namespace

juce::File getAppDocumentsDirectory()
{
#if JUCE_IOS
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
#else
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("OpenTune");
#endif
}

void chooseSaveTarget(const juce::String& title,
                      const juce::File& desktopInitialFile,
                      const juce::String& extension,
                      const juce::String& mobileSubdirectory,
                      SaveTargetCallback onChosen)
{
#if JUCE_IOS
    const auto defaultBaseName = desktopInitialFile.getFileNameWithoutExtension().isNotEmpty()
        ? desktopInitialFile.getFileNameWithoutExtension()
        : juce::String("Untitled");

    promptForFileName(title, defaultBaseName,
        [extension, mobileSubdirectory, callback = std::move(onChosen)](const juce::String& rawName) {
            const auto baseName = sanitiseBaseName(rawName, extension);
            if (baseName.isEmpty())
                return;

            const auto directory = getAppDocumentsDirectory().getChildFile(mobileSubdirectory);
            if (!directory.createDirectory().wasOk()) {
                AppLogger::error("[PlatformFileDialogs] Cannot create " + directory.getFullPathName());
                return;
            }
            callback(directory.getChildFile(baseName + extension));
        });
#else
    juce::ignoreUnused(mobileSubdirectory);

    auto chooser = std::make_shared<juce::FileChooser>(title, desktopInitialFile, "*" + extension);
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync(flags, [chooser, extension, onChosen = std::move(onChosen)](const juce::FileChooser& fc) {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;
        onChosen(withExtension(file, extension));
    });
#endif
}

void chooseFilesToOpen(const juce::String& title,
                       const juce::File& desktopInitialLocation,
                       const juce::String& wildcardFilter,
                       bool allowMultiple,
                       bool copyExternalFiles,
                       juce::Component* messageParent,
                       OpenFilesCallback onChosen)
{
#if JUCE_IOS
    juce::ignoreUnused(title, desktopInitialLocation);

    const auto destination = copyExternalFiles
        ? getAppDocumentsDirectory().getChildFile("Imported")
        : juce::File();
    juce::Component::SafePointer<juce::Component> safeParent(messageParent);

    IosDocumentPicker::presentOpenPicker(extensionsFromWildcard(wildcardFilter), allowMultiple, destination,
        [safeParent, callback = std::move(onChosen)](const IosDocumentPicker::Result& result) {
            if (!result.failures.isEmpty() && safeParent != nullptr) {
                ConfirmDialogContent::showMessage(safeParent.getComponent(),
                                                  LOC(kImportFailed),
                                                  LOC_RAW("These files could not be copied into OpenTune:")
                                                      + "\n" + result.failures.joinIntoString("\n"));
            }
            if (!result.files.isEmpty())
                callback(result.files);
        });
#else
    juce::ignoreUnused(copyExternalFiles, messageParent);

    auto chooser = std::make_shared<juce::FileChooser>(title, desktopInitialLocation, wildcardFilter);
    auto flags = juce::FileBrowserComponent::openMode
               | juce::FileBrowserComponent::canSelectFiles;
    if (allowMultiple)
        flags |= juce::FileBrowserComponent::canSelectMultipleItems;

    chooser->launchAsync(flags, [chooser, onChosen = std::move(onChosen)](const juce::FileChooser& fc) {
        const auto files = fc.getResults();
        if (!files.isEmpty())
            onChosen(files);
    });
#endif
}

} // namespace OpenTune::PlatformFileDialogs

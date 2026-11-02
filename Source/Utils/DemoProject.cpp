#include "DemoProject.h"

#include "AppLogger.h"
#include "PlatformFileDialogs.h"

namespace OpenTune::DemoProject {

namespace {

juce::File bundledDirectory()
{
#if JUCE_MAC
    // macOS: Contents/Resources/Demo (executable is in Contents/MacOS/)
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto resourcesDemo = exe.getParentDirectory().getParentDirectory()
                                  .getChildFile("Resources").getChildFile("Demo");
    if (resourcesDemo.isDirectory())
        return resourcesDemo;
#endif

    // iOS (flat bundle) or Windows / Linux (alongside executable)
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getParentDirectory()
        .getChildFile("Demo");
}

juce::File installedProjectFile()
{
    return PlatformFileDialogs::getAppDocumentsDirectory()
        .getChildFile("Projects")
        .getChildFile("Demo.otproj");
}

/** 把失效的音频引用改为指向包内音频；返回是否有改动。 */
bool linkMedia(juce::XmlElement& root, const juce::File& projectDirectory, const juce::File& bundledAudio)
{
    auto* mediaPool = root.getChildByName("MediaPool");
    if (mediaPool == nullptr)
        return false;

    bool changed = false;
    for (auto* source : mediaPool->getChildWithTagNameIterator("Source"))
    {
        const auto relative = source->getStringAttribute("relativeMediaPath");
        if (relative.isNotEmpty() && projectDirectory.getChildFile(relative).existsAsFile())
            continue;

        source->setAttribute("relativeMediaPath", bundledAudio.getRelativePathFrom(projectDirectory));
        source->setAttribute("originalImportPath", bundledAudio.getFullPathName());
        changed = true;
    }
    return changed;
}

bool writeLinked(juce::XmlElement& root, const juce::File& projectFile, const juce::File& bundledAudio)
{
    if (!linkMedia(root, projectFile.getParentDirectory(), bundledAudio) && projectFile.existsAsFile())
        return true;

    if (!root.writeTo(projectFile))
    {
        AppLogger::error("[DemoProject] Cannot write " + projectFile.getFullPathName());
        return false;
    }
    return true;
}

} // namespace

juce::File install()
{
    const auto bundledProject = bundledDirectory().getChildFile("Demo.otproj");
    const auto bundledAudio = bundledDirectory().getChildFile("Demo.wav");
    if (!bundledProject.existsAsFile() || !bundledAudio.existsAsFile())
    {
        AppLogger::error("[DemoProject] Demo resources missing in " + bundledDirectory().getFullPathName());
        return {};
    }

    const auto projectFile = installedProjectFile();
    if (!projectFile.getParentDirectory().createDirectory().wasOk())
        return {};

    const auto sourceFile = projectFile.existsAsFile() ? projectFile : bundledProject;
    auto root = juce::XmlDocument::parse(sourceFile);
    if (root == nullptr || !writeLinked(*root, projectFile, bundledAudio))
        return {};

    return projectFile;
}

void repairIfInstalled()
{
    const auto projectFile = installedProjectFile();
    const auto bundledAudio = bundledDirectory().getChildFile("Demo.wav");
    if (!projectFile.existsAsFile() || !bundledAudio.existsAsFile())
        return;

    if (auto root = juce::XmlDocument::parse(projectFile))
        writeLinked(*root, projectFile, bundledAudio);
}

} // namespace OpenTune::DemoProject

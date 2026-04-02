#include "PresetManager.h"

namespace OpenTune {

PresetManager::PresetManager() {
    presetDirectory_ = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                           .getChildFile("OpenTune")
                           .getChildFile("Presets");

    if (!presetDirectory_.exists()) {
        presetDirectory_.createDirectory();
    }
}

PresetManager::~PresetManager() {
}

juce::ValueTree PresetData::toValueTree() const {
    juce::ValueTree tree("Preset");

    tree.setProperty("name", name, nullptr);
    tree.setProperty("author", author, nullptr);
    tree.setProperty("description", description, nullptr);
    tree.setProperty("timestamp", timestamp, nullptr);
    tree.setProperty("version", version, nullptr);

    tree.setProperty("retuneSpeed", retuneSpeed, nullptr);
    tree.setProperty("scaleRoot", scaleRoot, nullptr);
    tree.setProperty("isMinorScale", isMinorScale, nullptr);

    tree.setProperty("vibratoDepth", vibratoDepth, nullptr);
    tree.setProperty("vibratoRate", vibratoRate, nullptr);

    tree.setProperty("showWaveform", showWaveform, nullptr);
    tree.setProperty("showLanes", showLanes, nullptr);
    tree.setProperty("zoomLevel", zoomLevel, nullptr);
    tree.setProperty("bpm", bpm, nullptr);

    return tree;
}

PresetData PresetData::fromValueTree(const juce::ValueTree& tree) {
    PresetData preset;

    preset.name = tree.getProperty("name", "Untitled");
    preset.author = tree.getProperty("author", "");
    preset.description = tree.getProperty("description", "");
    preset.timestamp = tree.getProperty("timestamp", juce::Time::currentTimeMillis());
    preset.version = tree.getProperty("version", 1);

    preset.retuneSpeed = tree.getProperty("retuneSpeed", 15.0f);
    preset.scaleRoot = tree.getProperty("scaleRoot", 0);
    preset.isMinorScale = tree.getProperty("isMinorScale", false);

    preset.vibratoDepth = tree.getProperty("vibratoDepth", 0.0f);
    preset.vibratoRate = tree.getProperty("vibratoRate", 6.0f);

    preset.showWaveform = tree.getProperty("showWaveform", true);
    preset.showLanes = tree.getProperty("showLanes", true);
    preset.zoomLevel = tree.getProperty("zoomLevel", 1.0);
    preset.bpm = tree.getProperty("bpm", 120.0);

    return preset;
}

bool PresetManager::savePreset(const PresetData& preset, const juce::File& file) {
    juce::ValueTree tree = preset.toValueTree();

    std::unique_ptr<juce::XmlElement> xml(tree.createXml());
    if (xml == nullptr) {
        DBG("Failed to create XML from preset data");
        return false;
    }

    if (!xml->writeTo(file)) {
        DBG("Failed to write preset to file: " + file.getFullPathName());
        return false;
    }

    DBG("Saved preset to: " + file.getFullPathName());
    return true;
}

PresetData PresetManager::loadPreset(const juce::File& file) {
    std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
    if (xml == nullptr) {
        DBG("Failed to parse preset file: " + file.getFullPathName());
        return PresetData();
    }

    juce::ValueTree tree = juce::ValueTree::fromXml(*xml);
    if (!tree.isValid()) {
        DBG("Invalid preset data in file: " + file.getFullPathName());
        return PresetData();
    }

    DBG("Loaded preset from: " + file.getFullPathName());
    return PresetData::fromValueTree(tree);
}

std::vector<juce::File> PresetManager::getPresetFiles(const juce::File& directory) {
    std::vector<juce::File> presetFiles;

    if (!directory.exists() || !directory.isDirectory()) {
        return presetFiles;
    }

    juce::Array<juce::File> files = directory.findChildFiles(juce::File::findFiles, false, "*.otpreset");

    for (const auto& file : files) {
        presetFiles.push_back(file);
    }

    return presetFiles;
}

juce::File PresetManager::getDefaultPresetDirectory() {
    return presetDirectory_;
}

PresetData PresetManager::captureCurrentState(OpenTuneAudioProcessor& processor) {
    PresetData preset;

    preset.name = "Untitled Preset";
    preset.author = "";
    preset.description = "";
    preset.timestamp = juce::Time::currentTimeMillis();

    preset.zoomLevel = processor.getZoomLevel();
    preset.bpm = processor.getBpm();

    return preset;
}

void PresetManager::applyPreset(const PresetData& preset, OpenTuneAudioProcessor& processor) {
    processor.setZoomLevel(preset.zoomLevel);
    processor.setBpm(preset.bpm);

    DBG("Applied preset: " + preset.name);
}

} // namespace OpenTune

#include "ProjectPersistence.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <functional>

namespace OpenTune {

namespace {

std::vector<AutomationPoint> automationPointsFromValueTree(const juce::ValueTree& tree)
{
    std::vector<AutomationPoint> points;
    const int count = static_cast<int>(tree.getProperty("pointCount", 0));
    points.reserve(static_cast<size_t>(juce::jmax(0, count)));
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        const auto child = tree.getChild(i);
        if (child.hasType("Point")) {
            points.push_back({
                child.getProperty("time", 0.0),
                static_cast<float>(child.getProperty("gainDb", 0.0))
            });
        }
    }
    return points;
}

} // namespace

// ============================================================================
// 属性读写辅助
// ============================================================================

void ProjectPersistence::setOptionalProperty(juce::ValueTree& tree,
                                              const juce::Identifier& name,
                                              const juce::String& value)
{
    tree.setProperty(name, value, nullptr);
}

juce::String ProjectPersistence::getOptionalProperty(const juce::ValueTree& tree,
                                                       const juce::Identifier& name,
                                                       const juce::String& defaultValue)
{
    if (!tree.hasProperty(name)) { return defaultValue; }
    return tree.getProperty(name, defaultValue).toString();
}

void ProjectPersistence::setColourProperty(juce::ValueTree& tree,
                                            const juce::Identifier& name,
                                            const juce::Colour& colour)
{
    tree.setProperty(name, colour.toString(), nullptr);
}

juce::Colour ProjectPersistence::getColourProperty(const juce::ValueTree& tree,
                                                     const juce::Identifier& name,
                                                     const juce::Colour& defaultColour)
{
    if (!tree.hasProperty(name)) { return defaultColour; }
    return juce::Colour::fromString(tree.getProperty(name).toString());
}

// ============================================================================
// 顶层 API
// ============================================================================

juce::ValueTree ProjectPersistence::toValueTree(const ProjectSnapshot& snapshot) const
{
    juce::ValueTree root(kRootNodeName);

    root.setProperty(kProjectFormatVersionAttr, kCurrentProjectFormatVersion, nullptr);
    setOptionalProperty(root, juce::Identifier(kAppVersionAttr), snapshot.header.appVersion);
    setOptionalProperty(root, "projectName", snapshot.header.projectName);
    setOptionalProperty(root, "projectId", snapshot.header.projectId);
    setOptionalProperty(root, "createdAt", snapshot.header.createdAt);
    setOptionalProperty(root, "lastSavedAt", snapshot.header.lastSavedAt);

    root.addChild(settingsToValueTree(snapshot.settings), -1, nullptr);

    if (!snapshot.sources.empty()) {
        juce::ValueTree mediaPool("MediaPool");
        for (const auto& src : snapshot.sources) {
            mediaPool.addChild(sourceToValueTree(src), -1, nullptr);
        }
        root.addChild(mediaPool, -1, nullptr);
    }

    if (!snapshot.contents.empty()) {
        juce::ValueTree contentList("Contents");
        for (const auto& mat : snapshot.contents) {
            contentList.addChild(contentToValueTree(mat), -1, nullptr);
        }
        root.addChild(contentList, -1, nullptr);
    }

    if (!snapshot.tracks.empty()) {
        juce::ValueTree tracksNode("Tracks");
        for (const auto& track : snapshot.tracks) {
            tracksNode.addChild(trackToValueTree(track), -1, nullptr);
        }
        root.addChild(tracksNode, -1, nullptr);
    }

    for (const auto& binding : snapshot.referenceBindings) {
        root.addChild(referenceBindingToValueTree(binding), -1, nullptr);
    }

    return root;
}

Result<ProjectSnapshot> ProjectPersistence::fromValueTree(const juce::ValueTree& tree) const
{
    if (!tree.hasType(kRootNodeName)) {
        return Result<ProjectSnapshot>::failure(
            Error::fromCode(ErrorCode::InvalidParameter,
                ("Root node is not " + juce::String(kRootNodeName)).toStdString()));
    }

    const auto version = static_cast<int>(tree.getProperty(kProjectFormatVersionAttr, 0));
    if (version < kMinimumProjectFormatVersion || version > kCurrentProjectFormatVersion) {
        const auto msg = "Unsupported project format version: " + juce::String(version)
            + " (supported " + juce::String(kMinimumProjectFormatVersion)
            + "-" + juce::String(kCurrentProjectFormatVersion) + ")";
        return Result<ProjectSnapshot>::failure(Error::fromCode(ErrorCode::InvalidParameter, msg.toStdString()));
    }

    if (version >= 6)
    {
        std::function<bool(const juce::ValueTree&)> validateDynamicEq;
        validateDynamicEq = [&](const juce::ValueTree& node) -> bool {
            if (node.hasType("EqSettings"))
            {
                if (!node.hasProperty("active")
                    || node.getNumChildren() <= 0
                    || node.getNumChildren() > EqSettings::kMaxFilters)
                    return false;

                EqSettings eq;
                eq.active = static_cast<int>(node.getProperty("active")) != 0;
                eq.filters.clear();
                for (int i = 0; i < node.getNumChildren(); ++i)
                {
                    const auto filterTree = node.getChild(i);
                    if (!filterTree.hasType("Filter")
                        || !filterTree.hasProperty("type")
                        || !filterTree.hasProperty("frequencyHz")
                        || !filterTree.hasProperty("gainDb")
                        || !filterTree.hasProperty("q"))
                        return false;

                    const int type = static_cast<int>(filterTree.getProperty("type"));
                    if (type < 0 || type > static_cast<int>(EqFilterType::HighCut))
                        return false;
                    // v7: paletteSlot 属性；v6 旧数据按索引确定性补 slot
                    const int slot = filterTree.hasProperty("slot")
                        ? static_cast<int>(filterTree.getProperty("slot", i))
                        : i;
                    if (slot < 0 || slot >= EqSettings::kMaxFilters)
                        return false;
                    eq.filters.push_back({
                        static_cast<EqFilterType>(type),
                        static_cast<float>(filterTree.getProperty("frequencyHz")),
                        static_cast<float>(filterTree.getProperty("gainDb")),
                        static_cast<float>(filterTree.getProperty("q")),
                        static_cast<int>(filterTree.getProperty("slot", i)),
                        static_cast<int>(filterTree.getProperty("bypassed", 0)) != 0
                    });
                }
                return eq.isValid();
            }

            for (int i = 0; i < node.getNumChildren(); ++i)
                if (!validateDynamicEq(node.getChild(i)))
                    return false;
            return true;
        };

        if (!validateDynamicEq(tree))
            return Result<ProjectSnapshot>::failure(
                Error::fromCode(ErrorCode::InvalidParameter,
                    "Project contains invalid dynamic EqSettings"));
    }

    ProjectSnapshot snapshot;

    // Header
    snapshot.header.projectFormatVersion = kCurrentProjectFormatVersion;
    snapshot.header.appVersion = getOptionalProperty(tree, juce::Identifier(kAppVersionAttr), "");
    snapshot.header.projectName = getOptionalProperty(tree, "projectName", "Untitled");
    snapshot.header.projectId = getOptionalProperty(tree, "projectId", "");
    snapshot.header.createdAt = getOptionalProperty(tree, "createdAt", "");
    snapshot.header.lastSavedAt = getOptionalProperty(tree, "lastSavedAt", "");

    // ProjectSettings — current format requires the node and mandatory fields
    auto settingsTree = tree.getChildWithName("ProjectSettings");
    if (!settingsTree.isValid()) {
        return Result<ProjectSnapshot>::failure(
            Error::fromCode(ErrorCode::InvalidParameter,
                "Project is missing required ProjectSettings node"));
    }
    if (!settingsTree.hasProperty("timeSignatureNumerator") || !settingsTree.hasProperty("timeSignatureDenominator")) {
        return Result<ProjectSnapshot>::failure(
            Error::fromCode(ErrorCode::InvalidParameter,
                "ProjectSettings is missing required timeSignatureNumerator or timeSignatureDenominator"));
    }
    snapshot.settings = settingsFromValueTree(settingsTree);

    // MediaPool → Sources
    auto mediaPool = tree.getChildWithName("MediaPool");
    if (mediaPool.isValid()) {
        for (int i = 0; i < mediaPool.getNumChildren(); ++i) {
            auto child = mediaPool.getChild(i);
            if (child.hasType("Source")) {
                snapshot.sources.push_back(sourceFromValueTree(child));
            }
        }
    }

    // Contents
    auto contentList = tree.getChildWithName("Contents");
    if (contentList.isValid()) {
        for (int i = 0; i < contentList.getNumChildren(); ++i) {
            auto child = contentList.getChild(i);
            if (child.hasType("Content")) {
                snapshot.contents.push_back(contentFromValueTree(child, version));
            }
        }
    }

    // Tracks
    auto tracksNode = tree.getChildWithName("Tracks");
    if (tracksNode.isValid()) {
        for (int i = 0; i < tracksNode.getNumChildren(); ++i) {
            auto child = tracksNode.getChild(i);
            if (child.hasType("Track")) {
                snapshot.tracks.push_back(trackFromValueTree(child));
            }
        }
    }

    // ReferenceBindings
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (child.hasType("ReferenceBinding")) {
            snapshot.referenceBindings.push_back(referenceBindingFromValueTree(child));
        }
    }

    return Result<ProjectSnapshot>::success(snapshot);
}

bool ProjectPersistence::writeProjectFile(const ProjectSnapshot& snapshot, const juce::File& file) const
{
    const auto vt = toValueTree(snapshot);
    auto xml = vt.createXml();
    if (!xml) { return false; }
    return xml->writeTo(file, {});
}

Result<ProjectSnapshot> ProjectPersistence::readProjectFile(const juce::File& file) const
{
    if (!file.existsAsFile()) {
        return Result<ProjectSnapshot>::failure(
            Error::fromCode(ErrorCode::UnknownError,
                ("Project file not found: " + file.getFullPathName()).toStdString()));
    }

    juce::XmlDocument doc(file);
    auto xml = doc.getDocumentElement();
    if (!xml) {
        return Result<ProjectSnapshot>::failure(
            Error::fromCode(ErrorCode::UnknownError,
                ("Failed to parse project file: " + file.getFullPathName()
                 + " — " + doc.getLastParseError()).toStdString()));
    }

    const auto vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid()) {
        return Result<ProjectSnapshot>::failure(
            Error::fromCode(ErrorCode::UnknownError,
                ("Invalid XML structure in: " + file.getFullPathName()).toStdString()));
    }

    return fromValueTree(vt);
}

// ============================================================================
// 序列化辅助 — Settings
// ============================================================================

juce::ValueTree ProjectPersistence::settingsToValueTree(const ProjectSettings& settings)
{
    juce::ValueTree tree("ProjectSettings");
    tree.setProperty("bpm", settings.bpm, nullptr);
    tree.setProperty("sampleRate", settings.sampleRate, nullptr);
    tree.setProperty("timeSignatureNumerator", settings.timeSignatureNumerator, nullptr);
    tree.setProperty("timeSignatureDenominator", settings.timeSignatureDenominator, nullptr);
    tree.setProperty("selectedTrackId", settings.selectedTrackId, nullptr);
    tree.setProperty("selectedPlacementId", static_cast<int64_t>(settings.selectedPlacementId), nullptr);
    tree.setProperty("activeTrackId", settings.activeTrackId, nullptr);
    tree.setProperty("scrollX", settings.scrollX, nullptr);
    tree.setProperty("scrollY", settings.scrollY, nullptr);
    tree.setProperty("zoomLevel", settings.zoomLevel, nullptr);
    tree.setProperty("verticalZoom", settings.verticalZoom, nullptr);
    tree.setProperty("timelineOriginSeconds", settings.timelineOriginSeconds, nullptr);
    return tree;
}

ProjectSettings ProjectPersistence::settingsFromValueTree(const juce::ValueTree& tree)
{
    ProjectSettings s;
    s.bpm = tree.getProperty("bpm", 120.0);
    s.sampleRate = tree.getProperty("sampleRate", 44100.0);
    s.timeSignatureNumerator = static_cast<int>(tree.getProperty("timeSignatureNumerator", 4));
    s.timeSignatureDenominator = static_cast<int>(tree.getProperty("timeSignatureDenominator", 4));
    s.selectedTrackId = static_cast<int>(tree.getProperty("selectedTrackId", 0));
    s.selectedPlacementId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("selectedPlacementId", 0)));
    s.activeTrackId = static_cast<int>(tree.getProperty("activeTrackId", 0));
    s.scrollX = tree.getProperty("scrollX", 0.0);
    s.scrollY = tree.getProperty("scrollY", 0.0);
    s.zoomLevel = tree.getProperty("zoomLevel", 1.0);
    s.verticalZoom = tree.getProperty("verticalZoom", 1.0);
    s.timelineOriginSeconds = tree.getProperty("timelineOriginSeconds", 0.0);
    return s;
}

// ============================================================================
// 序列化辅助 — Source
// ============================================================================

juce::ValueTree ProjectPersistence::sourceToValueTree(const ProjectSourceEntry& source)
{
    juce::ValueTree tree("Source");
    tree.setProperty("sourceId", static_cast<int64_t>(source.sourceId), nullptr);
    setOptionalProperty(tree, "displayName", source.displayName);
    setOptionalProperty(tree, "originalImportPath", source.originalImportPath);
    setOptionalProperty(tree, "relativeMediaPath", source.relativeMediaPath);
    tree.setProperty("sampleRate", source.sampleRate, nullptr);
    tree.setProperty("numChannels", source.numChannels, nullptr);
    tree.setProperty("lengthSamples", static_cast<int64_t>(source.lengthSamples), nullptr);
    tree.setProperty("lengthSeconds", source.lengthSeconds, nullptr);
    setOptionalProperty(tree, "contentHash", source.contentHash);
    tree.setProperty("fileSizeBytes", static_cast<int64_t>(source.fileSizeBytes), nullptr);
    return tree;
}

ProjectSourceEntry ProjectPersistence::sourceFromValueTree(const juce::ValueTree& tree)
{
    ProjectSourceEntry s;
    s.sourceId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("sourceId", 0)));
    s.displayName = getOptionalProperty(tree, "displayName", "");
    s.originalImportPath = getOptionalProperty(tree, "originalImportPath", "");
    s.relativeMediaPath = getOptionalProperty(tree, "relativeMediaPath", "");
    s.sampleRate = tree.getProperty("sampleRate", 0.0);
    s.numChannels = static_cast<int>(tree.getProperty("numChannels", 0));
    s.lengthSamples = static_cast<int64_t>(tree.getProperty("lengthSamples", 0));
    s.lengthSeconds = tree.getProperty("lengthSeconds", 0.0);
    s.contentHash = getOptionalProperty(tree, "contentHash", "");
    s.fileSizeBytes = static_cast<int64_t>(tree.getProperty("fileSizeBytes", 0));
    return s;
}

// ============================================================================
// 序列化辅助 — Content
// ============================================================================

juce::ValueTree ProjectPersistence::contentToValueTree(const ProjectContentEntry& mat)
{
    juce::ValueTree tree("Content");
    tree.setProperty("contentDomain", static_cast<int>(mat.contentKey.domainKind), nullptr);
    tree.setProperty("contentObjectId", static_cast<int64_t>(mat.contentKey.objectId), nullptr);
    tree.setProperty("contentDiscriminator", static_cast<int64_t>(mat.contentKey.sourceWindowDiscriminator), nullptr);
    tree.setProperty("sourceId", static_cast<int64_t>(mat.sourceId), nullptr);
    tree.setProperty("retired", mat.retired ? 1 : 0, nullptr);
    tree.setProperty("renderRevision", static_cast<int64_t>(mat.renderRevision), nullptr);
    tree.setProperty("lineageParentDomain", static_cast<int>(mat.lineageParentContentKey.domainKind), nullptr);
    tree.setProperty("lineageParentObjectId", static_cast<int64_t>(mat.lineageParentContentKey.objectId), nullptr);
    tree.setProperty("lineageParentDiscriminator", static_cast<int64_t>(mat.lineageParentContentKey.sourceWindowDiscriminator), nullptr);

    // SourceWindow
    juce::ValueTree swTree("SourceWindow");
    swTree.setProperty("sourceId", static_cast<int64_t>(mat.sourceWindow.sourceId), nullptr);
    swTree.setProperty("sourceStartSeconds", mat.sourceWindow.sourceStartSeconds, nullptr);
    swTree.setProperty("sourceEndSeconds", mat.sourceWindow.sourceEndSeconds, nullptr);
    tree.addChild(swTree, -1, nullptr);

    // DetectedKey
    juce::ValueTree dkTree("DetectedKey");
    dkTree.setProperty("tonic", static_cast<int>(mat.detectedKey.root), nullptr);
    dkTree.setProperty("scale", static_cast<int>(mat.detectedKey.scale), nullptr);
    dkTree.setProperty("confidence", mat.detectedKey.confidence, nullptr);
    dkTree.setProperty("origin", static_cast<int>(mat.detectedKey.origin), nullptr);
    tree.addChild(dkTree, -1, nullptr);

    // Notes
    if (!mat.notes.empty()) {
        tree.addChild(notesToValueTree(mat.notes, "Notes"), -1, nullptr);
    }
    tree.setProperty("noteTopologyInitialized", mat.noteTopologyInitialized ? 1 : 0, nullptr);

    // Volume envelope
    if (!mat.volumeEnvelope.empty()) {
        tree.addChild(volumeEnvelopeToValueTree(mat.volumeEnvelope), -1, nullptr);
    }

    // CorrectedSegments
    if (!mat.correctionSegments.empty()) {
        tree.addChild(segmentsToValueTree(mat.correctionSegments), -1, nullptr);
    }

    // TimeGrid
    tree.addChild(timeGridToValueTree(mat.timeGrid), -1, nullptr);

    // PitchShiftSettings
    tree.setProperty("pitchShiftSemitones", mat.pitchShiftSettings.semitone, nullptr);
    tree.setProperty("pitchShiftCents", mat.pitchShiftSettings.cents, nullptr);

    // OriginalF0State
    tree.setProperty("originalF0State", static_cast<int>(mat.originalF0State), nullptr);

    // SilentGaps
    if (!mat.silentGaps.empty()) {
        tree.addChild(silentGapsToValueTree(mat.silentGaps), -1, nullptr);
    }

    // ReferenceFeatures
    tree.addChild(referenceFeaturesToValueTree(mat.referenceFeatures), -1, nullptr);

    return tree;
}

ProjectContentEntry ProjectPersistence::contentFromValueTree(const juce::ValueTree& tree, int formatVersion)
{
    ProjectContentEntry m;
    m.contentKey.domainKind = static_cast<DomainKind>(static_cast<int>(tree.getProperty("contentDomain", 0)));
    m.contentKey.objectId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("contentObjectId", 0)));
    m.contentKey.sourceWindowDiscriminator = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("contentDiscriminator", 0)));
    m.sourceId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("sourceId", 0)));
    m.retired = static_cast<int>(tree.getProperty("retired", 0)) != 0;
    m.renderRevision = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("renderRevision", 0)));
    m.lineageParentContentKey.domainKind = static_cast<DomainKind>(static_cast<int>(tree.getProperty("lineageParentDomain", 0)));
    m.lineageParentContentKey.objectId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("lineageParentObjectId", 0)));
    m.lineageParentContentKey.sourceWindowDiscriminator = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("lineageParentDiscriminator", 0)));

    // SourceWindow
    auto swTree = tree.getChildWithName("SourceWindow");
    if (swTree.isValid()) {
        m.sourceWindow.sourceId = static_cast<uint64_t>(static_cast<int64_t>(swTree.getProperty("sourceId", 0)));
        m.sourceWindow.sourceStartSeconds = swTree.getProperty("sourceStartSeconds", 0.0);
        m.sourceWindow.sourceEndSeconds = swTree.getProperty("sourceEndSeconds", 0.0);
    }

    // DetectedKey
    auto dkTree = tree.getChildWithName("DetectedKey");
    if (dkTree.isValid()) {
        m.detectedKey.root = static_cast<Key>(static_cast<int>(dkTree.getProperty("tonic", 0)));
        m.detectedKey.scale = static_cast<Scale>(static_cast<int>(dkTree.getProperty("scale", 0)));
        m.detectedKey.confidence = dkTree.getProperty("confidence", 0.0f);
        // 旧数据无 origin 属性：按 confidence 迁移
        m.detectedKey.origin = dkTree.hasProperty("origin")
            ? static_cast<Origin>(static_cast<int>(dkTree.getProperty("origin", 0)))
            : DetectedKey::originFromLegacyConfidence(m.detectedKey.confidence);
    }

    // Notes
    m.notes = notesFromValueTree(tree.getChildWithName("Notes"), formatVersion);

    // Note topology state（旧工程无该 property 时按 notes 是否为空推断，避免覆盖已有音符）
    m.noteTopologyInitialized = tree.hasProperty("noteTopologyInitialized")
        ? static_cast<int>(tree.getProperty("noteTopologyInitialized", 0)) != 0
        : !m.notes.empty();

    const auto volumeEnvelopeTree = tree.getChildWithName("VolumeEnvelope");
    if (volumeEnvelopeTree.isValid()) {
        m.volumeEnvelope = volumeEnvelopeFromValueTree(volumeEnvelopeTree);
    } else {
        const auto noteGainLane = AutomationLane::fromLegacyNoteGains(m.notes);
        const auto sibilantTree = tree.getChildWithName("SibilantGainEnvelope");
        m.volumeEnvelope = sibilantTree.isValid()
            ? AutomationLane::sum(noteGainLane, AutomationLane::fromLegacyStepPoints(
                automationPointsFromValueTree(sibilantTree)))
            : noteGainLane;
    }

    // CorrectedSegments
    m.correctionSegments = segmentsFromValueTree(tree.getChildWithName("CorrectedSegments"));

    // TimeGrid
    auto tgTree = tree.getChildWithName("TimeGrid");
    if (tgTree.isValid()) {
        m.timeGrid = timeGridFromValueTree(tgTree);
    }

    // PitchShiftSettings
    m.pitchShiftSettings.semitone = static_cast<int>(tree.getProperty("pitchShiftSemitones", 0));
    m.pitchShiftSettings.cents = static_cast<int>(tree.getProperty("pitchShiftCents", 0));

    // OriginalF0State
    m.originalF0State = static_cast<uint8_t>(static_cast<int>(tree.getProperty("originalF0State", 0)));

    // SilentGaps
    m.silentGaps = silentGapsFromValueTree(tree.getChildWithName("SilentGaps"));

    // ReferenceFeatures
    auto rfTree = tree.getChildWithName("ReferenceFeatures");
    if (rfTree.isValid()) {
        m.referenceFeatures = referenceFeaturesFromValueTree(rfTree, formatVersion);
    }

    return m;
}

// ============================================================================
// Notes 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::notesToValueTree(const std::vector<Note>& notes, const juce::String& nodeName)
{
    juce::ValueTree tree(nodeName);
    for (const auto& note : notes) {
        juce::ValueTree nt("Note");
        nt.setProperty("startTime", note.startTime, nullptr);
        nt.setProperty("endTime", note.endTime, nullptr);
        nt.setProperty("pitch", note.pitch, nullptr);
        nt.setProperty("originalPitch", note.originalPitch, nullptr);
        nt.setProperty("pitchOffset", note.pitchOffset, nullptr);
        nt.setProperty("retuneSpeed", note.retuneSpeed, nullptr);
        nt.setProperty("pitchDriftScale", note.pitchDriftScale, nullptr);
        nt.setProperty("vibratoDepth", note.vibratoDepth, nullptr);
        nt.setProperty("vibratoRate", note.vibratoRate, nullptr);
        nt.setProperty("outputGainDb", note.outputGainDb, nullptr);
        nt.setProperty("isVoiced", note.isVoiced ? 1 : 0, nullptr);
        
        // v6: Per-note EQ settings — active + Filter 子节点列表
        if (note.eq.has_value()) {
            const auto& eq = *note.eq;
            juce::ValueTree eqTree("EqSettings");
            eqTree.setProperty("active", eq.active ? 1 : 0, nullptr);
            for (const auto& f : eq.filters) {
                juce::ValueTree fTree("Filter");
                fTree.setProperty("type", static_cast<int>(f.type), nullptr);
                fTree.setProperty("frequencyHz", f.frequencyHz, nullptr);
                fTree.setProperty("gainDb", f.gainDb, nullptr);
                fTree.setProperty("q", f.q, nullptr);
                fTree.setProperty("slot", f.paletteSlot, nullptr);
                if (f.bypassed)
                    fTree.setProperty("bypassed", 1, nullptr);
                eqTree.addChild(fTree, -1, nullptr);
            }
            nt.addChild(eqTree, -1, nullptr);
        }
        
        tree.addChild(nt, -1, nullptr);
    }
    return tree;
}

std::vector<Note> ProjectPersistence::notesFromValueTree(const juce::ValueTree& tree, int formatVersion)
{
    std::vector<Note> notes;
    if (!tree.isValid()) { return notes; }
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("Note")) { continue; }
        Note note;
        note.startTime = child.getProperty("startTime", 0.0);
        note.endTime = child.getProperty("endTime", 0.0);
        note.pitch = child.getProperty("pitch", 0.0f);
        note.originalPitch = child.getProperty("originalPitch", 0.0f);
        note.pitchOffset = child.getProperty("pitchOffset", 0.0f);
        note.retuneSpeed = child.getProperty("retuneSpeed", -1.0f);
        note.pitchDriftScale = child.getProperty("pitchDriftScale", 1.0f);
        note.vibratoDepth = child.getProperty("vibratoDepth", -1.0f);
        note.vibratoRate = child.getProperty("vibratoRate", -1.0f);
        note.outputGainDb = child.getProperty("outputGainDb", 0.0f);
        note.isVoiced = static_cast<int>(child.getProperty("isVoiced", 1)) != 0;
        
        // v5: 旧 scalar 9 字段 → 迁移为 5 个固定过滤器
        // v6: Filter 子节点列表
        auto eqTree = child.getChildWithName("EqSettings");
        if (formatVersion >= 5
            && eqTree.isValid()
            && eqTree.hasProperty("active"))
        {
            EqSettings eq;
            eq.active = static_cast<int>(eqTree.getProperty("active", 0)) != 0;
            bool parsedEq = false;

            if (formatVersion >= 6) {
                // v6: 当前格式必须完整合法，不能静默截断或跳过损坏的 Filter。
                eq.filters.clear();
                const int numChildren = eqTree.getNumChildren();
                parsedEq = numChildren > 0 && numChildren <= EqSettings::kMaxFilters;
                for (int fi = 0; parsedEq && fi < numChildren; ++fi) {
                    auto fTree = eqTree.getChild(fi);
                    if (!fTree.hasType("Filter")) { parsedEq = false; break; }
                    if (!fTree.hasProperty("type")
                        || !fTree.hasProperty("frequencyHz")
                        || !fTree.hasProperty("gainDb")
                        || !fTree.hasProperty("q")) {
                        parsedEq = false;
                        break;
                    }
                    const int typeInt = static_cast<int>(fTree.getProperty("type", 0));
                    if (typeInt < 0 || typeInt > static_cast<int>(EqFilterType::HighCut)) {
                        parsedEq = false;
                        break;
                    }
                    EqFilter f;
                    f.type = static_cast<EqFilterType>(typeInt);
                    f.frequencyHz = fTree.getProperty("frequencyHz", 1000.0f);
                    f.gainDb = fTree.getProperty("gainDb", 0.0f);
                    f.q = fTree.getProperty("q", 2.0f);
                    // v7: paletteSlot 属性；v6 旧数据按索引确定性补 slot
                    f.paletteSlot = fTree.hasProperty("slot")
                        ? static_cast<int>(fTree.getProperty("slot", fi))
                        : fi;
                    eq.filters.push_back(f);
                }
            } else if (eqTree.hasProperty("lowCutFrequencyHz")
                    && eqTree.hasProperty("lowShelfFrequencyHz")
                    && eqTree.hasProperty("lowShelfGainDb")
                    && eqTree.hasProperty("peakFrequencyHz")
                    && eqTree.hasProperty("peakGainDb")
                    && eqTree.hasProperty("highShelfFrequencyHz")
                    && eqTree.hasProperty("highShelfGainDb")
                    && eqTree.hasProperty("highCutFrequencyHz")) {
                // v5 legacy: scalar 9 字段 → 5 个固定过滤器
                const float lowCutFreq = eqTree.getProperty("lowCutFrequencyHz", 80.0f);
                const float lowShelfFreq = eqTree.getProperty("lowShelfFrequencyHz", 500.0f);
                const float lowShelfGain = eqTree.getProperty("lowShelfGainDb", 0.0f);
                const float peakFreq = eqTree.getProperty("peakFrequencyHz", 3000.0f);
                const float peakGain = eqTree.getProperty("peakGainDb", 0.0f);
                const float highShelfFreq = eqTree.getProperty("highShelfFrequencyHz", 8000.0f);
                const float highShelfGain = eqTree.getProperty("highShelfGainDb", 0.0f);
                const float highCutFreq = eqTree.getProperty("highCutFrequencyHz", 12000.0f);

                eq.filters = {
                    { EqFilterType::LowCut,   lowCutFreq,   0.0f,    0.707f, 0 },
                    { EqFilterType::LowShelf, lowShelfFreq, lowShelfGain, 2.0f, 1 },
                    { EqFilterType::Peak,     peakFreq,     peakGain, 2.0f, 2 },
                    { EqFilterType::HighShelf,highShelfFreq,highShelfGain, 2.0f, 3 },
                    { EqFilterType::HighCut,  highCutFreq,  0.0f,    0.707f, 4 }
                };
                parsedEq = true;
            }

            if (parsedEq && eq.isValid())
                note.eq = eq;
        }
        
        notes.push_back(note);
    }
    return notes;
}

// ============================================================================
// VolumeEnvelope 序列化（点数 + 每点 time/gainDb）
// ============================================================================

juce::ValueTree ProjectPersistence::volumeEnvelopeToValueTree(const AutomationLane& envelope)
{
    juce::ValueTree tree("VolumeEnvelope");
    const auto& points = envelope.points();
    tree.setProperty("pointCount", static_cast<int>(points.size()), nullptr);
    for (const auto& point : points) {
        juce::ValueTree pt("Point");
        pt.setProperty("time", point.timeSeconds, nullptr);
        pt.setProperty("gainDb", point.gainDb, nullptr);
        tree.addChild(pt, -1, nullptr);
    }
    return tree;
}

AutomationLane ProjectPersistence::volumeEnvelopeFromValueTree(const juce::ValueTree& tree)
{
    if (!tree.isValid()) { return {}; }
    return AutomationLane::fromSnapshot(automationPointsFromValueTree(tree));
}

// ============================================================================
// CorrectedSegments 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::segmentsToValueTree(
    const std::vector<ProjectContentEntry::SegmentEntry>& segments)
{
    juce::ValueTree tree("CorrectedSegments");
    for (const auto& seg : segments) {
        juce::ValueTree st("Segment");
        st.setProperty("startFrame", seg.startFrame, nullptr);
        st.setProperty("endFrame", seg.endFrame, nullptr);
        st.setProperty("source", static_cast<int>(seg.source), nullptr);
        st.setProperty("retuneSpeed", seg.retuneSpeed, nullptr);
        st.setProperty("pitchDriftScale", seg.pitchDriftScale, nullptr);
        st.setProperty("vibratoDepth", seg.vibratoDepth, nullptr);
        st.setProperty("vibratoRate", seg.vibratoRate, nullptr);
        if (!seg.f0Data.empty()) {
            juce::MemoryBlock block(seg.f0Data.data(), seg.f0Data.size() * sizeof(float));
            st.setProperty("f0Data", block.toBase64Encoding(), nullptr);
        }
        tree.addChild(st, -1, nullptr);
    }
    return tree;
}

std::vector<ProjectContentEntry::SegmentEntry> ProjectPersistence::segmentsFromValueTree(
    const juce::ValueTree& tree)
{
    std::vector<ProjectContentEntry::SegmentEntry> segments;
    if (!tree.isValid()) { return segments; }
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("Segment")) { continue; }
        ProjectContentEntry::SegmentEntry seg;
        seg.startFrame = static_cast<int>(child.getProperty("startFrame", 0));
        seg.endFrame = static_cast<int>(child.getProperty("endFrame", 0));
        seg.source = static_cast<uint8_t>(static_cast<int>(child.getProperty("source", 0)));
        seg.retuneSpeed = child.getProperty("retuneSpeed", -1.0f);
        seg.pitchDriftScale = child.getProperty("pitchDriftScale", 1.0f);
        seg.vibratoDepth = child.getProperty("vibratoDepth", -1.0f);
        seg.vibratoRate = child.getProperty("vibratoRate", -1.0f);
        // Deserialize f0Data
        auto f0DataBase64 = child.getProperty("f0Data", juce::String{}).toString();
        if (f0DataBase64.isNotEmpty()) {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding(f0DataBase64)) {
                const size_t numFloats = block.getSize() / sizeof(float);
                seg.f0Data.resize(numFloats);
                std::memcpy(seg.f0Data.data(), block.getData(), block.getSize());
            }
        }
        segments.push_back(seg);
    }
    return segments;
}

// ============================================================================
// TimeGrid 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::timeGridToValueTree(const ProjectContentEntry::TimeGridEntry& tg)
{
    juce::ValueTree tree("TimeGrid");
    tree.setProperty("revision", static_cast<int64_t>(tg.revision), nullptr);
    for (const auto& handle : tg.handles) {
        juce::ValueTree ht("Handle");
        ht.setProperty("id", static_cast<int64_t>(handle.id), nullptr);
        ht.setProperty("kind", static_cast<int>(handle.kind), nullptr);
        ht.setProperty("sourceSeconds", handle.sourceSeconds, nullptr);
        ht.setProperty("outputSeconds", handle.outputSeconds, nullptr);
        ht.setProperty("confidence", static_cast<int>(handle.confidence), nullptr);
        tree.addChild(ht, -1, nullptr);
    }
    return tree;
}

ProjectContentEntry::TimeGridEntry ProjectPersistence::timeGridFromValueTree(const juce::ValueTree& tree)
{
    ProjectContentEntry::TimeGridEntry tg;
    if (!tree.isValid()) { return tg; }
    tg.revision = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("revision", 0)));
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("Handle")) { continue; }
        ProjectContentEntry::TimeGridEntry::HandleEntry h;
        h.id = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("id", 0)));
        h.kind = static_cast<uint8_t>(static_cast<int>(child.getProperty("kind", 0)));
        h.sourceSeconds = child.getProperty("sourceSeconds", 0.0);
        h.outputSeconds = child.getProperty("outputSeconds", 0.0);
        h.confidence = static_cast<uint8_t>(static_cast<int>(child.getProperty("confidence", 0)));
        tg.handles.push_back(h);
    }
    return tg;
}

// ============================================================================
// Track 与 Placement 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::trackToValueTree(const ProjectTrackEntry& track)
{
    juce::ValueTree tree("Track");
    tree.setProperty("trackId", track.trackId, nullptr);
    setOptionalProperty(tree, "name", track.name);
    tree.setProperty("gain", track.gain, nullptr);
    tree.setProperty("mute", track.mute ? 1 : 0, nullptr);
    tree.setProperty("solo", track.solo ? 1 : 0, nullptr);
    setColourProperty(tree, "colour", track.colour);

    if (!track.placements.empty()) {
        tree.addChild(placementsToValueTree(track.placements), -1, nullptr);
    }

    return tree;
}

ProjectTrackEntry ProjectPersistence::trackFromValueTree(const juce::ValueTree& tree)
{
    ProjectTrackEntry t;
    t.trackId = static_cast<int>(tree.getProperty("trackId", 0));
    t.name = getOptionalProperty(tree, "name", "");
    t.gain = static_cast<float>(tree.getProperty("gain", 1.0));
    t.mute = static_cast<int>(tree.getProperty("mute", 0)) != 0;
    t.solo = static_cast<int>(tree.getProperty("solo", 0)) != 0;
    t.colour = getColourProperty(tree, "colour", juce::Colours::grey);

    auto placementsTree = tree.getChildWithName("Placements");
    if (placementsTree.isValid()) {
        t.placements = placementsFromValueTree(placementsTree);
    }

    return t;
}

juce::ValueTree ProjectPersistence::placementsToValueTree(const std::vector<ProjectPlacementEntry>& placements)
{
    juce::ValueTree tree("Placements");
    for (const auto& p : placements) {
        juce::ValueTree pt("Placement");
        pt.setProperty("placementId", static_cast<int64_t>(p.placementId), nullptr);
        pt.setProperty("contentDomain", static_cast<int>(p.contentKey.domainKind), nullptr);
        pt.setProperty("contentObjectId", static_cast<int64_t>(p.contentKey.objectId), nullptr);
        pt.setProperty("contentDiscriminator", static_cast<int64_t>(p.contentKey.sourceWindowDiscriminator), nullptr);
        pt.setProperty("mappingRevision", static_cast<int64_t>(p.mappingRevision), nullptr);
        pt.setProperty("timelineStartSeconds", p.timelineStartSeconds, nullptr);
        pt.setProperty("timelineDurationSeconds", p.timelineDurationSeconds, nullptr);
        pt.setProperty("clipGain", p.clipGain, nullptr);
        pt.setProperty("fadeInDurationSeconds", p.fadeInDurationSeconds, nullptr);
        pt.setProperty("fadeOutDurationSeconds", p.fadeOutDurationSeconds, nullptr);
        pt.setProperty("clipInSeconds", p.clipInSeconds, nullptr);
        setOptionalProperty(pt, "name", p.name);
        tree.addChild(pt, -1, nullptr);
    }
    return tree;
}

std::vector<ProjectPlacementEntry> ProjectPersistence::placementsFromValueTree(const juce::ValueTree& tree)
{
    std::vector<ProjectPlacementEntry> placements;
    if (!tree.isValid()) { return placements; }
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("Placement")) { continue; }
        ProjectPlacementEntry p;
        p.placementId = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("placementId", 0)));
        p.contentKey.domainKind = static_cast<DomainKind>(static_cast<int>(child.getProperty("contentDomain", 0)));
        p.contentKey.objectId = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("contentObjectId", 0)));
        p.contentKey.sourceWindowDiscriminator = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("contentDiscriminator", 0)));
        p.mappingRevision = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("mappingRevision", 0)));
        p.timelineStartSeconds = child.getProperty("timelineStartSeconds", 0.0);
        p.timelineDurationSeconds = child.getProperty("timelineDurationSeconds", 0.0);
        p.clipGain = static_cast<float>(child.getProperty("clipGain", 1.0));
        p.fadeInDurationSeconds = child.getProperty("fadeInDurationSeconds", 0.0);
        p.fadeOutDurationSeconds = child.getProperty("fadeOutDurationSeconds", 0.0);
        p.clipInSeconds = child.getProperty("clipInSeconds", 0.0);
        p.name = getOptionalProperty(child, "name", "");
        placements.push_back(p);
    }
    return placements;
}

// ============================================================================
// ReferenceBinding 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::referenceBindingToValueTree(const ProjectReferenceBinding& binding)
{
    juce::ValueTree tree("ReferenceBinding");
    tree.setProperty("targetPlacementId", static_cast<int64_t>(binding.targetPlacementId), nullptr);
    tree.setProperty("referencePlacementId", static_cast<int64_t>(binding.referencePlacementId), nullptr);
    return tree;
}

ProjectReferenceBinding ProjectPersistence::referenceBindingFromValueTree(const juce::ValueTree& tree)
{
    ProjectReferenceBinding rb;
    rb.targetPlacementId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("targetPlacementId", 0)));
    rb.referencePlacementId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("referencePlacementId", 0)));
    return rb;
}

// ============================================================================
// SilentGaps 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::silentGapsToValueTree(const std::vector<ProjectContentEntry::SilentGapEntry>& gaps)
{
    juce::ValueTree tree("SilentGaps");
    for (const auto& gap : gaps) {
        juce::ValueTree gt("SilentGap");
        gt.setProperty("startSample", static_cast<int64_t>(gap.startSample), nullptr);
        gt.setProperty("endSampleExclusive", static_cast<int64_t>(gap.endSampleExclusive), nullptr);
        gt.setProperty("minLevel_dB", gap.minLevel_dB, nullptr);
        tree.addChild(gt, -1, nullptr);
    }
    return tree;
}

std::vector<ProjectContentEntry::SilentGapEntry> ProjectPersistence::silentGapsFromValueTree(const juce::ValueTree& tree)
{
    std::vector<ProjectContentEntry::SilentGapEntry> gaps;
    if (!tree.isValid()) { return gaps; }
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("SilentGap")) { continue; }
        ProjectContentEntry::SilentGapEntry gap;
        gap.startSample = static_cast<int64_t>(child.getProperty("startSample", 0));
        gap.endSampleExclusive = static_cast<int64_t>(child.getProperty("endSampleExclusive", 0));
        gap.minLevel_dB = child.getProperty("minLevel_dB", 0.0f);
        gaps.push_back(gap);
    }
    return gaps;
}

// ============================================================================
// ReferenceFeatures 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::referenceFeaturesToValueTree(const ProjectContentEntry::ReferenceFeatureEntry& rf)
{
    juce::ValueTree tree("ReferenceFeatures");
    tree.setProperty("analysisRevision", rf.analysisRevision, nullptr);
    tree.setProperty("status", static_cast<int>(rf.status), nullptr);
    tree.setProperty("producer", static_cast<int>(rf.producer), nullptr);
    tree.setProperty("inputFingerprint", static_cast<int64_t>(rf.inputFingerprint), nullptr);
    tree.setProperty("sourceDurationSeconds", rf.sourceDurationSeconds, nullptr);
    setOptionalProperty(tree, "errorMessage", rf.errorMessage);

    // Pitch notes
    if (!rf.pitchNotes.empty()) {
        tree.addChild(notesToValueTree(rf.pitchNotes, "PitchNotes"), -1, nullptr);
    }

    // Timing anchors
    if (!rf.timingAnchors.empty()) {
        juce::ValueTree taTree("TimingAnchors");
        for (const auto& anchor : rf.timingAnchors) {
            juce::ValueTree at("TimingAnchor");
            at.setProperty("anchorId", static_cast<int64_t>(anchor.anchorId), nullptr);
            at.setProperty("sourceSeconds", anchor.sourceSeconds, nullptr);
            at.setProperty("strength", anchor.strength, nullptr);
            at.setProperty("kind", static_cast<int>(anchor.kind), nullptr);
            at.setProperty("confidence", anchor.confidence, nullptr);
            taTree.addChild(at, -1, nullptr);
        }
        tree.addChild(taTree, -1, nullptr);
    }

    return tree;
}

ProjectContentEntry::ReferenceFeatureEntry ProjectPersistence::referenceFeaturesFromValueTree(const juce::ValueTree& tree, int formatVersion)
{
    ProjectContentEntry::ReferenceFeatureEntry rf;
    if (!tree.isValid()) { return rf; }

    rf.analysisRevision = static_cast<int>(tree.getProperty("analysisRevision", 0));
    rf.status = static_cast<uint8_t>(static_cast<int>(tree.getProperty("status", 0)));
    rf.producer = static_cast<uint8_t>(static_cast<int>(tree.getProperty("producer", 0)));
    rf.inputFingerprint = static_cast<int64_t>(tree.getProperty("inputFingerprint", 0));
    rf.sourceDurationSeconds = tree.getProperty("sourceDurationSeconds", 0.0);
    rf.errorMessage = getOptionalProperty(tree, "errorMessage", "");

    // Pitch notes
    rf.pitchNotes = notesFromValueTree(tree.getChildWithName("PitchNotes"), formatVersion);

    // Timing anchors
    auto taTree = tree.getChildWithName("TimingAnchors");
    if (taTree.isValid()) {
        for (int i = 0; i < taTree.getNumChildren(); ++i) {
            auto child = taTree.getChild(i);
            if (!child.hasType("TimingAnchor")) { continue; }
            ProjectContentEntry::ReferenceFeatureEntry::TimingAnchorEntry anchor;
            anchor.anchorId = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("anchorId", 0)));
            anchor.sourceSeconds = child.getProperty("sourceSeconds", 0.0);
            anchor.strength = child.getProperty("strength", 0.0f);
            anchor.kind = static_cast<uint8_t>(static_cast<int>(child.getProperty("kind", 0)));
            anchor.confidence = child.getProperty("confidence", 0.0f);
            rf.timingAnchors.push_back(anchor);
        }
    }

    return rf;
}

} // namespace OpenTune

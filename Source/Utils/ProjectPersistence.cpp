#include "ProjectPersistence.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace OpenTune {

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

    root.setProperty(kProjectFormatVersionAttr, snapshot.header.projectFormatVersion, nullptr);
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

    if (!snapshot.materializations.empty()) {
        juce::ValueTree matList("Materializations");
        for (const auto& mat : snapshot.materializations) {
            matList.addChild(materializationToValueTree(mat), -1, nullptr);
        }
        root.addChild(matList, -1, nullptr);
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
    if (version != kCurrentProjectFormatVersion) {
        const auto msg = "Unsupported project format version: " + juce::String(version)
            + " (expected " + juce::String(kCurrentProjectFormatVersion) + ")";
        return Result<ProjectSnapshot>::failure(Error::fromCode(ErrorCode::InvalidParameter, msg.toStdString()));
    }

    ProjectSnapshot snapshot;

    // Header
    snapshot.header.projectFormatVersion = version;
    snapshot.header.appVersion = getOptionalProperty(tree, juce::Identifier(kAppVersionAttr), "");
    snapshot.header.projectName = getOptionalProperty(tree, "projectName", "Untitled");
    snapshot.header.projectId = getOptionalProperty(tree, "projectId", "");
    snapshot.header.createdAt = getOptionalProperty(tree, "createdAt", "");
    snapshot.header.lastSavedAt = getOptionalProperty(tree, "lastSavedAt", "");

    // ProjectSettings
    auto settingsTree = tree.getChildWithName("ProjectSettings");
    if (settingsTree.isValid()) {
        snapshot.settings = settingsFromValueTree(settingsTree);
    }

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

    // Materializations
    auto matList = tree.getChildWithName("Materializations");
    if (matList.isValid()) {
        for (int i = 0; i < matList.getNumChildren(); ++i) {
            auto child = matList.getChild(i);
            if (child.hasType("Materialization")) {
                snapshot.materializations.push_back(materializationFromValueTree(child));
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
            Error::fromCode(ErrorCode::ModelNotFound,
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
// 序列化辅助 — Materialization
// ============================================================================

juce::ValueTree ProjectPersistence::materializationToValueTree(const ProjectMaterializationEntry& mat)
{
    juce::ValueTree tree("Materialization");
    tree.setProperty("materializationId", static_cast<int64_t>(mat.materializationId), nullptr);
    tree.setProperty("sourceId", static_cast<int64_t>(mat.sourceId), nullptr);
    tree.setProperty("retired", mat.retired ? 1 : 0, nullptr);
    tree.setProperty("renderRevision", static_cast<int64_t>(mat.renderRevision), nullptr);
    tree.setProperty("lineageParentMaterializationId", static_cast<int64_t>(mat.lineageParentMaterializationId), nullptr);

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
    tree.addChild(dkTree, -1, nullptr);

    // Notes
    if (!mat.notes.empty()) {
        tree.addChild(notesToValueTree(mat.notes, "Notes"), -1, nullptr);
    }

    // CorrectedSegments
    if (!mat.correctedSegments.empty()) {
        tree.addChild(segmentsToValueTree(mat.correctedSegments), -1, nullptr);
    }

    // TimeGrid
    tree.addChild(timeGridToValueTree(mat.timeGrid), -1, nullptr);

    // DerivedAnalysis (wrapped in a parent node per spec)
    {
        juce::ValueTree daTree("DerivedAnalysis");
        daTree.addChild(derivedAnalysisToValueTree(mat.basicAnalysis, "Basic"), -1, nullptr);
        daTree.addChild(derivedAnalysisToValueTree(mat.enhancedAnalysis, "Enhanced"), -1, nullptr);
        tree.addChild(daTree, -1, nullptr);
    }

    return tree;
}

ProjectMaterializationEntry ProjectPersistence::materializationFromValueTree(const juce::ValueTree& tree)
{
    ProjectMaterializationEntry m;
    m.materializationId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("materializationId", 0)));
    m.sourceId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("sourceId", 0)));
    m.retired = static_cast<int>(tree.getProperty("retired", 0)) != 0;
    m.renderRevision = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("renderRevision", 0)));
    m.lineageParentMaterializationId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("lineageParentMaterializationId", 0)));

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
    }

    // Notes
    m.notes = notesFromValueTree(tree.getChildWithName("Notes"));

    // CorrectedSegments
    m.correctedSegments = segmentsFromValueTree(tree.getChildWithName("CorrectedSegments"));

    // TimeGrid
    auto tgTree = tree.getChildWithName("TimeGrid");
    if (tgTree.isValid()) {
        m.timeGrid = timeGridFromValueTree(tgTree);
    }

    // DerivedAnalysis
    auto basicTree = tree.getChildWithName("DerivedAnalysis");
    if (basicTree.isValid()) {
        auto basicNode = basicTree.getChildWithName("Basic");
        if (basicNode.isValid()) {
            m.basicAnalysis = derivedAnalysisFromValueTree(basicNode);
        }
        auto enhancedNode = basicTree.getChildWithName("Enhanced");
        if (enhancedNode.isValid()) {
            m.enhancedAnalysis = derivedAnalysisFromValueTree(enhancedNode);
        }
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
        nt.setProperty("vibratoDepth", note.vibratoDepth, nullptr);
        nt.setProperty("vibratoRate", note.vibratoRate, nullptr);
        nt.setProperty("velocity", note.velocity, nullptr);
        nt.setProperty("isVoiced", note.isVoiced ? 1 : 0, nullptr);
        nt.setProperty("selected", note.selected ? 1 : 0, nullptr);
        tree.addChild(nt, -1, nullptr);
    }
    return tree;
}

std::vector<Note> ProjectPersistence::notesFromValueTree(const juce::ValueTree& tree)
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
        note.vibratoDepth = child.getProperty("vibratoDepth", -1.0f);
        note.vibratoRate = child.getProperty("vibratoRate", -1.0f);
        note.velocity = child.getProperty("velocity", 1.0f);
        note.isVoiced = static_cast<int>(child.getProperty("isVoiced", 1)) != 0;
        note.selected = static_cast<int>(child.getProperty("selected", 0)) != 0;
        notes.push_back(note);
    }
    return notes;
}

// ============================================================================
// CorrectedSegments 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::segmentsToValueTree(
    const std::vector<ProjectMaterializationEntry::SegmentEntry>& segments)
{
    juce::ValueTree tree("CorrectedSegments");
    for (const auto& seg : segments) {
        juce::ValueTree st("Segment");
        st.setProperty("startFrame", seg.startFrame, nullptr);
        st.setProperty("endFrame", seg.endFrame, nullptr);
        st.setProperty("source", static_cast<int>(seg.source), nullptr);
        st.setProperty("retuneSpeed", seg.retuneSpeed, nullptr);
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

std::vector<ProjectMaterializationEntry::SegmentEntry> ProjectPersistence::segmentsFromValueTree(
    const juce::ValueTree& tree)
{
    std::vector<ProjectMaterializationEntry::SegmentEntry> segments;
    if (!tree.isValid()) { return segments; }
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("Segment")) { continue; }
        ProjectMaterializationEntry::SegmentEntry seg;
        seg.startFrame = static_cast<int>(child.getProperty("startFrame", 0));
        seg.endFrame = static_cast<int>(child.getProperty("endFrame", 0));
        seg.source = static_cast<uint8_t>(static_cast<int>(child.getProperty("source", 0)));
        seg.retuneSpeed = child.getProperty("retuneSpeed", -1.0f);
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
// DerivedAnalysis 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::derivedAnalysisToValueTree(
    const ProjectDerivedAnalysisEntry& analysis, const juce::String& nodeName)
{
    juce::ValueTree tree(nodeName);
    tree.setProperty("analysisRevision", analysis.analysisRevision, nullptr);
    setOptionalProperty(tree, "inputFingerprint", analysis.inputFingerprint);
    setOptionalProperty(tree, "status", analysis.status);
    setOptionalProperty(tree, "backend", analysis.backend);

    if (!analysis.notes.empty()) {
        tree.addChild(notesToValueTree(analysis.notes, "Notes"), -1, nullptr);
    }

    if (!analysis.anchors.empty()) {
        juce::ValueTree anchorsNode("Anchors");
        for (const auto& anchor : analysis.anchors) {
            juce::ValueTree at("Anchor");
            at.setProperty("id", anchor.id, nullptr);
            at.setProperty("sourceSeconds", anchor.sourceSeconds, nullptr);
            at.setProperty("strength", anchor.strength, nullptr);
            anchorsNode.addChild(at, -1, nullptr);
        }
        tree.addChild(anchorsNode, -1, nullptr);
    }

    return tree;
}

ProjectDerivedAnalysisEntry ProjectPersistence::derivedAnalysisFromValueTree(const juce::ValueTree& tree)
{
    ProjectDerivedAnalysisEntry da;
    if (!tree.isValid()) { return da; }
    da.analysisRevision = static_cast<int>(tree.getProperty("analysisRevision", 0));
    da.inputFingerprint = getOptionalProperty(tree, "inputFingerprint", "");
    da.status = getOptionalProperty(tree, "status", "NotRequested");
    da.backend = getOptionalProperty(tree, "backend", "CPU");
    da.notes = notesFromValueTree(tree.getChildWithName("Notes"));
    da.anchors = anchorsFromValueTree(tree.getChildWithName("Anchors"));
    return da;
}

std::vector<ProjectDerivedAnalysisEntry::AnchorEntry> ProjectPersistence::anchorsFromValueTree(
    const juce::ValueTree& tree)
{
    std::vector<ProjectDerivedAnalysisEntry::AnchorEntry> anchors;
    if (!tree.isValid()) { return anchors; }
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("Anchor")) { continue; }
        ProjectDerivedAnalysisEntry::AnchorEntry a;
        a.id = static_cast<int>(child.getProperty("id", 0));
        a.sourceSeconds = child.getProperty("sourceSeconds", 0.0);
        a.strength = child.getProperty("strength", 0.0f);
        anchors.push_back(a);
    }
    return anchors;
}

// ============================================================================
// TimeGrid 序列化
// ============================================================================

juce::ValueTree ProjectPersistence::timeGridToValueTree(const ProjectMaterializationEntry::TimeGridEntry& tg)
{
    juce::ValueTree tree("TimeGrid");
    tree.setProperty("revision", static_cast<int64_t>(tg.revision), nullptr);
    for (const auto& handle : tg.handles) {
        juce::ValueTree ht("Handle");
        ht.setProperty("id", handle.id, nullptr);
        ht.setProperty("kind", static_cast<int>(handle.kind), nullptr);
        ht.setProperty("sourceSeconds", handle.sourceSeconds, nullptr);
        ht.setProperty("outputSeconds", handle.outputSeconds, nullptr);
        ht.setProperty("confidence", handle.confidence, nullptr);
        ht.setProperty("isUserAdded", handle.isUserAdded ? 1 : 0, nullptr);
        tree.addChild(ht, -1, nullptr);
    }
    return tree;
}

ProjectMaterializationEntry::TimeGridEntry ProjectPersistence::timeGridFromValueTree(const juce::ValueTree& tree)
{
    ProjectMaterializationEntry::TimeGridEntry tg;
    if (!tree.isValid()) { return tg; }
    tg.revision = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("revision", 0)));
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto child = tree.getChild(i);
        if (!child.hasType("Handle")) { continue; }
        ProjectMaterializationEntry::TimeGridEntry::HandleEntry h;
        h.id = static_cast<int>(child.getProperty("id", 0));
        h.kind = static_cast<uint8_t>(static_cast<int>(child.getProperty("kind", 0)));
        h.sourceSeconds = child.getProperty("sourceSeconds", 0.0);
        h.outputSeconds = child.getProperty("outputSeconds", 0.0);
        h.confidence = child.getProperty("confidence", 0.0f);
        h.isUserAdded = static_cast<int>(child.getProperty("isUserAdded", 0)) != 0;
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
        pt.setProperty("materializationId", static_cast<int64_t>(p.materializationId), nullptr);
        pt.setProperty("mappingRevision", static_cast<int64_t>(p.mappingRevision), nullptr);
        pt.setProperty("timelineStartSeconds", p.timelineStartSeconds, nullptr);
        pt.setProperty("timelineDurationSeconds", p.timelineDurationSeconds, nullptr);
        pt.setProperty("clipGain", p.clipGain, nullptr);
        pt.setProperty("fadeInDurationSeconds", p.fadeInDurationSeconds, nullptr);
        pt.setProperty("fadeOutDurationSeconds", p.fadeOutDurationSeconds, nullptr);
        pt.setProperty("clipInSeconds", p.clipInSeconds, nullptr);
        setOptionalProperty(pt, "name", p.name);
        setColourProperty(pt, "colour", p.colour);
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
        p.materializationId = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("materializationId", 0)));
        p.mappingRevision = static_cast<uint64_t>(static_cast<int64_t>(child.getProperty("mappingRevision", 0)));
        p.timelineStartSeconds = child.getProperty("timelineStartSeconds", 0.0);
        p.timelineDurationSeconds = child.getProperty("timelineDurationSeconds", 0.0);
        p.clipGain = static_cast<float>(child.getProperty("clipGain", 1.0));
        p.fadeInDurationSeconds = child.getProperty("fadeInDurationSeconds", 0.0);
        p.fadeOutDurationSeconds = child.getProperty("fadeOutDurationSeconds", 0.0);
        p.clipInSeconds = child.getProperty("clipInSeconds", 0.0);
        p.name = getOptionalProperty(child, "name", "");
        p.colour = getColourProperty(child, "colour", juce::Colours::grey);
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
    tree.setProperty("bindingRevision", static_cast<int64_t>(binding.bindingRevision), nullptr);
    setOptionalProperty(tree, "analysisMode", binding.analysisMode);
    return tree;
}

ProjectReferenceBinding ProjectPersistence::referenceBindingFromValueTree(const juce::ValueTree& tree)
{
    ProjectReferenceBinding rb;
    rb.targetPlacementId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("targetPlacementId", 0)));
    rb.referencePlacementId = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("referencePlacementId", 0)));
    rb.bindingRevision = static_cast<uint64_t>(static_cast<int64_t>(tree.getProperty("bindingRevision", 0)));
    rb.analysisMode = getOptionalProperty(tree, "analysisMode", "Basic");
    return rb;
}

} // namespace OpenTune

/**
 * HandleNoteMerger.cpp — 实现 barrier merge + 算法。
 * 见 HandleNoteMerger.h 头注释。
 */
#include "HandleNoteMerger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_set>

namespace OpenTune {

namespace {

// 单调递增的 stable id 生成器，与 TimeGrid.cpp 内部一致。
// 这里独立计数以避免 cross-file ID 冲突——TimeGrid.cpp 的 makeStableId 也独立。
// merge 产生的新 NoteOnly handle id 与 WordSegmenter handles id 不冲突即可。
std::atomic<uint64_t> g_mergerStableId{0};

uint64_t allocateNoteOnlyId() {
    // 用高位 bit 标识 merger 产生的 id，与 WordSegmenter id 区分（约定）。
    // 保留 (1ULL<<63) 作为 NoteOnly handle 的高位，调用方不应假设 id 数值范围。
    constexpr uint64_t kNoteOnlyTag = 1ULL << 62;
    return kNoteOnlyTag | g_mergerStableId.fetch_add(1, std::memory_order_relaxed);
}

bool isEndpointKind(HandleKind k) noexcept {
    return k == HandleKind::ClipStart || k == HandleKind::ClipEnd;
}

} // namespace

// ============================================================================
// Barrier
// ============================================================================

void HandleNoteMerger::deliverHandles(std::vector<TimeHandle> handles) {
    cachedHandles_ = std::move(handles);
    tryFireMerge();
}

void HandleNoteMerger::deliverNotes(std::vector<Note> notes) {
    cachedNotes_ = std::move(notes);
    tryFireMerge();
}

void HandleNoteMerger::reset() {
    cachedHandles_.reset();
    cachedNotes_.reset();
}

void HandleNoteMerger::tryFireMerge() {
    if (!cachedHandles_.has_value() || !cachedNotes_.has_value()) {
        return;
    }
    auto merged = merge(*cachedHandles_, *cachedNotes_);
    if (onMergeComplete) {
        onMergeComplete(std::move(merged));
    }
    // Barrier 一次性触发：清空缓存避免后续重复触发。
    // 用户后续再编辑 notes 不会自动再 merge（per spec zombie 防护）。
    cachedHandles_.reset();
    cachedNotes_.reset();
}

// ============================================================================
// merge — Initial pass
// ============================================================================

std::vector<TimeHandle> HandleNoteMerger::merge(
    const std::vector<TimeHandle>& handlesPre,
    const std::vector<Note>& notes)
{
    // 复制 handles（confidence 字段稍后被 Pass 1 修改）
    std::vector<TimeHandle> result(handlesPre);

    if (notes.empty() || result.size() < 2) {
        return result;
    }

    const double clipStartSrc = result.front().source_seconds;
    const double clipEndSrc   = result.back().source_seconds;

    // Pass 1: 对每个 note 找最近的非 endpoint handle (within threshold),
    //         命中 → handle.confidence = High。note 被标记为 matched，
    //         不参与 Pass 2 失配补位。
    //         多个 note 可命中同一 handle (handle 已 High 后保持 High)。
    std::unordered_set<size_t> matchedNoteIdx;
    for (size_t ni = 0; ni < notes.size(); ++ni) {
        double bestDist = kMatchThresholdSeconds + 1.0;
        size_t bestHandleIdx = static_cast<size_t>(-1);
        for (size_t i = 0; i < result.size(); ++i) {
            if (isEndpointKind(result[i].kind)) continue;
            const double dist = std::abs(notes[ni].startTime - result[i].source_seconds);
            if (dist <= kMatchThresholdSeconds && dist < bestDist) {
                bestDist = dist;
                bestHandleIdx = i;
            }
        }
        if (bestHandleIdx != static_cast<size_t>(-1)) {
            result[bestHandleIdx].confidence = Confidence::High;
            matchedNoteIdx.insert(ni);
        }
    }

    // Pass 2: 失配 note → NoteOnly handle 候选
    std::vector<TimeHandle> noteOnlyCandidates;
    for (size_t ni = 0; ni < notes.size(); ++ni) {
        if (matchedNoteIdx.count(ni)) continue;
        const double t = notes[ni].startTime;

        // 排除靠近 endpoint 的 note (T_match 内)
        if (t <= clipStartSrc + kMatchThresholdSeconds) continue;
        if (t >= clipEndSrc - kMatchThresholdSeconds) continue;

        // 与现有最近 handle 距离 >= min-segment 才能插入
        double minDistToExisting = std::numeric_limits<double>::infinity();
        for (const auto& h : result) {
            const double d = std::abs(h.source_seconds - t);
            minDistToExisting = std::min(minDistToExisting, d);
        }
        // 与候选 NoteOnly 之间也应满足 min-segment，避免两个相近 note 都插入
        for (const auto& cand : noteOnlyCandidates) {
            const double d = std::abs(cand.source_seconds - t);
            minDistToExisting = std::min(minDistToExisting, d);
        }
        if (minDistToExisting < kMinSegmentSeconds) continue;

        TimeHandle h;
        h.id = allocateNoteOnlyId();
        h.source_seconds = t;
        h.output_seconds = t;
        h.kind = HandleKind::NoteOnly;
        h.locked = false;
        h.confidence = Confidence::Default;
        noteOnlyCandidates.push_back(h);
    }

    // 合并 + 按 source_seconds 排序
    result.insert(result.end(), noteOnlyCandidates.begin(), noteOnlyCandidates.end());
    std::sort(result.begin(), result.end(),
              [](const TimeHandle& a, const TimeHandle& b) {
                  return a.source_seconds < b.source_seconds;
              });

    return result;
}

// ============================================================================
// reSeed — Manual re-seed
// ============================================================================

std::vector<TimeHandle> HandleNoteMerger::reSeed(
    const TimeGridSnapshot& currentSnapshot,
    const std::vector<TimeHandle>& handlesPre,
    const std::vector<Note>& notes)
{
    const auto& currentHandles = currentSnapshot.handles();

    // Step 1: 收集 UserAdded handles (完整保留)
    std::vector<TimeHandle> userAdded;
    for (const auto& h : currentHandles) {
        if (h.kind == HandleKind::UserAdded) {
            userAdded.push_back(h);
        }
    }

    // Step 2: 对 handlesPre 中每个非 endpoint handle，
    //         查找 currentHandles 中相同 source_seconds 的 auto handle，
    //         继承 output_seconds（用户编辑保留）。
    //         未匹配的 → output_seconds = source_seconds (恒等)
    std::vector<TimeHandle> reseededAuto;
    for (const auto& hPre : handlesPre) {
        if (isEndpointKind(hPre.kind)) continue;

        TimeHandle h = hPre;
        h.confidence = Confidence::Default; // 重置，由 merge 重算
        // 查找现有 handle 同 source_seconds（auto kinds 不含 UserAdded/NoteOnly）
        for (const auto& cur : currentHandles) {
            if (isEndpointKind(cur.kind)) continue;
            if (cur.kind == HandleKind::UserAdded) continue;
            if (cur.kind == HandleKind::NoteOnly) continue;
            if (std::abs(cur.source_seconds - hPre.source_seconds) < 1e-9) {
                h.output_seconds = cur.output_seconds; // 继承用户编辑
                h.id = cur.id;                          // 保持稳定 id
                break;
            }
        }
        reseededAuto.push_back(h);
    }

    // Step 3: 组装 ClipStart + reseededAuto + UserAdded + ClipEnd
    std::vector<TimeHandle> assembled;
    assembled.push_back(currentHandles.front()); // ClipStart
    for (auto& h : reseededAuto) assembled.push_back(h);
    for (auto& h : userAdded) assembled.push_back(h);
    assembled.push_back(currentHandles.back()); // ClipEnd
    std::sort(assembled.begin(), assembled.end(),
              [](const TimeHandle& a, const TimeHandle& b) {
                  return a.source_seconds < b.source_seconds;
              });

    // Step 4: 跑 merge pass on assembled + notes
    return merge(assembled, notes);
}

} // namespace OpenTune

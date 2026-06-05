#pragma once

/**
 * 音符和音符序列数据结构
 * 
 * 定义音频处理中使用的核心数据结构：
 * - Note：单个音符的时间、音高、颤音参数等
 * - NoteSequence：音符序列管理，支持插入、删除、查询等操作
 * - LineAnchor：音高线锚点，用于手绘F0曲线
 */

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

namespace OpenTune {

struct Note {
    double startTime = 0.0;         // 起始时间（秒）
    double endTime = 0.0;           // 结束时间（秒）
    float pitch = 0.0f;             // 基准音高 (Hz)
    float originalPitch = 0.0f;     // 原始检测音高 (Hz，未量化)，用于相对移调
    float pitchOffset = 0.0f;       // 音高偏移（半音），用于拖拽调整
    float retuneSpeed = -1.0f;      // 重调速度（-1表示使用默认值）
    float vibratoDepth = -1.0f;     // 颤音深度（-1表示使用默认值）
    float vibratoRate = -1.0f;      // 颤音速率（-1表示使用默认值）
    float velocity = 1.0f;          // 力度
    bool isVoiced = true;           // 是否为有声段
    bool selected = false;          // 选中状态
    bool dirty = false;             // 脏标记，用于增量渲染

    double getDuration() const {
        return endTime - startTime;
    }

    float getAdjustedPitch() const {
        if (pitch <= 0.0f) return 0.0f;
        return pitch * std::pow(2.0f, pitchOffset / 12.0f);
    }

    int getMidiNote() const {
        float adjustedPitch = getAdjustedPitch();
        if (adjustedPitch <= 0.0f) {
            return 0;
        }
        return static_cast<int>(std::round(69.0f + 12.0f * std::log2(adjustedPitch / 440.0f)));
    }

    int getBaseMidiNote() const {
        if (pitch <= 0.0f) {
            return 0;
        }
        return static_cast<int>(std::round(69.0f + 12.0f * std::log2(pitch / 440.0f)));
    }

    static float midiToFrequency(int midiNote) {
        return 440.0f * std::pow(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
    }

    static int frequencyToMidi(float frequency) {
        if (frequency <= 0.0f) {
            return 0;
        }
        return static_cast<int>(std::round(69.0f + 12.0f * std::log2(frequency / 440.0f)));
    }
};

class NoteSequence {
public:
    NoteSequence() = default;
    ~NoteSequence() = default;

    void insertNoteSorted(const Note& note) {
        if (note.endTime <= note.startTime) {
            return;
        }
        notes_.push_back(note);
        normalizeNonOverlapping(notes_);
    }

    void setNotesSorted(const std::vector<Note>& notes) {
        notes_ = notes;
        normalizeNonOverlapping(notes_);
    }

    void clear() {
        notes_.clear();
    }

    const std::vector<Note>& getNotes() const {
        return notes_;
    }

    std::vector<Note>& getNotes() {
        return notes_;
    }

    size_t size() const {
        return notes_.size();
    }

    bool isEmpty() const {
        return notes_.empty();
    }

    void eraseRange(double startTime, double endTime) {
        if (endTime < startTime) {
            std::swap(startTime, endTime);
        }
        if (endTime <= startTime) {
            return;
        }

        std::vector<Note> updated;
        updated.reserve(notes_.size() + 2);

        for (const auto& note : notes_) {
            bool overlap = (note.endTime > startTime && note.startTime < endTime);
            if (!overlap) {
                updated.push_back(note);
                continue;
            }

            if (note.startTime < startTime) {
                Note left = note;
                left.endTime = startTime;
                left.selected = false;
                left.dirty = true;
                if (left.endTime > left.startTime) {
                    updated.push_back(left);
                }
            }

            if (note.endTime > endTime) {
                Note right = note;
                right.startTime = endTime;
                right.selected = false;
                right.dirty = true;
                if (right.endTime > right.startTime) {
                    updated.push_back(right);
                }
            }
        }

        notes_ = std::move(updated);
    }

private:
    static void normalizeNonOverlapping(std::vector<Note>& notes) {
        notes.erase(
            std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
                return n.endTime <= n.startTime;
            }),
            notes.end());

        std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
            return (a.startTime < b.startTime) || (a.startTime == b.startTime && a.endTime < b.endTime);
        });

        for (size_t i = 1; i < notes.size(); ++i) {
            if (notes[i - 1].endTime > notes[i].startTime) {
                notes[i - 1].endTime = notes[i].startTime;
            }
        }

        notes.erase(
            std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
                return n.endTime <= n.startTime;
            }),
            notes.end());
    }

    std::vector<Note> notes_;
};

struct LineAnchor {
    int id = 0;             // 锚点ID
    double time = 0.0;      // 时间位置（秒）
    float freq = 0.0f;      // 频率 (Hz)
    bool selected = false;  // 选中状态
};

// 标准化存储的 notes：去重叠、排序、去零时长 note
inline std::vector<Note> normalizeStoredNotes(const std::vector<Note>& notes)
{
    NoteSequence sequence;
    sequence.setNotesSorted(notes);
    return sequence.getNotes();
}

} // namespace OpenTune

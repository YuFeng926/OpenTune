#pragma once

#include <cmath>
#include <limits>

namespace OpenTune {

/** 进入钢琴卷帘时用来和播放头比较的音符。时间与查询时间同一域（时间轴秒）。 */
struct CenterableNote
{
    double startTime = 0.0;
    double endTime = 0.0;
    float adjustedPitchHz = 0.0f;
};

inline bool isCenterableNote(const CenterableNote& note) noexcept
{
    return note.endTime > note.startTime
        && std::isfinite(note.startTime)
        && std::isfinite(note.endTime)
        && std::isfinite(note.adjustedPitchHz)
        && note.adjustedPitchHz > 0.0f;
}

/** 查询时间到音符区间的距离。落在音符内部时为 0。 */
inline double distanceFromTimeToNote(double time, const CenterableNote& note) noexcept
{
    if (time < note.startTime)
        return note.startTime - time;
    if (time > note.endTime)
        return time - note.endTime;
    return 0.0;
}

/**
 * 离 time 最近的可定位音符。
 * 落在音符内部距离为 0；在外面则量到最近的端点。
 * 距离相同保留开始更早的那一个。没有则返回 -1。
 */
inline int noteIndexNearestTime(const CenterableNote* notes, int count, double time) noexcept
{
    if (!std::isfinite(time))
        time = 0.0;

    int best = -1;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int i = 0; i < count; ++i)
    {
        if (notes == nullptr || !isCenterableNote(notes[i]))
            continue;
        const double distance = distanceFromTimeToNote(time, notes[i]);
        if (best < 0 || distance < bestDistance
            || (distance == bestDistance && notes[i].startTime < notes[best].startTime))
        {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

/**
 * 视口要对准的时间。播放头在音符内时就是播放头；
 * 否则是离播放头更近的那个端点，这样音符本身落在画面中央。
 */
inline double noteCenterTimeNearest(double time, const CenterableNote& note) noexcept
{
    if (!std::isfinite(time) || time < note.startTime)
        return note.startTime;
    if (time > note.endTime)
        return note.endTime;
    return time;
}

} // namespace OpenTune

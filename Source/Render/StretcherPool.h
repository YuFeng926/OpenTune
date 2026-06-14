#pragma once

#include "../Content/ContentKey.h"
#include <juce_core/juce_core.h>
#include <map>
#include <memory>

namespace OpenTune {

class SoundTouchStretcher; // forward

/**
 * StretcherPool — SoundTouchStretcher インスタンス管理。
 *
 * ContentKey 単位で SoundTouchStretcher の作成、検索、削除を管理。
 * 各 content に最大 1 つの stretcher を持ち、Stage 2 time-stretch build に使用。
 *
 * Phase 0: content owner の per-entry stretcher から抽出 (h:310, cpp:591+)
 *
 * 再利用コード：
 * - content owner.h:223-225, h:310 (per-entry stretcher)
 * - content owner.cpp:591+ (getOpenTuneStretcher)
 *
 * ContentRenderService.cpp:263-280 の vector モードは使用しない (線形検索、効率が悪い)
 */
class StretcherPool
{
public:
    StretcherPool() = default;
    ~StretcherPool();

    StretcherPool(const StretcherPool&) = delete;
    StretcherPool& operator=(const StretcherPool&) = delete;

    /**
     * ContentKey に対応する stretcher を取得する。
     * 存在しない場合、または sampleRate/channels が一致しない場合は再作成する。
     *
     * @return non-owning ポインタ。エントリが見つからない / 作成できない場合は nullptr。
     */
    SoundTouchStretcher* getOrCreate(ContentKey key, double sampleRate, int channels);

    /**
     * ContentKey に対応する stretcher エントリを削除する。
     */
    void remove(ContentKey key);

    /**
     * すべてのエントリを削除する。
     */
    void clear();

private:
    struct Entry
    {
        double sampleRate{0.0};
        int channels{0};
        std::unique_ptr<SoundTouchStretcher> stretcher;
    };

    juce::ReadWriteLock lock_;
    std::map<ContentKey, Entry> entries_;
};

} // namespace OpenTune

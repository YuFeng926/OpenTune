#include "ArrangementViewComponent.h"
#include "AuroraTheme.h"
#include "FrameScheduler.h"
#include "../Utils/ZoomSensitivityConfig.h"

namespace OpenTune {

ArrangementViewComponent::ArrangementViewComponent(OpenTuneAudioProcessor& processor)
    : processor_(processor)
{
    setWantsKeyboardFocus(true);
    timeConverter_.setContext(processor_.getBpm(), processor_.getTimeSigNumerator(), processor_.getTimeSigDenominator());
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);

    addAndMakeVisible(horizontalScrollBar_);
    addAndMakeVisible(verticalScrollBar_);
    horizontalScrollBar_.addListener(this);
    verticalScrollBar_.addListener(this);
    horizontalScrollBar_.setAutoHide(false);
    verticalScrollBar_.setAutoHide(false);

    scrollModeToggleButton_.setButtonText(scrollMode_ == ScrollMode::Continuous ? "Cont" : "Page");
    scrollModeToggleButton_.setLookAndFeel(&smallButtonLookAndFeel_);
    scrollModeToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    scrollModeToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    scrollModeToggleButton_.onClick = [this] {
        if (scrollMode_ == ScrollMode::Page) {
            scrollMode_ = ScrollMode::Continuous;
            scrollModeToggleButton_.setButtonText("Cont");
        } else {
            scrollMode_ = ScrollMode::Page;
            scrollModeToggleButton_.setButtonText("Page");
        }
        updateAutoScroll();
    };
    scrollModeToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    scrollModeToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    addAndMakeVisible(scrollModeToggleButton_);

    timeUnitToggleButton_.setButtonText("Time");
    timeUnitToggleButton_.setLookAndFeel(&smallButtonLookAndFeel_);
    timeUnitToggleButton_.onClick = [this] {
        if (timeUnit_ == TimeUnit::Seconds) {
            timeUnit_ = TimeUnit::Bars;
            timeUnitToggleButton_.setButtonText("BPM");
        } else {
            timeUnit_ = TimeUnit::Seconds;
            timeUnitToggleButton_.setButtonText("Time");
        }
        repaint();
    };
    timeUnitToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    timeUnitToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    addAndMakeVisible(timeUnitToggleButton_);

    addAndMakeVisible(playheadOverlay_);
    playheadOverlay_.setPianoKeyWidth(8);
    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });
}

ArrangementViewComponent::~ArrangementViewComponent()
{
    scrollVBlankAttachment_.reset();
    scrollModeToggleButton_.setLookAndFeel(nullptr);
    timeUnitToggleButton_.setLookAndFeel(nullptr);
    stopTimer();
    horizontalScrollBar_.removeListener(this);
    verticalScrollBar_.removeListener(this);
}

void ArrangementViewComponent::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void ArrangementViewComponent::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void ArrangementViewComponent::setZoomLevel(double zoom)
{
    // 限制缩放范围：0.02~10.0（支持更长音频的完整显示）
    zoomLevel_ = juce::jlimit(0.02, 10.0, zoom);
    timeConverter_.setZoom(zoomLevel_);
    // 同步缩放级别到高性能播放头覆盖层
    playheadOverlay_.setZoomLevel(zoomLevel_);
    updateScrollBars();
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
}

void ArrangementViewComponent::setScrollOffset(int pixels)
{
    const int newOffset = juce::jmax(0, pixels);
    if (newOffset == scrollOffset_)
        return;

    scrollOffset_ = newOffset;
    timeConverter_.setScrollOffset(scrollOffset_);
    horizontalScrollBar_.setCurrentRangeStart(scrollOffset_);
    // 同步滚动偏移到高性能播放头覆盖层
    playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
}

void ArrangementViewComponent::setVerticalScrollOffset(int offset)
{
    // 计算最大滚动偏移（12轨道高度 + ruler高度 - 可见高度）
    const int totalContentHeight = rulerHeight_ + OpenTuneAudioProcessor::MAX_TRACKS * processor_.getTrackHeight();
    const int visibleHeight = getHeight();
    const int maxScrollOffset = juce::jmax(0, totalContentHeight - visibleHeight);
    
    // 限制滚动范围 [0, maxScrollOffset]
    verticalScrollOffset_ = juce::jlimit(0, maxScrollOffset, offset);
    verticalScrollBar_.setCurrentRangeStart(verticalScrollOffset_);
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
}

void ArrangementViewComponent::fitToContent()
{
    // 如果用户已手动调整过缩放，不自动覆盖
    if (userHasManuallyZoomed_) {
        return;
    }

    double maxEndTime = 0.0;
    // 音频存储采样率为固定 44.1kHz，用于计算音频时长
    constexpr double storedSampleRate = 44100.0;

    for (int t = 0; t < OpenTuneAudioProcessor::MAX_TRACKS; ++t)
    {
        int numClips = processor_.getNumClips(t);
        for (int i = 0; i < numClips; ++i)
        {
            double start = processor_.getClipStartSeconds(t, i);
            const auto* audio = processor_.getClipAudioBuffer(t, i);
            double dur = 0.0;
            if (audio) dur = (double)audio->getNumSamples() / storedSampleRate;
            maxEndTime = juce::jmax(maxEndTime, start + dur);
        }
    }

    if (maxEndTime <= 0.0 || getWidth() <= 8) {
        return;
    }

    int viewWidth = getWidth() - 8;
    int paddingPx = 12;
    int drawableWidth = juce::jmax(1, viewWidth - paddingPx);
    double zoom = (static_cast<double>(drawableWidth) / maxEndTime) / 100.0;
    // 限制缩放范围：0.02~10.0（支持自动缩放到更长音频）
    zoom = juce::jlimit(0.02, 10.0, zoom);
    setZoomLevel(zoom);
    setScrollOffset(0);
}

void ArrangementViewComponent::updateMeter(int trackId, float rmsDb)
{
    juce::ignoreUnused(trackId, rmsDb);
}

void ArrangementViewComponent::prioritizeWaveformBuildForClip(int trackId, uint64_t clipId)
{
    juce::ignoreUnused(trackId);
    prioritizedWaveformKey_ = clipId;
}

bool ArrangementViewComponent::isWaveformCacheCompleteForClip(int trackId, uint64_t clipId) const
{
    juce::ignoreUnused(trackId);
    if (clipId == 0)
        return false;

    const auto it = waveformCaches_.find(clipId);
    if (it == waveformCaches_.end())
        return false;

    return it->second.complete;
}

void ArrangementViewComponent::resized()
{
    auto bounds = getLocalBounds();
    horizontalScrollBar_.setBounds(bounds.removeFromBottom(15));
    verticalScrollBar_.setBounds(bounds.removeFromLeft(15));

    // Position toggle buttons in top right of ruler
    int btnW = 50;
    int btnH = 20;
    int spacing = 5;
    int currentX = getWidth() - spacing - btnW;
    
    scrollModeToggleButton_.setBounds(currentX, 5, btnW, btnH);
    currentX -= (btnW + spacing);
    timeUnitToggleButton_.setBounds(currentX, 5, btnW, btnH);

    updateScrollBars();

    // 播放头覆盖层覆盖整个组件区域
    playheadOverlay_.setBounds(getLocalBounds());
}

void ArrangementViewComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &horizontalScrollBar_)
    {
        setScrollOffset(static_cast<int>(newRangeStart));
        smoothScrollCurrent_ = (float)newRangeStart; // Sync for manual scroll
    }
    else if (scrollBar == &verticalScrollBar_)
    {
        verticalScrollOffset_ = static_cast<int>(newRangeStart);
        // 通知监听器垂直滚动偏移变化（用于同步TrackPanel）
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
    }
}

void ArrangementViewComponent::updateScrollBars()
{
    double maxEndTime = 60.0 * 5.0; // Default 5 minutes
    // [TIME-01] Audio is stored at fixed 44.1kHz
    constexpr double storedSampleRate = 44100.0;

    for (int t = 0; t < OpenTuneAudioProcessor::MAX_TRACKS; ++t)
    {
        int numClips = processor_.getNumClips(t);
        for (int i = 0; i < numClips; ++i)
        {
            double start = processor_.getClipStartSeconds(t, i);
            const auto* audio = processor_.getClipAudioBuffer(t, i);
            double dur = 0.0;
            if (audio) dur = (double)audio->getNumSamples() / storedSampleRate;
            maxEndTime = juce::jmax(maxEndTime, start + dur + 10.0);
        }
    }

    double pixelsPerSecond = 100.0 * zoomLevel_;
    int totalContentWidth = static_cast<int>(maxEndTime * pixelsPerSecond);
    int visibleWidth = getWidth() - 15;
    
    horizontalScrollBar_.setRangeLimits(0.0, totalContentWidth + visibleWidth);
    horizontalScrollBar_.setCurrentRange(scrollOffset_, visibleWidth);

    int totalTrackHeight = rulerHeight_ + OpenTuneAudioProcessor::MAX_TRACKS * processor_.getTrackHeight();
    int visibleHeight = getHeight() - 15;
    verticalScrollBar_.setRangeLimits(0.0, totalTrackHeight + visibleHeight);
    verticalScrollBar_.setCurrentRange(verticalScrollOffset_, visibleHeight);
}

int ArrangementViewComponent::timeToX(double seconds) const
{
    return timeConverter_.timeToPixel(seconds) + 8;
}

double ArrangementViewComponent::xToTime(int x) const
{
    return timeConverter_.pixelToTime(x - 8);
}

juce::Rectangle<int> ArrangementViewComponent::getTrackLaneBounds(int trackId) const
{
    auto bounds = getLocalBounds().withTrimmedTop(rulerHeight_);
    bounds.removeFromLeft(15); // Reserve space for vertical scrollbar
    bounds.removeFromBottom(15); // Reserve space for horizontal scrollbar
    int h = processor_.getTrackHeight();
    return bounds.withY(rulerHeight_ + trackId * h - verticalScrollOffset_).withHeight(h);
}

juce::Rectangle<int> ArrangementViewComponent::getClipBounds(int trackId, int clipIndex) const
{
    const auto* audio = processor_.getClipAudioBuffer(trackId, clipIndex);
    if (audio == nullptr)
        return {};

    double startSeconds = processor_.getClipStartSeconds(trackId, clipIndex);
    int numSamples = audio->getNumSamples();
    if (numSamples <= 0)
        return {};

    // [TIME-01] Audio is stored at fixed 44.1kHz - use stored sample rate for duration calculation
    constexpr double storedSampleRate = 44100.0;
    double durationSeconds = static_cast<double>(numSamples) / storedSampleRate;
    auto lane = getTrackLaneBounds(trackId).reduced(6, 8);

    int x1 = timeToX(startSeconds);
    int x2 = timeToX(startSeconds + durationSeconds);
    int w = juce::jmax(8, x2 - x1);

    return { x1, lane.getY(), w, lane.getHeight() };
}

ArrangementViewComponent::HitTestResult ArrangementViewComponent::hitTestClip(juce::Point<int> p) const
{
    HitTestResult r;
    if (p.y < rulerHeight_)
        return r;

    // Adjust for vertical scroll
    int adjustedY = p.y + verticalScrollOffset_;

    // Determine track ID based on dynamic track height
    int h = processor_.getTrackHeight();
    int trackId = (h > 0) ? (adjustedY - rulerHeight_) / h : -1;
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) return r;

    int numClips = processor_.getNumClips(trackId);
    for (int i = 0; i < numClips; ++i)
    {
        auto bounds = getClipBounds(trackId, i);
        if (bounds.isEmpty())
            continue;
        if (bounds.contains(p))
        {
            r.trackId = trackId;
            r.clipIndex = i;
            r.clipBounds = bounds;
            r.isTopEdge = (p.y - bounds.getY()) <= 6;
            return r;
        }
    }
    return r;
}

uint64_t ArrangementViewComponent::getClipCacheKey(int trackId, int clipIndex) const
{
    const uint64_t id = processor_.getClipId(trackId, clipIndex);
    if (id != 0)
        return id;

    return (static_cast<uint64_t>(static_cast<uint32_t>(trackId)) << 32) | static_cast<uint32_t>(clipIndex);
}

ArrangementViewComponent::WaveformCache& ArrangementViewComponent::getOrCreateWaveformCache(uint64_t key)
{
    auto it = waveformCaches_.find(key);
    if (it != waveformCaches_.end())
        return it->second;

    auto inserted = waveformCaches_.emplace(key, WaveformCache{});
    return inserted.first->second;
}

void ArrangementViewComponent::prepareWaveformCache(WaveformCache& cache, const juce::AudioBuffer<float>& audio, int width)
{
    if (width < 1)
        width = 1;

    const int numSamples = audio.getNumSamples();
    const int numChannels = audio.getNumChannels();

    // 只在音频数据或像素宽度变化时重建缓存
    if (cache.buffer != &audio || cache.numSamples != numSamples || cache.numChannels != numChannels || cache.width != width)
    {
        cache.buffer = &audio;
        cache.numSamples = numSamples;
        cache.numChannels = numChannels;
        cache.width = width;
        cache.peaks.assign(static_cast<std::size_t>(width), 0.0f);
        cache.nextX = 0;
        cache.complete = false;
    }
}

bool ArrangementViewComponent::buildWaveformCacheSlice(WaveformCache& cache, double timeBudgetMs)
{
    if (cache.complete)
        return false;
    if (cache.buffer == nullptr)
        return false;
    if (cache.numSamples <= 0 || cache.numChannels <= 0)
        return false;
    if (cache.width <= 0 || cache.peaks.size() != static_cast<std::size_t>(cache.width))
        return false;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool progressed = false;

    const int width = cache.width;
    const int numSamples = cache.numSamples;
    const int numChannels = cache.numChannels;
    const auto& audio = *cache.buffer;

    // 增量构建：每次从 nextX 开始处理
    while (cache.nextX < static_cast<std::size_t>(width))
    {
        const int x = static_cast<int>(cache.nextX);
        const int64_t start = (static_cast<int64_t>(x) * static_cast<int64_t>(numSamples)) / static_cast<int64_t>(width);
        const int64_t end = (static_cast<int64_t>(x + 1) * static_cast<int64_t>(numSamples)) / static_cast<int64_t>(width);

        float peak = 0.0f;
        if (end > start)
        {
            // 多声道取最大峰值
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* data = audio.getReadPointer(ch);
                
                // 使用 SIMD 加速峰值检测
                const int numSamplesToScan = static_cast<int>(end - start);
                const auto range = juce::FloatVectorOperations::findMinAndMax(
                    data + start, numSamplesToScan);
                
                const float channelPeak = juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd()));
                peak = juce::jmax(peak, channelPeak);
            }
        }

        cache.peaks[cache.nextX] = peak;
        ++cache.nextX;
        progressed = true;

        // 时间预算检查
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if ((nowMs - startMs) >= timeBudgetMs)
            break;
    }

    if (cache.nextX >= static_cast<std::size_t>(width))
        cache.complete = true;

    return progressed;
}

bool ArrangementViewComponent::buildWaveformCaches(double timeBudgetMs)
{
    if (timeBudgetMs <= 0.0)
        return false;

    std::unordered_set<uint64_t> alive;
    alive.reserve(static_cast<std::size_t>(OpenTuneAudioProcessor::MAX_TRACKS * 16));
    for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId)
    {
        const int numClips = processor_.getNumClips(trackId);
        for (int clipIndex = 0; clipIndex < numClips; ++clipIndex)
            alive.insert(getClipCacheKey(trackId, clipIndex));
    }

    pruneWaveformCaches(waveformCaches_, alive);

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool progressed = false;

    if (prioritizedWaveformKey_ != 0)
    {
        auto it = waveformCaches_.find(prioritizedWaveformKey_);
        if (it != waveformCaches_.end())
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            const double remain = timeBudgetMs - (nowMs - startMs);
            if (remain > 0.0 && buildWaveformCacheSlice(it->second, remain))
                progressed = true;
        }
    }

    for (auto& kv : waveformCaches_)
    {
        if (kv.first == prioritizedWaveformKey_)
            continue;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
            break;

        if (buildWaveformCacheSlice(kv.second, remain))
            progressed = true;
    }

    return progressed;
}

bool ArrangementViewComponent::pruneWaveformCaches(std::unordered_map<uint64_t, WaveformCache>& caches, const std::unordered_set<uint64_t>& alive)
{
    bool removed = false;
    for (auto it = caches.begin(); it != caches.end();)
    {
        if (alive.find(it->first) == alive.end())
        {
            it = caches.erase(it);
            removed = true;
            continue;
        }
        ++it;
    }

    return removed;
}

void ArrangementViewComponent::paint(juce::Graphics& g)
{
    const double bpm = processor_.getBpm();
    const int timeSigNum = processor_.getTimeSigNumerator();
    const int timeSigDenom = processor_.getTimeSigDenominator();
    if (lastContextBpm_ != bpm || lastContextTimeSigNum_ != timeSigNum || lastContextTimeSigDenom_ != timeSigDenom)
    {
        timeConverter_.setContext(bpm, timeSigNum, timeSigDenom);
        lastContextBpm_ = bpm;
        lastContextTimeSigNum_ = timeSigNum;
        lastContextTimeSigDenom_ = timeSigDenom;
    }
    timeConverter_.setZoom(zoomLevel_);
    
    if (processor_.isPlaying() && scrollMode_ == ScrollMode::Continuous)
        timeConverter_.setScrollOffset(static_cast<double>(smoothScrollCurrent_));
    else
        timeConverter_.setScrollOffset(static_cast<double>(scrollOffset_));

    const auto themeId = Theme::getActiveTheme();
    
    auto bounds = getLocalBounds().toFloat();
    
    if (themeId == ThemeId::DarkBlueGrey) {
        // Soothe 2 Spectrum Background Style
        // It has a specific gradient and grid look
        UIColors::fillSoothe2SpectrumBackground(g, bounds, 0.0f);
    } else {
        g.fillAll(UIColors::backgroundMedium);
    }

    drawGridLines(g);

    for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId)
    {
        int numClips = processor_.getNumClips(trackId);
        for (int clipIndex = 0; clipIndex < numClips; ++clipIndex)
        {
            auto clipBounds = getClipBounds(trackId, clipIndex);
            if (clipBounds.isEmpty())
                continue;

            bool isSelected = (trackId == selectedTrack_ && clipIndex == selectedClip_);

            auto clipArea = clipBounds.toFloat();
            if (themeId == ThemeId::DarkBlueGrey && isSelected)
            {
                juce::ColourGradient sel(juce::Colour { 0xFFF7F3EA }, clipArea.getX(), clipArea.getBottom(),
                                         juce::Colour { 0xFFBFE0EF }, clipArea.getX(), clipArea.getY(), false);
                g.setGradientFill(sel);
                g.fillRoundedRectangle(clipArea, 6.0f);
                g.setColour(UIColors::panelBorder.withAlpha(0.55f));
                g.drawRoundedRectangle(clipArea.reduced(0.5f), 6.0f, 1.0f);
            }
            else if (themeId == ThemeId::BlueBreeze)
            {
                // Blue Breeze Clip Style - Sky Blue Gradient
                juce::Colour topColor = isSelected ? juce::Colour(BlueBreeze::Colors::ClipSelectedTop) 
                                                   : juce::Colour(BlueBreeze::Colors::ClipGradientTop);
                juce::Colour bottomColor = isSelected ? juce::Colour(BlueBreeze::Colors::ClipSelectedBottom) 
                                                      : juce::Colour(BlueBreeze::Colors::ClipGradientBottom);
                
                juce::ColourGradient grad(topColor, clipArea.getX(), clipArea.getY(),
                                          bottomColor, clipArea.getX(), clipArea.getBottom(), false);
                g.setGradientFill(grad);
                g.fillRoundedRectangle(clipArea, 6.0f);
                
                g.setColour(juce::Colour(BlueBreeze::Colors::ClipBorder));
                g.drawRoundedRectangle(clipArea.reduced(0.5f), 6.0f, 1.0f);
            }
            else if (themeId == ThemeId::Aurora)
            {
                // Aurora主题：CLIP背景色跟随轨道面板颜色（使用相同的霓虹色系）
                // 与 TrackPanelComponent 的轨道颜色循环保持一致（6色循环）
                juce::Colour trackColor;
                switch(trackId % 6) {
                    case 0: trackColor = juce::Colour(Aurora::Colors::Cyan); break;
                    case 1: trackColor = juce::Colour(Aurora::Colors::Violet); break;
                    case 2: trackColor = juce::Colour(Aurora::Colors::NeonGreen); break;
                    case 3: trackColor = juce::Colour(Aurora::Colors::Magenta); break;
                    case 4: trackColor = juce::Colour(Aurora::Colors::ElectricBlue); break;
                    case 5: trackColor = juce::Colour(Aurora::Colors::Warning); break;
                }
                
                if (isSelected)
                {
                    // 选中状态：使用更深的颜色
                    g.setColour(trackColor.withAlpha(0.45f));
                    g.fillRoundedRectangle(clipArea, 6.0f);
                    // 选中边框使用霓虹蓝色
                    g.setColour(juce::Colour(Aurora::Colors::Cyan));
                    g.drawRoundedRectangle(clipArea.reduced(0.5f), 6.0f, 2.0f);
                }
                else
                {
                    // 正常状态：使用与轨道背景相同的颜色，透明度稍高使CLIP更明显
                    g.setColour(trackColor.withAlpha(0.30f));
                    g.fillRoundedRectangle(clipArea, 6.0f);
                    g.setColour(trackColor.withAlpha(0.6f));
                    g.drawRoundedRectangle(clipArea.reduced(0.5f), 6.0f, 1.0f);
                }
            }
            else
            {
                juce::Colour fill = isSelected ? UIColors::primaryPurple : UIColors::buttonNormal;
                g.setColour(fill);
                g.fillRoundedRectangle(clipArea, 6.0f);
                g.setColour(UIColors::panelBorder);
                g.drawRoundedRectangle(clipArea.reduced(0.5f), 6.0f, 1.0f);
            }

            const auto* audio = processor_.getClipAudioBuffer(trackId, clipIndex);
            float gain = processor_.getClipGain(trackId, clipIndex);

            if (audio != nullptr)
            {
                const uint64_t key = getClipCacheKey(trackId, clipIndex);
                auto& cache = getOrCreateWaveformCache(key);
                auto waveformBounds = clipBounds.reduced(6, 6);
                prepareWaveformCache(cache, *audio, waveformBounds.getWidth());

                // Aurora主题使用更亮的波形颜色，其他主题使用深灰色
                if (themeId == ThemeId::Aurora)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.85f));
                }
                else
                {
                    g.setColour(juce::Colour(0xFF3E4652).withAlpha(0.85f));
                }

                // 直接绘制已构建的峰值数据
                const float midY = static_cast<float>(waveformBounds.getCentreY());
                const float halfH = waveformBounds.getHeight() * 0.45f;
                const int x0 = waveformBounds.getX();
                const int built = static_cast<int>(juce::jmin<std::size_t>(cache.nextX, cache.peaks.size()));

                for (int x = 0; x < built; ++x)
                {
                    const float peak = cache.peaks[static_cast<std::size_t>(x)] * gain;
                    float displayHeight = peak * halfH * 2.0f;
                    
                    // 确保可见性：即使峰值很小也显示至少2像素
                    if (peak > 0.0001f)
                        displayHeight = juce::jmax(displayHeight, 2.0f);

                    const float y1 = midY - displayHeight * 0.5f;
                    const float y2 = midY + displayHeight * 0.5f;
                    g.drawLine(static_cast<float>(x0 + x), y1, static_cast<float>(x0 + x), y2, 1.0f);
                }
            }

            // 在CLIP左上角显示文件名（小字体）
            juce::String clipName = processor_.getClipName(trackId, clipIndex);
            if (clipName.isNotEmpty())
            {
                // 截断过长的文件名
                if (clipName.length() > 20)
                    clipName = clipName.substring(0, 17) + "...";
                
                g.setColour(UIColors::textPrimary.withAlpha(0.85f));
                g.setFont(UIColors::getUIFont(10.0f));  // 小字体
                g.drawText(clipName, clipBounds.reduced(6, 4), juce::Justification::topLeft);
            }

            // 在右上角显示增益值
            float db = 0.0f;
            if (gain > 0.0001f)
                db = 20.0f * std::log10(gain);
            else
                db = -100.0f;

            juce::String gainStr;
            if (db > -90.0f)
                gainStr = (db >= 0 ? "+" : "") + juce::String(db, 1) + " dB";
            else
                gainStr = "-inf dB";

            g.setColour(UIColors::textSecondary.withAlpha(0.9f));
            g.setFont(UIColors::getUIFont(11.0f));
            g.drawText(gainStr, clipBounds.reduced(6, 4), juce::Justification::topRight);
        }
    }

    drawTimeRuler(g);

}

#if JUCE_DEBUG
bool ArrangementViewComponent::runDebugSelfTest()
{
    juce::AudioBuffer<float> audio(2, 4096);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        float* w = audio.getWritePointer(ch);
        const float v = (ch == 0) ? 0.5f : -0.25f;
        for (int i = 0; i < audio.getNumSamples(); ++i)
            w[i] = v;
    }

    WaveformCache cache;
    prepareWaveformCache(cache, audio, 44100);
    if (cache.width <= 0)
        return false;
    if (cache.peaks.size() != static_cast<std::size_t>(cache.width))
        return false;

    int guard = 0;
    while (!cache.complete && guard < 10000)
    {
        if (!buildWaveformCacheSlice(cache, 0.25))
            break;
        ++guard;
    }

    if (!cache.complete)
        return false;
    if (cache.nextX != static_cast<std::size_t>(cache.width))
        return false;

    // 测试pruneWaveformCaches
    std::unordered_map<uint64_t, WaveformCache> caches;
    caches.emplace(1u, WaveformCache{});
    caches.emplace(2u, WaveformCache{});
    std::unordered_set<uint64_t> alive;
    alive.insert(2u);
    if (!pruneWaveformCaches(caches, alive))
        return false;
    if (caches.size() != 1)
        return false;
    if (caches.find(2u) == caches.end())
        return false;

    return true;
}
#endif

void ArrangementViewComponent::drawTimeRuler(juce::Graphics& g)
{
    const auto themeId = Theme::getActiveTheme();
    auto bounds = getLocalBounds();
    auto rulerArea = bounds.removeFromTop(rulerHeight_);
    
    g.setColour(UIColors::backgroundMedium);
    g.fillRect(rulerArea);

    g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::panelBorder.withAlpha(0.18f) : UIColors::panelBorder);
    g.drawLine(0.0f, static_cast<float>(rulerHeight_), static_cast<float>(getWidth()), static_cast<float>(rulerHeight_), 1.0f);

    double sr = processor_.getSampleRate();
    if (sr <= 0.0)
        sr = 44100.0;
    
    // Switch between Seconds and Bars based on timeUnit_
    if (timeUnit_ == TimeUnit::Bars)
    {
        double bpm = processor_.getBpm();
        if (bpm <= 0.0) bpm = 120.0;
        
        // Calculate pixels per beat
        double pixelsPerSecond = 100.0 * zoomLevel_;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;
        
        // Determine interval (in beats) based on density
        // We want at least ~40 pixels between labels
        double beatInterval = 1.0;
        if (pixelsPerBeat < 40.0) beatInterval = 4.0;       // Every measure
        if (pixelsPerBeat < 10.0) beatInterval = 8.0;       // Every 2 measures
        if (pixelsPerBeat < 5.0) beatInterval = 16.0;       // Every 4 measures
        if (pixelsPerBeat < 2.5) beatInterval = 32.0;       // Every 8 measures
        
        // Convert visible range to beats
        double startTime = xToTime(0);
        double endTime = xToTime(getWidth());
        
        int64_t startBeat = static_cast<int64_t>(startTime / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        // Align to interval
        startBeat = (startBeat / (int64_t)beatInterval) * (int64_t)beatInterval;
        
        int64_t endBeat = static_cast<int64_t>(endTime / secondsPerBeat) + 1;
        
        g.setFont(UIColors::getUIFont(13.0f));
        
        for (int64_t beat = startBeat; beat <= endBeat; beat += (int64_t)beatInterval)
        {
            double time = beat * secondsPerBeat;
            int pixelX = timeToX(time);
            
            // Draw tick
            g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::gridLine.withAlpha(0.10f) : UIColors::gridLine);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerHeight_ - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerHeight_), 1.0f);
            
            // Draw label (Bar:Beat) -> actually just Bar number usually for overview
            // Let's show Bar number (1-based)
            int64_t bar = (beat / 4) + 1;
            int64_t beatInBar = (beat % 4) + 1;
            
            juce::String label;
            if (beatInterval >= 4.0)
                label = juce::String(bar);
            else
                label = juce::String::formatted("%lld.%lld", (long long) bar, (long long) beatInBar);
            
            g.setColour(UIColors::textSecondary);
            g.drawText(label, pixelX - 20, 2, 40, rulerHeight_ - 12, juce::Justification::centred);
        }
    }
    else
    {
        // Seconds mode
        double pixelsPerSecond = 100.0 * zoomLevel_;
        // double secondsPerPixel = 1.0 / pixelsPerSecond;
        
        // Adaptive interval logic based on user request to omit small ticks when zoomed out
        // User wants: only show 5s, 10s etc when dense.
        // Let's check pixel spacing for labels.
        
        double markerInterval = 1.0;
        
        // If 1s takes less than 40px, switch to 5s
        if (pixelsPerSecond < 40.0) markerInterval = 5.0;
        
        // If 5s takes less than 40px (pixelsPerSecond < 8), switch to 10s
        if (pixelsPerSecond < 8.0) markerInterval = 10.0;
        
        // If 10s takes less than 40px (pixelsPerSecond < 4), switch to 30s or 60s
        if (pixelsPerSecond < 4.0) markerInterval = 30.0;
        
        // If 30s takes less than 40px (pixelsPerSecond < 1.33), switch to 60s
        if (pixelsPerSecond < 1.33) markerInterval = 60.0;

        double startTime = xToTime(0);
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;

        double endTime = xToTime(getWidth());

        g.setFont(UIColors::getUIFont(13.0f));
        for (double time = startTime; time < endTime; time += markerInterval) {
            int pixelX = timeToX(time);
            
            g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::gridLine.withAlpha(0.10f) : UIColors::gridLine);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerHeight_ - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerHeight_), 1.0f);

            int totalSecs = static_cast<int>(time);
            int mins = totalSecs / 60;
            int secs = totalSecs % 60;
            juce::String timeStr = juce::String::formatted("%d:%02d", mins, secs);

            g.setColour(UIColors::textSecondary);
            g.drawText(timeStr, pixelX - 20, 2, 40, rulerHeight_ - 12, juce::Justification::centred);
        }
    }
}

void ArrangementViewComponent::drawGridLines(juce::Graphics& g)
{
    const auto themeId = Theme::getActiveTheme();
    double sr = processor_.getSampleRate();
    if (sr <= 0.0)
        sr = 44100.0;
    
    // Switch between Seconds and Bars based on timeUnit_ (match drawTimeRuler logic)
    if (timeUnit_ == TimeUnit::Bars)
    {
        double bpm = processor_.getBpm();
        if (bpm <= 0.0) bpm = 120.0;
        
        double pixelsPerSecond = 100.0 * zoomLevel_;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;
        
        double beatInterval = 1.0;
        if (pixelsPerBeat < 40.0) beatInterval = 4.0;
        if (pixelsPerBeat < 10.0) beatInterval = 8.0;
        if (pixelsPerBeat < 5.0) beatInterval = 16.0;
        if (pixelsPerBeat < 2.5) beatInterval = 32.0;
        
        double startTime = xToTime(0);
        double endTime = xToTime(getWidth());
        
        int64_t startBeat = static_cast<int64_t>(startTime / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / (int64_t)beatInterval) * (int64_t)beatInterval;
        
        int64_t endBeat = static_cast<int64_t>(endTime / secondsPerBeat) + 1;
        
        // Safety limit
        if (endBeat - startBeat > 2000) endBeat = startBeat + 2000;

        for (int64_t beat = startBeat; beat <= endBeat; beat += (int64_t)beatInterval)
        {
            double time = beat * secondsPerBeat;
            int pixelX = timeToX(time);
            
            // Only draw lines that are visible and within bounds
            // timeToX already handles scroll offset
            if (pixelX < -2 || pixelX > getWidth() + 2) continue; // Allow slight margin
            
            // Determine if this is a major measure line
            // If beatInterval >= 4 (measures), all are measure lines
            // If beatInterval < 4, only multiples of 4 are measures
            bool isMeasure = false;
            if (beatInterval >= 4.0) {
                isMeasure = true; 
            } else {
                isMeasure = (beat % 4) == 0;
            }

            if (themeId == ThemeId::DarkBlueGrey)
            {
                g.setColour(UIColors::panelBorder.withAlpha(0.12f));
                g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(getHeight()));
            }
            else
            {
                if (isMeasure) {
                    g.setColour(UIColors::panelBorder.brighter(0.3f));
                    g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(getHeight()));
                } else {
                    g.setColour(UIColors::panelBorder.withAlpha(0.25f));
                    g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(getHeight()));
                }
            }
        }
    }
    else
    {
        // Seconds mode
        double pixelsPerSecond = 100.0 * zoomLevel_;
        // double secondsPerPixel = 1.0 / pixelsPerSecond;
        
        double markerInterval = 1.0;
        if (pixelsPerSecond < 40.0) markerInterval = 5.0;
        if (pixelsPerSecond < 8.0) markerInterval = 10.0;
        if (pixelsPerSecond < 4.0) markerInterval = 30.0;
        if (pixelsPerSecond < 1.33) markerInterval = 60.0;

        double startTime = xToTime(0);
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;

        double endTime = xToTime(getWidth());

        // Safety limit: prevent infinite loop if markerInterval is somehow 0
        if (markerInterval < 0.001) markerInterval = 1.0;

        for (double time = startTime; time < endTime + markerInterval; time += markerInterval) {
            int pixelX = timeToX(time);
            
            if (pixelX < -2 || pixelX > getWidth() + 2) continue;

            g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::panelBorder.withAlpha(0.12f) : UIColors::panelBorder.withAlpha(0.25f));
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(getHeight()));
        }
    }
}

void ArrangementViewComponent::timerCallback()
{
    onHeartbeatTick();
}

void ArrangementViewComponent::onHeartbeatTick()
{
    if (!isShowing())
        return;

    const bool playingNow = processor_.isPlaying();

    // 同步播放状态到高性能播放头覆盖层
    playheadOverlay_.setPlaying(playingNow);

    // 推理活跃时主动降频：波形后台构建改为低频小预算，减少消息线程竞争。
    bool progressed = false;
    if (inferenceActive_)
    {
        waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
        if (waveformBuildTickCounter_ == 0)
            progressed = buildWaveformCaches(0.15);
    }
    else if (playingNow)
    {
        waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 3;
        if (waveformBuildTickCounter_ == 0)
            progressed = buildWaveformCaches(0.25);
    }
    else
    {
        waveformBuildTickCounter_ = 0;
        progressed = buildWaveformCaches(0.75);
    }

    if (!playingNow)
    {
        if (progressed)
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Background);
        return;
    }

}

void ArrangementViewComponent::updateAutoScroll()
{
    if (!isPlaying_.load(std::memory_order_relaxed))
        return;

    const double playheadTime = readPlayheadTime();

    if (scrollMode_ == ScrollMode::Page)
    {
        double pixelsPerSecond = 100.0 * zoomLevel_;
        int playheadVisualX = static_cast<int>(playheadTime * pixelsPerSecond) - scrollOffset_ + 8;

        if (playheadVisualX >= getWidth()) {
            int visibleW = getWidth() - 8;
            setScrollOffset(scrollOffset_ + visibleW);
            smoothScrollCurrent_ = static_cast<float>(scrollOffset_);
        } else if (playheadVisualX < 8) {
            int absX = static_cast<int>(playheadTime * pixelsPerSecond);
            
            int visibleW = getWidth() - 8;
            int pageIndex = absX / visibleW;
            int newScroll = pageIndex * visibleW;
            
            setScrollOffset(newScroll);
            smoothScrollCurrent_ = static_cast<float>(newScroll);
        }
    }
}

void ArrangementViewComponent::onScrollVBlankCallback(double timestampSec)
{
    juce::ignoreUnused(timestampSec);

    if (!isShowing() || !isPlaying_.load(std::memory_order_relaxed))
        return;

    const double playheadTime = readPlayheadTime();
    playheadOverlay_.setPlayheadSeconds(playheadTime);

    if (scrollMode_ == ScrollMode::Continuous)
    {
        const double pixelsPerSecond = 100.0 * zoomLevel_;
        const float playheadAbsX = static_cast<float>(playheadTime * pixelsPerSecond);

        float targetScroll = playheadAbsX + 8.0f - (getWidth() / 2.0f);
        if (targetScroll < 0.0f)
            targetScroll = 0.0f;

        const float diff = targetScroll - smoothScrollCurrent_;
        if (std::abs(diff) < 1.0f)
        {
            smoothScrollCurrent_ = targetScroll;
        }
        else
        {
            smoothScrollCurrent_ += diff * 0.1f;
        }

        const int newScrollInt = static_cast<int>(std::llround(smoothScrollCurrent_));
        if (newScrollInt != scrollOffset_)
        {
            setScrollOffset(newScrollInt);
        }

        playheadOverlay_.setScrollOffset(static_cast<double>(smoothScrollCurrent_));
        return;
    }

    if (scrollMode_ == ScrollMode::Page)
    {
        double pixelsPerSecond = 100.0 * zoomLevel_;
        int playheadVisualX = static_cast<int>(playheadTime * pixelsPerSecond) - scrollOffset_ + 8;

        if (playheadVisualX >= getWidth()) {
            int visibleW = getWidth() - 8;
            setScrollOffset(scrollOffset_ + visibleW);
            smoothScrollCurrent_ = static_cast<float>(scrollOffset_);
        } else if (playheadVisualX < 8) {
            int absX = static_cast<int>(playheadTime * pixelsPerSecond);

            int visibleW = getWidth() - 8;
            int pageIndex = absX / visibleW;
            int newScroll = pageIndex * visibleW;

            setScrollOffset(newScroll);
            smoothScrollCurrent_ = static_cast<float>(newScroll);
        }
    }
}

double ArrangementViewComponent::readPlayheadTime() const
{
    if (auto source = positionSource_.lock())
    {
        return source->load(std::memory_order_relaxed);
    }

    return processor_.getPosition();
}

void ArrangementViewComponent::mouseMove(const juce::MouseEvent& e)
{
    if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey))
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    if (e.y <= rulerHeight_)
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    auto hit = hitTestClip(e.getPosition());
    if (hit.trackId >= 0)
    {
        if (hit.isTopEdge)
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        else
            setMouseCursor(juce::MouseCursor::NormalCursor); // User requested standard arrow on hover
    }
    else
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void ArrangementViewComponent::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey))
    {
        isPanning_ = true;
        lastMousePos_ = e.getPosition();
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    double sr = processor_.getSampleRate();
    if (sr <= 0.0) sr = 44100.0;

    // Always update playhead position on click, even if on a clip
    double newPosSeconds = xToTime(e.x);
    processor_.setPosition(newPosSeconds);
    playheadOverlay_.setPlayheadSeconds(newPosSeconds);
    playheadOverlay_.repaint();
    repaint();

    if (e.y <= rulerHeight_)
    {
        isDraggingPlayhead_ = true;
        dragStartPos_ = e.getPosition();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    auto hit = hitTestClip(e.getPosition());
    if (hit.trackId < 0)
    {
        repaint();
        return;
    }

    selectedTrack_ = hit.trackId;
    selectedClip_ = hit.clipIndex;
    selectedClipId_ = processor_.getClipId(selectedTrack_, selectedClip_);

    processor_.setActiveTrack(selectedTrack_);
    processor_.setSelectedClip(selectedTrack_, selectedClip_);

    listeners_.call([this](Listener& l) {
        l.clipSelectionChanged(selectedTrack_, selectedClip_);
    });

    dragStartPos_ = e.getPosition();
    dragStartClipSeconds_ = processor_.getClipStartSeconds(selectedTrack_, selectedClip_);
    dragStartClipGain_ = processor_.getClipGain(selectedTrack_, selectedClip_);
    dragStartClipId_ = selectedClipId_;
    dragStartTrackId_ = selectedTrack_;  // 记录拖拽开始时的轨道ID

    isAdjustingGain_ = hit.isTopEdge;
    isDraggingClip_ = !isAdjustingGain_;
    
    repaint();
}

void ArrangementViewComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (isPanning_)
    {
        auto delta = e.getPosition() - lastMousePos_;
        
        // Horizontal Scroll
        setScrollOffset(scrollOffset_ - delta.x);
        
        // Vertical Scroll
        verticalScrollOffset_ -= delta.y;
        
        double maxScroll = verticalScrollBar_.getRangeLimit().getEnd() - verticalScrollBar_.getCurrentRangeSize();
        verticalScrollOffset_ = juce::jlimit(0, static_cast<int>(maxScroll), verticalScrollOffset_);
        verticalScrollBar_.setCurrentRangeStart(verticalScrollOffset_);
        
        // 通知监听器垂直滚动偏移变化（用于同步TrackPanel）
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
        
        lastMousePos_ = e.getPosition();
        repaint();
        return;
    }

    double sr = processor_.getSampleRate();
    if (sr <= 0.0) sr = 44100.0;

    if (isDraggingPlayhead_)
    {
        double newPosSeconds = xToTime(e.x);
        processor_.setPosition(newPosSeconds);
        playheadOverlay_.setPlayheadSeconds(newPosSeconds);
        playheadOverlay_.repaint();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (selectedTrack_ < 0 || selectedTrack_ >= OpenTuneAudioProcessor::MAX_TRACKS)
        return;

    int clipIndex = selectedClip_;
    if (selectedClipId_ != 0) {
        clipIndex = processor_.findClipIndexById(selectedTrack_, selectedClipId_);
    }
    if (clipIndex < 0 || clipIndex >= processor_.getNumClips(selectedTrack_))
        return;
    selectedClip_ = clipIndex;

    auto delta = e.getPosition() - dragStartPos_;
    if (isDraggingClip_)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);

        // Calculate time delta using xToTime for correct scaling and offset
        double startT = xToTime(dragStartPos_.x);
        double currentT = xToTime(e.x);
        double deltaSeconds = currentT - startT;
        
        if (selectedClipId_ != 0) {
            processor_.setClipStartSecondsById(selectedTrack_, selectedClipId_, dragStartClipSeconds_ + deltaSeconds);
        } else {
            processor_.setClipStartSeconds(selectedTrack_, selectedClip_, dragStartClipSeconds_ + deltaSeconds);
        }

        listeners_.call([this](Listener& l) {
            l.clipTimingChanged(selectedTrack_, selectedClip_);
        });

        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }
    else if (isAdjustingGain_)
    {
        double factor = std::pow(10.0, (-static_cast<double>(delta.y)) / 200.0);
        processor_.setClipGain(selectedTrack_, selectedClip_, static_cast<float>(dragStartClipGain_ * factor));

        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }
}

void ArrangementViewComponent::mouseUp(const juce::MouseEvent& e)
{
    if (isDraggingClip_ && dragStartClipId_ != 0) {
        // 计算鼠标移动距离，只有超过阈值才算拖拽（区分单击和拖拽）
        auto delta = e.getPosition() - dragStartPos_;
        const float dragThreshold = 5.0f;  // 5像素拖拽阈值
        bool isDraggedSignificantly = delta.getDistanceFromOrigin() > dragThreshold;
        
        // 计算鼠标所在轨道（需要加上verticalScrollOffset_！）
        int adjustedY = e.y + verticalScrollOffset_;
        int mouseTrackId = (adjustedY - rulerHeight_) / processor_.getTrackHeight();
        mouseTrackId = juce::jlimit(0, 2, mouseTrackId);
        
        double currentStart = processor_.getClipStartSecondsById(dragStartTrackId_, dragStartClipId_);
        
        // 只有真正拖拽时才允许跨轨道移动
        if (isDraggedSignificantly && mouseTrackId != dragStartTrackId_) {
            // 跨轨道移动
            if (processor_.moveClipToTrack(dragStartTrackId_, mouseTrackId, dragStartClipId_, currentStart)) {
                processor_.getUndoManager().addAction(std::make_unique<ClipCrossTrackMoveAction>(
                    processor_, dragStartTrackId_, mouseTrackId, dragStartClipId_, 
                    dragStartClipSeconds_, currentStart));
                selectedTrack_ = mouseTrackId;
                selectedClip_ = processor_.getSelectedClip(mouseTrackId);
                selectedClipId_ = dragStartClipId_;
                processor_.setActiveTrack(selectedTrack_);
                listeners_.call([this](Listener& l) {
                    l.clipSelectionChanged(selectedTrack_, selectedClip_);
                });
            }
        } else if (std::abs(currentStart - dragStartClipSeconds_) > 1e-9) {
            // 同轨道移动（水平拖拽）
            processor_.getUndoManager().addAction(std::make_unique<ClipMoveAction>(
                processor_, selectedTrack_, dragStartClipId_, dragStartClipSeconds_, currentStart));
        }
    }
    
    // 处理增益调整的 Undo
    if (isAdjustingGain_ && dragStartClipId_ != 0) {
        float currentGain = processor_.getClipGain(selectedTrack_, selectedClip_);
        if (std::abs(currentGain - dragStartClipGain_) > 0.001f) {
            processor_.getUndoManager().addAction(std::make_unique<ClipGainChangeAction>(
                processor_, selectedTrack_, dragStartClipId_, dragStartClipGain_, currentGain));
        }
    }
    
    isDraggingClip_ = false;
    isAdjustingGain_ = false;
    isDraggingPlayhead_ = false;
    isPanning_ = false;
    dragStartTrackId_ = -1;
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void ArrangementViewComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    auto hit = hitTestClip(e.getPosition());
    if (hit.trackId >= 0 && hit.clipIndex >= 0)
    {
        listeners_.call([&](Listener& l) {
            l.clipDoubleClicked(hit.trackId, hit.clipIndex);
        });
    }
}

void ArrangementViewComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto& settings = ZoomSensitivityConfig::getSettings();
    
    // Shift + Wheel = Vertical Zoom (Track Height) - 与TrackPanel同步
    if (e.mods.isShiftDown())
    {
        if (wheel.deltaY != 0.0f)
        {
            int currentHeight = processor_.getTrackHeight();
            int change = static_cast<int>(wheel.deltaY * settings.verticalZoomFactor * 150);
            change = (change == 0) ? ((wheel.deltaY > 0) ? 10 : -10) : change;
            
            int newHeight = juce::jlimit(70, 300, currentHeight + change);
            
            if (newHeight != currentHeight)
            {
                processor_.setTrackHeight(newHeight);
                listeners_.call([newHeight](Listener& l) { l.trackHeightChanged(newHeight); });
                repaint();
            }
        }
        return;
    }

    // Ctrl + Wheel = Horizontal Zoom at Pointer
    if (e.mods.isCtrlDown())
    {
        if (wheel.deltaY != 0.0f)
        {
            double zoomFactor = 1.0 + wheel.deltaY * settings.horizontalZoomFactor * 1.7;
            zoomFactor = juce::jlimit(0.5, 1.5, zoomFactor);
            double newZoom = juce::jlimit(0.02, 10.0, zoomLevel_ * zoomFactor);

            if (std::abs(newZoom - zoomLevel_) > 0.001)
            {
                double timeAtMouse = xToTime(e.x);

                setZoomLevel(newZoom);
                userHasManuallyZoomed_ = true;

                double pps = 100.0 * newZoom;
                int newOffset = static_cast<int>(timeAtMouse * pps) + 8 - e.x;
                setScrollOffset(newOffset);

                FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
            }
        }
        return;
    }

    // Alt + Wheel = Horizontal Scroll (Time)
    if (e.mods.isAltDown())
    {
        if (wheel.deltaY != 0.0f)
        {
            setScrollOffset(scrollOffset_ - static_cast<int>(wheel.deltaY * settings.scrollSpeed * 10.0f));
        }
        return;
    }

    // Default: Vertical Scroll (Tracks)
    if (wheel.deltaY != 0.0f)
    {
        int scrollDelta = static_cast<int>(wheel.deltaY * settings.scrollSpeed);
        int newOffset = verticalScrollOffset_ - scrollDelta;
        setVerticalScrollOffset(newOffset);
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
    }
    
    // Horizontal Scroll via Touchpad/Mouse Horizontal Wheel
    if (wheel.deltaX != 0.0f)
    {
        setScrollOffset(scrollOffset_ - static_cast<int>(wheel.deltaX * settings.scrollSpeed * 5.0f));
    }
}

bool ArrangementViewComponent::keyPressed(const juce::KeyPress& key)
{
    // Ctrl+Z/Y 不再在此处理，统一由 PluginEditor 处理全局 Undo/Redo
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) {
        processor_.performUndo();
        selectedClip_ = processor_.getSelectedClip(selectedTrack_);
        if (selectedClip_ >= 0 && selectedClip_ < processor_.getNumClips(selectedTrack_)) {
            selectedClipId_ = processor_.getClipId(selectedTrack_, selectedClip_);
        } else {
            selectedClipId_ = 0;
        }
        repaint();
        return true;
    }

    // Ctrl+Shift+Z: Redo
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        processor_.performRedo();
        selectedClip_ = processor_.getSelectedClip(selectedTrack_);
        if (selectedClip_ >= 0 && selectedClip_ < processor_.getNumClips(selectedTrack_)) {
            selectedClipId_ = processor_.getClipId(selectedTrack_, selectedClip_);
        } else {
            selectedClipId_ = 0;
        }
        repaint();
        return true;
    }

    if (key.isKeyCode(juce::KeyPress::spaceKey))
    {
        processor_.setPlaying(!processor_.isPlaying());
        return true;
    }

    if (key.isKeyCode(juce::KeyPress::backspaceKey) || key.isKeyCode(juce::KeyPress::deleteKey))
    {
        if (selectedTrack_ >= 0 && selectedTrack_ < OpenTuneAudioProcessor::MAX_TRACKS)
        {
            uint64_t clipId = selectedClipId_;
            if (clipId == 0 && selectedClip_ >= 0 && selectedClip_ < processor_.getNumClips(selectedTrack_)) {
                clipId = processor_.getClipId(selectedTrack_, selectedClip_);
            }

            OpenTuneAudioProcessor::ClipSnapshot snap;
            int deletedIndex = -1;
            if (clipId != 0 && processor_.deleteClipById(selectedTrack_, clipId, &snap, &deletedIndex))
            {
                ClipDeleteAction::ClipSnapshotData snapData;
                snapData.audioBuffer = std::move(snap.audioBuffer);
                snapData.startSeconds = snap.startSeconds;
                snapData.gain = snap.gain;
                snapData.fadeInDuration = snap.fadeInDuration;
                snapData.fadeOutDuration = snap.fadeOutDuration;
                snapData.name = snap.name;
                snapData.colour = snap.colour;
                snapData.pitchCurve = snap.pitchCurve;
                snapData.originalF0State = static_cast<int>(snap.originalF0State);
                snapData.detectedRootNote = static_cast<int>(snap.detectedKey.root);
                snapData.detectedScaleType = static_cast<int>(snap.detectedKey.scale);
                snapData.renderCache = snap.renderCache;

                processor_.getUndoManager().addAction(std::make_unique<ClipDeleteAction>(
                    processor_, selectedTrack_, clipId, deletedIndex, std::move(snapData)));
                selectedClip_ = processor_.getSelectedClip(selectedTrack_);
                if (selectedClip_ >= 0 && selectedClip_ < processor_.getNumClips(selectedTrack_)) {
                    selectedClipId_ = processor_.getClipId(selectedTrack_, selectedClip_);
                } else {
                    selectedClipId_ = 0;
                }
                repaint();
            }
        }
        return true;
    }

    if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')
    {
        if (selectedTrack_ < 0 || selectedTrack_ >= OpenTuneAudioProcessor::MAX_TRACKS)
            return true;

        if (selectedClip_ < 0 || selectedClip_ >= processor_.getNumClips(selectedTrack_))
            return true;

        int originalClipIndex = selectedClip_;
        uint64_t originalClipId = processor_.getClipId(selectedTrack_, selectedClip_);
        double originalDuration = 0.0;
        if (const auto* buf = processor_.getClipAudioBuffer(selectedTrack_, selectedClip_)) {
            originalDuration = static_cast<double>(buf->getNumSamples()) / OpenTuneAudioProcessor::getStoredAudioSampleRate();
        }
        
        double splitSeconds = processor_.getPosition();
        if (processor_.splitClipAtSeconds(selectedTrack_, selectedClip_, splitSeconds))
        {
            int newClipIndex = processor_.getSelectedClip(selectedTrack_);
            uint64_t newClipId = 0;
            if (newClipIndex >= 0 && newClipIndex < processor_.getNumClips(selectedTrack_)) {
                newClipId = processor_.getClipId(selectedTrack_, newClipIndex);
            }
            
            ClipSplitAction::SplitResult result;
            result.newClipId = newClipId;
            result.splitSeconds = splitSeconds;
            result.originalClipDuration = originalDuration;
            
            processor_.getUndoManager().addAction(std::make_unique<ClipSplitAction>(
                processor_, selectedTrack_, originalClipId, result, originalClipIndex, newClipIndex));
            
            selectedClip_ = newClipIndex;
            if (selectedClip_ >= 0 && selectedClip_ < processor_.getNumClips(selectedTrack_)) {
                selectedClipId_ = processor_.getClipId(selectedTrack_, selectedClip_);
            } else {
                selectedClipId_ = 0;
            }
            listeners_.call([this](Listener& l) {
                l.clipTimingChanged(selectedTrack_, selectedClip_);
            });
            repaint();
        }
        return true;
    }

    return false;
}



} // namespace OpenTune

#pragma once

#include "../Content/ContentKey.h"
#include "../DSP/ReferenceFeatures.h"
#include "../Utils/AppLogger.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>

namespace OpenTune {

class ReferenceAnalysisService {
public:
    struct AnalysisJobKey {
        ContentKey contentKey;
        int64_t  contentRevision{0};
        int64_t  analysisRevision{0};
    };

    using AnalysisFunc = std::function<ReferenceFeatureSet(
        const AnalysisJobKey& jobKey)>;
    using NotificationDispatcher = std::function<void(std::function<void()> task)>;

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void analysisCompleted(ContentKey key,
                                       const ReferenceFeatureSet& result) = 0;
        virtual void analysisFailed(ContentKey key,
                                    const juce::String& reason) = 0;
    };

    ReferenceAnalysisService();
    ~ReferenceAnalysisService();

    void setAnalysisFunc(AnalysisFunc func);
    void setNotificationDispatcher(NotificationDispatcher dispatcher);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void submitAnalysis(ContentKey key, int64_t contentRevision);

    void cancelAll();

private:
    void workerLoop();
    void notifyListenersCompleted(ContentKey key, const ReferenceFeatureSet& result);
    void notifyListenersFailed(ContentKey key, const juce::String& reason);

    AnalysisFunc analysisFunc_;
    NotificationDispatcher notificationDispatcher_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::map<ContentKey, AnalysisJobKey> pendingJobs_;

    std::optional<AnalysisJobKey> activeJob_;
    std::set<ContentKey> cancelledActiveJobs_;

    std::atomic<bool> running_{true};
    std::shared_ptr<std::atomic<bool>> aliveToken_{std::make_shared<std::atomic<bool>>(true)};
    std::thread workerThread_;

    juce::ListenerList<Listener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceAnalysisService)
};

} // namespace OpenTune

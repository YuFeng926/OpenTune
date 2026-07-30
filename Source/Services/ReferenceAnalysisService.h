#pragma once

#include "../Content/ContentKey.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>

namespace OpenTune {

class ReferenceAnalysisService {
public:
    struct AnalysisJobKey {
        ContentKey contentKey;
        int64_t  inputFingerprint{0};
        ReferenceFeatureProducer producer{ReferenceFeatureProducer::Unknown};

        // pendingJobs_ stores the latest job per ContentKey. Equality identifies
        // the exact active job so duplicate submissions do not restart analysis.
        bool operator==(const AnalysisJobKey& rhs) const noexcept
        {
            return contentKey == rhs.contentKey
                && inputFingerprint == rhs.inputFingerprint
                && producer == rhs.producer;
        }
    };

    using AnalysisFunc = std::function<ReferenceFeatureSet(
        const AnalysisJobKey& jobKey)>;
    using NotificationDispatcher = std::function<void(std::function<void()> task)>;

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void analysisFinished(ContentKey key,
                                      const ReferenceFeatureSet& result) = 0;
    };

    ReferenceAnalysisService();
    ~ReferenceAnalysisService();

    void setAnalysisFunc(AnalysisFunc func);
    void setNotificationDispatcher(NotificationDispatcher dispatcher);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void submitAnalysis(ContentKey key, int64_t inputFingerprint,
                        ReferenceFeatureProducer producer = ReferenceFeatureProducer::Unknown);

    void shutdown();

private:
    void workerLoop();

    AnalysisFunc analysisFunc_;
    NotificationDispatcher notificationDispatcher_;

    std::mutex mutex_;
    std::condition_variable cv_;

    std::map<ContentKey, AnalysisJobKey> pendingJobs_;

    std::optional<AnalysisJobKey> activeJob_;

    std::atomic<bool> running_{true};
    std::shared_ptr<std::atomic<bool>> aliveToken_{std::make_shared<std::atomic<bool>>(true)};
    std::thread workerThread_;

    juce::ListenerList<Listener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceAnalysisService)
};

} // namespace OpenTune

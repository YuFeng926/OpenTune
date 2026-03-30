#pragma once

namespace OpenTune::ZoomSensitivityConfig {

constexpr float kDefaultHorizontalZoomFactor = 0.35f;
constexpr float kMinHorizontalZoomFactor = 0.1f;
constexpr float kMaxHorizontalZoomFactor = 1.0f;

constexpr float kDefaultVerticalZoomFactor = 0.35f;
constexpr float kMinVerticalZoomFactor = 0.1f;
constexpr float kMaxVerticalZoomFactor = 1.0f;

constexpr float kDefaultScrollSpeed = 90.0f;
constexpr float kMinScrollSpeed = 30.0f;
constexpr float kMaxScrollSpeed = 300.0f;

struct ZoomSensitivitySettings {
    float horizontalZoomFactor = kDefaultHorizontalZoomFactor;
    float verticalZoomFactor = kDefaultVerticalZoomFactor;
    float scrollSpeed = kDefaultScrollSpeed;
    
    static ZoomSensitivitySettings getDefault() {
        return ZoomSensitivitySettings{};
    }
};

inline ZoomSensitivitySettings& getMutableSettings() {
    static ZoomSensitivitySettings instance;
    return instance;
}

inline const ZoomSensitivitySettings& getSettings() {
    return getMutableSettings();
}

inline void setSettings(const ZoomSensitivitySettings& settings) {
    getMutableSettings() = settings;
}

} // namespace OpenTune::ZoomSensitivityConfig
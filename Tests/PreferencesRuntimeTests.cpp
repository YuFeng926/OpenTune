#include <iostream>
#include <cassert>
#include <cmath>
#include <string>

#include "../Source/Utils/TuningConfig.h"
#include "../Source/Utils/PitchUtils.h"
#include "../Source/Utils/ZoomSensitivityConfig.h"

using namespace OpenTune;

// Test that configuration parameters are properly designed for runtime use

int main() {
    std::cout << "=== Preferences Runtime Tests ===" << std::endl;
    int passed = 0;
    int failed = 0;

    auto test = [&](const std::string& name, bool result) {
        if (result) {
            std::cout << "  [PASS] " << name << std::endl;
            ++passed;
        } else {
            std::cout << "  [FAIL] " << name << std::endl;
            ++failed;
        }
    };

    // Test 1: TuningConfig runtime update
    {
        std::cout << "\nTest 1: TuningConfig runtime update" << std::endl;
        
        // Default should be 440
        test("Default currentTuningHz == 440", 
             TuningConfig::currentTuningHz() == 440.0f);
        
        // Change to 432
        TuningConfig::currentTuningHz() = 432.0f;
        test("After set, currentTuningHz == 432", 
             TuningConfig::currentTuningHz() == 432.0f);
        
        // PitchUtils should reflect the change
        float midi69 = PitchUtils::midiToFreq(69.0f);
        test("midiToFreq(69) == 432 when tuned to 432", 
             std::abs(midi69 - 432.0f) < 0.01f);
        
        float freq432 = PitchUtils::freqToMidi(432.0f);
        test("freqToMidi(432) == 69 when tuned to 432", 
             std::abs(freq432 - 69.0f) < 0.01f);
        
        // Test with different tuning
        TuningConfig::currentTuningHz() = 415.0f;
        test("currentTuningHz can be set to 415", 
             TuningConfig::currentTuningHz() == 415.0f);
        
        float midi69_415 = PitchUtils::midiToFreq(69.0f);
        test("midiToFreq(69) == 415 when tuned to 415", 
             std::abs(midi69_415 - 415.0f) < 0.01f);
        
        // Restore
        TuningConfig::currentTuningHz() = 440.0f;
        test("Restored currentTuningHz == 440", 
             TuningConfig::currentTuningHz() == 440.0f);
    }

    // Test 2: ZoomSensitivityConfig constants are safe (used only for UI ranges)
    {
        std::cout << "\nTest 2: ZoomSensitivityConfig constants (UI ranges only)" << std::endl;
        
        test("kDefaultHorizontalZoomFactor == 0.35f", 
             ZoomSensitivityConfig::kDefaultHorizontalZoomFactor == 0.35f);
        test("kMinHorizontalZoomFactor == 0.1f", 
             ZoomSensitivityConfig::kMinHorizontalZoomFactor == 0.1f);
        test("kMaxHorizontalZoomFactor == 1.0f", 
             ZoomSensitivityConfig::kMaxHorizontalZoomFactor == 1.0f);
        
        test("kDefaultVerticalZoomFactor == 0.35f", 
             ZoomSensitivityConfig::kDefaultVerticalZoomFactor == 0.35f);
        test("kDefaultScrollSpeed == 90.0f", 
             ZoomSensitivityConfig::kDefaultScrollSpeed == 90.0f);
        
        // Settings struct works correctly
        auto settings = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
        test("getDefault().horizontalZoomFactor == 0.35f", 
             settings.horizontalZoomFactor == 0.35f);
        settings.horizontalZoomFactor = 0.5f;
        test("Settings can be modified at runtime", 
             settings.horizontalZoomFactor == 0.5f);
        test("Settings verticalZoomFactor unchanged", 
             settings.verticalZoomFactor == 0.35f);
    }

    // Test 3: TuningSettings struct
    {
        std::cout << "\nTest 3: TuningSettings struct" << std::endl;
        
        TuningConfig::TuningSettings settings;
        test("Default tuningHz == 440", 
             settings.tuningHz == 440.0f);
        
        settings.tuningHz = 432.0f;
        test("tuningHz can be modified to 432", 
             settings.tuningHz == 432.0f);
        
        auto defaultSettings = TuningConfig::TuningSettings::getDefault();
        test("getDefault() returns 440", 
             defaultSettings.tuningHz == 440.0f);
    }

    // Summary
    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    
    if (failed == 0) {
        std::cout << "\n=== All tests passed! ===" << std::endl;
    } else {
        std::cout << "\n=== SOME TESTS FAILED ===" << std::endl;
    }
    
    return failed;
}

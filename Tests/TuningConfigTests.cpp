#include <iostream>
#include <cassert>
#include <cmath>

#include "../Source/Utils/TuningConfig.h"
#include "../Source/Utils/PitchUtils.h"

using namespace OpenTune;

bool almostEqual(float a, float b, float epsilon = 0.01f) {
    return std::abs(a - b) < epsilon;
}

int main() {
    std::cout << "=== TuningConfig Tests ===" << std::endl;
    
    // Test 1: Verify default tuning frequency
    std::cout << "Test 1: Default tuning frequency = " << TuningConfig::kDefaultTuningHz << " Hz" << std::endl;
    assert(TuningConfig::kDefaultTuningHz == 440.0f);
    assert(TuningConfig::currentTuningHz() == 440.0f);
    std::cout << "  PASS" << std::endl;
    
    // Test 2: Verify min/max bounds
    std::cout << "Test 2: Min tuning = " << TuningConfig::kMinTuningHz << " Hz, Max tuning = " << TuningConfig::kMaxTuningHz << " Hz" << std::endl;
    assert(TuningConfig::kMinTuningHz == 400.0f);
    assert(TuningConfig::kMaxTuningHz == 500.0f);
    std::cout << "  PASS" << std::endl;
    
    // Test 3: Verify MIDI 69 = A4 = 440 Hz with default tuning
    std::cout << "Test 3: MIDI 69 (A4) with default tuning" << std::endl;
    float freq69 = PitchUtils::midiToFreq(69.0f);
    std::cout << "  midiToFreq(69) = " << freq69 << " Hz" << std::endl;
    assert(almostEqual(freq69, 440.0f));
    std::cout << "  PASS" << std::endl;
    
    // Test 4: Verify freqToMidi and midiToFreq are inverses
    std::cout << "Test 4: freqToMidi and midiToFreq inverse relationship" << std::endl;
    float testFreq = 440.0f;
    float midi = PitchUtils::freqToMidi(testFreq);
    float backToFreq = PitchUtils::midiToFreq(midi);
    std::cout << "  freqToMidi(" << testFreq << ") = " << midi << std::endl;
    std::cout << "  midiToFreq(" << midi << ") = " << backToFreq << " Hz" << std::endl;
    assert(almostEqual(testFreq, backToFreq));
    std::cout << "  PASS" << std::endl;
    
    // Test 5: Verify MIDI note calculations
    std::cout << "Test 5: MIDI note calculations" << std::endl;
    float c4 = PitchUtils::midiToFreq(60.0f);  // C4 = 261.63 Hz
    std::cout << "  MIDI 60 (C4) = " << c4 << " Hz" << std::endl;
    assert(almostEqual(c4, 261.63f, 0.1f));
    
    float a5 = PitchUtils::midiToFreq(81.0f);  // A5 = 880 Hz
    std::cout << "  MIDI 81 (A5) = " << a5 << " Hz" << std::endl;
    assert(almostEqual(a5, 880.0f));
    std::cout << "  PASS" << std::endl;
    
    // Test 6: Verify TuningSettings struct
    std::cout << "Test 6: TuningSettings struct" << std::endl;
    TuningConfig::TuningSettings settings;
    std::cout << "  Default settings.tuningHz = " << settings.tuningHz << " Hz" << std::endl;
    assert(settings.tuningHz == TuningConfig::kDefaultTuningHz);
    
    TuningConfig::TuningSettings defaultSettings = TuningConfig::TuningSettings::getDefault();
    std::cout << "  getDefault().tuningHz = " << defaultSettings.tuningHz << " Hz" << std::endl;
    assert(defaultSettings.tuningHz == TuningConfig::kDefaultTuningHz);
    std::cout << "  PASS" << std::endl;
    
    // Test 7: Verify runtime tuning frequency can be changed (432 Hz)
    std::cout << "Test 7: Runtime tuning to 432 Hz (A4 = 432 Hz)" << std::endl;
    TuningConfig::currentTuningHz() = 432.0f;
    std::cout << "  currentTuningHz() = " << TuningConfig::currentTuningHz() << " Hz" << std::endl;
    assert(almostEqual(TuningConfig::currentTuningHz(), 432.0f));
    
    // With 432 Hz tuning, MIDI 69 should now map to 432 Hz
    float freq69_432 = PitchUtils::midiToFreq(69.0f);
    std::cout << "  midiToFreq(69) = " << freq69_432 << " Hz (should be 432)" << std::endl;
    assert(almostEqual(freq69_432, 432.0f));
    
    // freqToMidi(432) should return 69
    float midi_432 = PitchUtils::freqToMidi(432.0f);
    std::cout << "  freqToMidi(432) = " << midi_432 << " (should be 69)" << std::endl;
    assert(almostEqual(midi_432, 69.0f));
    
    // A5 (81) with 432 Hz tuning should be 432 * 2 = 864 Hz
    float a5_432 = PitchUtils::midiToFreq(81.0f);
    std::cout << "  MIDI 81 (A5) = " << a5_432 << " Hz (should be 864)" << std::endl;
    assert(almostEqual(a5_432, 864.0f));
    std::cout << "  PASS" << std::endl;
    
    // Test 8: Restore default and verify
    std::cout << "Test 8: Restore default 440 Hz" << std::endl;
    TuningConfig::currentTuningHz() = 440.0f;
    float freq69_restored = PitchUtils::midiToFreq(69.0f);
    std::cout << "  midiToFreq(69) = " << freq69_restored << " Hz (should be 440)" << std::endl;
    assert(almostEqual(freq69_restored, 440.0f));
    std::cout << "  PASS" << std::endl;
    
    std::cout << "\n=== All tests passed! ===" << std::endl;
    return 0;
}

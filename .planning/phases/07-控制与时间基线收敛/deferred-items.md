# Deferred Items (07-02)

1. `cmake --build build --config Debug --target OpenTune_VST3` fails in `Source/PluginProcessor.cpp` with missing symbol `updateHostTransportSnapshot`.
   - Classification: out-of-scope pre-existing blocker (not introduced by 07-02 changes)
   - Impact: blocks full target build verification for Task 3 in this workspace snapshot.

2. `OpenTuneTests` link step fails due unresolved `OpenTuneAudioProcessor::getHostTransportSnapshot` referenced by existing tests.
   - Classification: out-of-scope pre-existing blocker unrelated to 07-02 mapping implementation.
   - Impact: unit test executable cannot be linked in current baseline.

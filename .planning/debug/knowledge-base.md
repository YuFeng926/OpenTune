# GSD Debug Knowledge Base

Resolved debug sessions. Used by `gsd-debugger` to surface known-pattern hypotheses at the start of new investigations.

---

## vst3-plugin-editor-isolation-regression — VST3 build loaded the Standalone editor shell
- **Date:** 2026-04-15
- **Error patterns:** VST3, plugin editor, standalone shell, UI isolation, editor factory, standalone interface, 插件界面, Standalone 界面
- **Root cause:** JUCE generated `OpenTune_SharedCode` with both `JucePlugin_Build_Standalone=1` and `JucePlugin_Build_VST3=1`. The project compiled both editor factories into shared code and chose between them with `#if JucePlugin_Build_Standalone`, so shared code always kept the Standalone factory and excluded the plugin factory; the VST3 wrapper only linked that shared library and therefore loaded the Standalone UI.
- **Fix:** Move Standalone editor/factory sources to `OpenTune_Standalone`, move plugin editor/factory sources to `OpenTune_VST3`, inject wrapper-specific build macros and `/utf-8 /bigobj` compile options there, correct the plugin factory namespace, add a test-only `createOpenTuneEditor` stub for unit-test linkage, and restore the missing Standalone debug member `diagnosticHeartbeatCounter_` so the wrapper can compile independently.
- **Files changed:** CMakeLists.txt, Source/Editor/EditorFactoryPlugin.cpp, Source/Standalone/PluginEditor.h, Tests/TestEditorFactoryStub.cpp
---

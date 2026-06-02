# External Integrations

**Analysis Date:** 2026-06-02

## APIs & External Services

**No external API integrations detected.** OpenTune is a fully offline desktop application.

Key evidence from `CMakeLists.txt`:
- `JUCE_USE_CURL=0` — HTTP/cURL explicitly disabled
- `JUCE_WEB_BROWSER=0` — Web browser component disabled
- `JUCE_REPORT_APP_USAGE=0` — Analytics/usage reporting disabled

No network calls, REST APIs, webhooks, or cloud services are used anywhere in the codebase. The application operates entirely on local audio files and locally-bundled AI models.

## Data Storage

**Databases:**
- No database — all data is file-based

**File Storage (Local Filesystem Only):**
- **Project files:** Custom format (saved/loaded via `Source/Utils/ProjectPersistence.cpp` / `Source/Utils/ProjectSession.cpp`)
- **AI model files:** `.onnx` format, bundled locally at `models/` and `pc_nsf_hifigan_44.1k_ONNX/`
  - Git LFS managed (`.gitattributes`: `*.onnx filter=lfs`)
- **Audio import:** WAV, FLAC, OGG, MP3 (via JUCE audio format readers)
- **Audio samples:** Piano samples for keyboard audition (`Resources/PianoSamples-mp3/`), embedded as binary data via `juce_add_binary_data()`
- **Font:** HONORSansCN-Medium.ttf, embedded as binary data
- **UI theme assets:** PNG files in `Resources/UI/assets/`, embedded as binary data
- **User guide:** HTML files in `docs/` (multi-language: zh, en, ja, ru, es)

**Caching:**
- **Render cache:** `Source/Inference/RenderCache.cpp` — in-memory cache for AI inference results
- **Time-stretch cache:** `Source/Inference/TimeStretchCache.cpp` — in-memory cache for SoundTouch output
- **Piano roll render model cache:** `Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.cpp` — in-memory UI cache
- **Arrangement render model cache:** `Source/Standalone/UI/ArrangementRenderModelCache.cpp` — in-memory UI cache
- **Waveform mipmap:** `Source/Standalone/UI/WaveformMipmap.cpp` — precomputed waveform levels
- All caches are in-process, in-memory only. No Redis, Memcached, or disk-based cache.

## Authentication & Identity

**Auth Provider:**
- None required — desktop application with no user accounts or login

## Audio Plugin Host Integration

**VST3 Plugin Interface:**
- **Standard:** Steinberg VST3 (Audio Unit / VST3 SDK via JUCE)
- **ARA2 Extension:** Celemony ARA SDK v2.2.0 (optional, `OPENTUNE_ENABLE_ARA=ON`)
  - Supported DAWs: Studio One (native), Logic Pro (native), and other ARA2-compatible hosts
  - Implementation: `Source/ARA/` — Document controller, playback renderer, session management
  - Non-ARA VST3 capture workflow: `Source/Plugin/Capture/` — ring buffer, segment, compactor, session, persistence

**Audio I/O (Standalone):**
- **Windows:** ASIO (`JUCE_ASIO=1`) + Windows Audio
- **macOS:** CoreAudio (native)
- **Microphone access:** Required for real-time monitoring (`NSMicrophoneUsageDescription` in Info.plist)

## Monitoring & Observability

**Error Tracking:**
- **None** — no external crash reporting (Sentry, Crashlytics, etc.)
- **Internal only:** `Source/Utils/AppLogger.cpp` / `Source/Utils/AppLogger.h` — local logging utility
- `Source/Utils/ChannelLayoutLogger.cpp` — audio channel configuration logging
- `Source/Standalone/UI/TimelineRenderingDiagnostics.cpp` — runtime performance diagnostics overlay

**Logs:**
- In-process logging via `AppLogger` utility
- No structured logging framework, no log shipping to external services
- No telemetry or analytics

## CI/CD & Deployment

**Hosting:**
- No cloud hosting — desktop application distributed as downloadable installers
- **GitHub Releases:** Binary distribution via `https://github.com/YuFeng926/OpenTune/releases`

**CI Pipeline:**
- **None detected** — no `.github/workflows/`, no CI configuration files
- Manual builds via CMake + Visual Studio/Xcode

**Packaging:**
- **Windows:** Inno Setup 6 (`Installer/OpenTune_Installer.iss`) → `.exe` installer
  - LZMA2/ultra64 compression, admin privileges, multi-language
  - Output: `dist/OpenTune-{version}-Windows-x64-Setup.exe`
- **macOS:** Custom Bash script (`scripts/sign-and-package.sh`) → `.dmg` disk image
  - Ad-hoc or Developer ID signing with Hardened Runtime
  - Optional Apple notarization via `xcrun notarytool`
  - Includes `scripts/install.command` for one-click installation
  - Output: `OpenTune-{version}-macOS-arm64.dmg`

## Environment Configuration

**Required env vars (macOS signing/notarization only):**
- `APPLE_ID` — Apple ID email for notarization
- `APPLE_TEAM_ID` — Team identifier
- `APPLE_APP_PASSWORD` — App-specific password for notarization

**No runtime environment variables required** — the application is self-contained with all models and DLLs bundled locally. AI model paths are resolved via `Source/Utils/ModelPathResolver.h` which searches:
1. Application executable directory
2. Common app data directory (`C:\ProgramData\OpenTune\Models` on Windows)

**Secrets location:**
- No secrets management — no API keys, tokens, or credentials used in the application
- macOS signing credentials are passed as environment variables to the packaging script only

## Webhooks & Callbacks

**Incoming:**
- None — desktop application with no server component

**Outgoing:**
- None — all network functionality is disabled at compile time

## Version Control & Collaboration

**Repository:**
- Git hosted on GitHub: `https://github.com/YuFeng926/OpenTune`
- License: AGPL v3.0

**Git LFS:**
- `.onnx` model files tracked via Git LFS (`.gitattributes`: `*.onnx filter=lfs diff=lfs merge=lfs -text`)

---

*Integration audit: 2026-06-02*

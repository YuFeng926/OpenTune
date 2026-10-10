# Download and Installation

> For issues with release packages, see here; for building from source, see [BUILDING.en.md](BUILDING.en.md); for project introduction, see [README.en.md](README.en.md).

Please visit the [Releases](https://github.com/YuFeng926/OpenTune/releases) page to download the latest installation package.

## Windows
Extract the ZIP package and run `OpenTune.exe` directly. Keep the `models/`, `D3D12/`, and DLL files in the same directory as the executable.
Copy the `OpenTune.vst3` folder to `C:\Program Files\Common Files\VST3\` to load the plugin in your DAW.

## macOS
There is one **Universal2** release for Intel Macs, native Apple Silicon, and DAWs running under Rosetta on Apple Silicon. The recommended download is `OpenTune-<version>-macOS-Universal2.pkg`; double-click it and follow the installer. The Standalone is installed in `/Applications`, and the VST3 in the system-wide `/Library/Audio/Plug-Ins/VST3/` directory.

You can also download `OpenTune-<version>-macOS-Universal2.dmg`. Mount it and run **Install OpenTune.command** or **安装 OpenTune.command**; the script installs the Standalone in `/Applications` and the VST3 in the current user's `~/Library/Audio/Plug-Ins/VST3/` directory. The DMG also contains the `.app` and `.vst3` bundles for manual copying.

> ⚠️ The Standalone and VST3 bundles are ad-hoc signed; the PKG itself has no Developer ID Installer signature or Apple notarization. Gatekeeper may block installation or first launch. If macOS blocks an app, choose **Open Anyway** in System Settings → Privacy & Security, or remove the quarantine flag from the app/plugin:
>
> ```bash
> xattr -rd com.apple.quarantine /Applications/OpenTune.app
> xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
> xattr -rd com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
> ```

## Runtime File Structure (Windows)

```
OpenTune/
├── OpenTune.exe
├── OpenTuneOnnxRuntime_1_24_4.dll ← ONNX Runtime (built-in DirectML)
├── DirectML.dll             ← DirectML runtime
├── D3D12/
│   ├── D3D12Core.dll        ← DirectX Agility SDK
│   └── D3D12SDKLayers.dll
├── models/
│   ├── fcpe.onnx            ← Pitch extraction model (FCPE)
│   └── hifigan.onnx         ← Vocoder model
└── docs/
    └── UserGuide.html
```

> The layout above is the ZIP portable distribution (models next to the executable).
> When installed with the installer, models live in `%ProgramData%\OpenTune\Models`
> and are shared by the Standalone and the VST3/ARA plugin; everything else is the
> same. A custom install directory does not affect model resolution (the resolver
> looks in the module directory first and falls back to the shared locations).

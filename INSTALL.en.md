# Download and Installation

> For issues with release packages, see here; for building from source, see [BUILDING.en.md](BUILDING.en.md); for project introduction, see [README.en.md](README.en.md).

Please visit the [Releases](https://github.com/YuFeng926/OpenTune/releases) page to download the latest installation package.

## Windows
Extract the ZIP package and run `OpenTune.exe` directly. Keep the `models/`, `D3D12/`, and DLL files in the same directory as the executable.
Copy the `OpenTune.vst3` folder to `C:\Program Files\Common Files\VST3\` to load the plugin in your DAW.

## macOS
1. Download the matching `OpenTune-<version>-macOS-Intel.dmg` or `OpenTune-<version>-macOS-Apple-Silicon.dmg` for your Mac architecture and double-click to mount.
2. Double-click **install.command** in the mounted disk window (if blocked by Gatekeeper on first run, go to "System Settings → Privacy & Security" and click *Open Anyway*; or right-click the script in the DMG window → *Open*).
3. The terminal will automatically execute:
    - Copy `OpenTune.app` to `/Applications/` and remove the quarantine attribute;
    - Ask whether to install VST3 as well. If agreed, it will request administrator password to install `OpenTune.vst3` to the system-level `/Library/Audio/Plug-Ins/VST3/` (globally visible in DAWs);
    - Ask whether to launch the Standalone immediately.
4. After installation is complete, you can eject and discard the DMG. To update, re-running the script will overwrite the old version.

> ⚠️ Release packages currently use ad-hoc signing (no Apple Notarization), so you may see a "Cannot verify developer" prompt on first launch. `install.command` automatically removes the quarantine flag. If you manually drag the `.app` bypassing the script, you need to run:
>
> ```bash
> xattr -rd com.apple.quarantine /Applications/OpenTune.app
> xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
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
│   ├── rmvpe.onnx           ← Pitch extraction model
│   └── hifigan.onnx         ← Vocoder model
└── docs/
    └── UserGuide.html
```

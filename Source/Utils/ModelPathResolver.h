#pragma once

/**
 * ModelPathResolver - 模型路径解析器
 * 
 * 负责解析 ONNX Runtime 动态库路径和模型目录路径。
 * 自带优先：模块所在目录（随应用分发的那一份）永远第一，共享安装目录只作兜底，
 * 避免旧安装静默遮蔽便携包/开发构建。
 * 搜索顺序：模块目录 > Resources > Program Files > ProgramData > 当前工作目录 > exe 目录
 */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#include <string>
#include <juce_core/juce_core.h>

namespace OpenTune {

class ModelPathResolver {
public:
    // 成功时 loadReport 填入实际加载的 DLL 完整路径；失败时填入各候选的失败详情。
    static bool ensureOnnxRuntimeLoaded(std::string* loadReport = nullptr) {
#if defined(_WIN32)
        if (const HMODULE alreadyLoaded = ::GetModuleHandleW(L"OpenTuneOnnxRuntime_1_24_4.dll")) {
            if (loadReport != nullptr) {
                *loadReport = getModuleFullPath(alreadyLoaded).toStdString();
            }
            return true;
        }

        const juce::File moduleFile = getCurrentModuleFile();
        const juce::File programFilesRoot = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
            .getChildFile("OpenTune");

        // 唯一 DLL 名 OpenTuneOnnxRuntime_1_24_4.dll：带版本后缀，绝不装载宿主的同名裸 DLL。
        // 模块目录优先：应用必须使用随自身分发的那一份；Program Files 共享安装只作兜底。
        const juce::File candidates[] = {
            moduleFile.getParentDirectory().getChildFile("OpenTuneOnnxRuntime_1_24_4.dll"),
            programFilesRoot.getChildFile("OpenTuneOnnxRuntime_1_24_4.dll")
        };

        std::string detail;
        for (const auto& candidate : candidates) {
            const auto path = candidate.getFullPathName().toStdString();
            if (!candidate.existsAsFile()) {
                detail += path + " missing; ";
                continue;
            }
            const auto handle = ::LoadLibraryExW(
                candidate.getFullPathName().toWideCharPointer(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH
            );
            if (handle != nullptr) {
                if (loadReport != nullptr) {
                    *loadReport = getModuleFullPath(handle).toStdString();
                }
                return true;
            }
            // GetLastError 必须在 LoadLibraryExW 之后立刻取，避免被后续调用覆盖
            const DWORD loadError = ::GetLastError();
            detail += path + " loadError=" + std::to_string(loadError) + "; ";
        }
        if (loadReport != nullptr) {
            *loadReport = detail;
        }
        return false;
#else
        return true;
#endif
    }

    static std::string getModelsDirectory() {
        const juce::File moduleFile = getCurrentModuleFile();

        // 自带优先：模块目录（Standalone 与 VST3 bundle 随包分发）> bundle Resources；
        // Program Files / ProgramData 共享安装只作兜底。
        juce::File modelsDir = moduleFile.getParentDirectory().getChildFile("models");
        if (modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        modelsDir = moduleFile.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("models");
        if (modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        const juce::File programFilesModelsDir = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
            .getChildFile("OpenTune")
            .getChildFile("models");
        if (programFilesModelsDir.isDirectory()) {
            return programFilesModelsDir.getFullPathName().toStdString();
        }

        const juce::File programDataModelsDir = juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
            .getChildFile("OpenTune")
            .getChildFile("models");
        if (programDataModelsDir.isDirectory()) {
            return programDataModelsDir.getFullPathName().toStdString();
        }

        modelsDir = juce::File::getCurrentWorkingDirectory().getChildFile("models");
        if (modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        modelsDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("models");
        if (modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        return "./models";
    }

private:
#if defined(_WIN32)
    static juce::String getModuleFullPath(HMODULE moduleHandle) {
        std::wstring path;
        path.resize(32768);
        const DWORD len = ::GetModuleFileNameW(moduleHandle, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0 || len >= path.size()) {
            return {};
        }
        path.resize(len);
        return juce::String(path.c_str());
    }
#endif

    static juce::File getCurrentModuleFile() {
#if defined(_WIN32)
        HMODULE moduleHandle = nullptr;
        const BOOL ok = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModelPathResolver::getModelsDirectory),
            &moduleHandle
        );

        if (ok != 0 && moduleHandle != nullptr) {
            const auto path = getModuleFullPath(moduleHandle);
            if (path.isNotEmpty()) {
                return juce::File(path);
            }
        }
#endif
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    }
};

} // namespace OpenTune

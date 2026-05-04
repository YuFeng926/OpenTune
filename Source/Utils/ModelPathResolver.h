#pragma once

/**
 * ModelPathResolver - 模型路径解析器
 * 
 * 负责解析 ONNX Runtime 动态库路径和模型目录路径。
 * 搜索顺序：程序目录 > Program Files > AppData > 注册表 > 当前工作目录
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
    static bool ensureOnnxRuntimeLoaded() {
#if defined(_WIN32)
        if (::GetModuleHandleW(L"onnxruntime.dll") != nullptr) {
            return true;
        }

        const juce::File moduleFile = getCurrentModuleFile();
        const juce::File programFilesRoot = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
            .getChildFile("OpenTune");
        const juce::File programDataRoot = juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
            .getChildFile("OpenTune");

        const juce::File candidates[] = {
            programFilesRoot.getChildFile("onnxruntime.dll"),
            programDataRoot.getChildFile("onnxruntime.dll"),
            moduleFile.getParentDirectory().getChildFile("onnxruntime.dll")
        };

        for (const auto& candidate : candidates) {
            if (!candidate.existsAsFile()) {
                continue;
            }
            const auto handle = ::LoadLibraryExW(
                candidate.getFullPathName().toWideCharPointer(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH
            );
            if (handle != nullptr) {
                return true;
            }
        }
        return false;
#else
        return true;
#endif
    }

    static std::string getModelsDirectory() {
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

        const juce::File moduleFile = getCurrentModuleFile();
        juce::File modelsDir = moduleFile.getParentDirectory().getChildFile("models");

        if (modelsDir.exists() && modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        modelsDir = moduleFile.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("models");

        if (modelsDir.exists() && modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        #ifdef _WIN32
        juce::String regPath = juce::WindowsRegistry::getValue(
            "HKEY_LOCAL_MACHINE\\Software\\MakediffVST\\OpenTune\\ModelsPath"
        );
        if (regPath.isNotEmpty()) {
            juce::File regModelsDir(regPath);
            if (regModelsDir.exists() && regModelsDir.isDirectory()) {
                return regPath.toStdString();
            }
        }
        #endif

        modelsDir = juce::File::getCurrentWorkingDirectory().getChildFile("models");
        if (modelsDir.exists() && modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        modelsDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("models");
        if (modelsDir.exists() && modelsDir.isDirectory()) {
            return modelsDir.getFullPathName().toStdString();
        }

        return "./models";
    }

private:
    static juce::File getCurrentModuleFile() {
#if defined(_WIN32)
        HMODULE moduleHandle = nullptr;
        const BOOL ok = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModelPathResolver::getModelsDirectory),
            &moduleHandle
        );

        if (ok != 0 && moduleHandle != nullptr) {
            std::wstring path;
            path.resize(32768);
            const DWORD len = GetModuleFileNameW(moduleHandle, path.data(), static_cast<DWORD>(path.size()));
            if (len > 0 && len < path.size()) {
                path.resize(len);
                return juce::File(juce::String(path.c_str()));
            }
        }
#endif
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    }
};

} // namespace OpenTune

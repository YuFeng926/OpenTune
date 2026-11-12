// Vst3ModulePin.cpp — VST3 模块生命周期挂点（进程寿命后端的加载期前提）
//
// 进程寿命后端要求推理单例（ProcessF0Runtime / ProcessRenderRuntime）在全部
// 插件实例卸载后仍存活，供后续实例直接复用 ORT Env / Session。
//
// Windows：宿主可在任意时刻 FreeLibrary 卸载 .vst3 模块，模块卸载会连同函数
// 内静态对象与 CRT 一并销毁，因此本 TU 的静态初始化器在加载期用
// GetModuleHandleExW(FROM_ADDRESS | PIN) 把模块 pin 到进程寿命。该调用只对
// 已加载模块设置 pin 标志，是官方文档支持在模块加载期执行的操作。禁止新增
// DllMain；本文件不创建任何线程。pin 结果由 openTuneVst3PinSucceeded() 暴露，
// JUCE 的 VST3 initModule 在返回 true 之前检查它。
//
// macOS/Linux/BSD：模块可以卸载，最后一次 bundleExit / ModuleExit 是同步
// pre-unload 点。JUCE patch 在该点调用 openTuneVst3Shutdown()，同步关闭进程
// 渲染运行时并 join worker；shutdownAndJoin 在返回前还会排空运行时已接受的
// 消息线程投递，保证模块卸载后不存在仍指向本模块的回调。Windows 的 shutdown
// helper 是 no-op，因为模块 pin 已把清理留给进程终止。
//
// 本文件只编译进 OpenTune_VST3 target（见 CMakeLists.txt）。Standalone 是
// 独立进程，进程寿命天然成立，不 pin。

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>

namespace OpenTune {
namespace {

// 锚点地址：本函数位于 OpenTune.vst3 模块内，FROM_ADDRESS 据此解析模块句柄。
void* vst3ModulePinAnchor()
{
    return nullptr;
}

bool g_vst3ModulePinSucceeded = false;

struct Vst3ModulePin
{
    Vst3ModulePin()
    {
        HMODULE module = nullptr;
        g_vst3ModulePinSucceeded =
            ::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&vst3ModulePinAnchor),
                &module) != FALSE;
    }
};

// 静态初始化器：模块加载时执行一次，此后模块在进程生命周期内永不被卸载。
// 构造函数调用外部 API 有副作用，编译器不会剔除该对象。
Vst3ModulePin g_vst3ModulePin;

} // namespace
} // namespace OpenTune

#endif // _WIN32

#if !defined(_WIN32)
#include "../Runtime/ProcessRenderRuntime.h"
#endif

#if defined(_WIN32)
extern "C" bool openTuneVst3PinSucceeded() noexcept;
#endif
extern "C" bool openTuneVst3Shutdown() noexcept;

#if defined(_WIN32)
extern "C" bool openTuneVst3PinSucceeded() noexcept
{
    return OpenTune::g_vst3ModulePinSucceeded;
}
#endif

extern "C" bool openTuneVst3Shutdown() noexcept
{
#if defined(_WIN32)
    return true;
#else
    OpenTune::ProcessRenderRuntime::getInstance().shutdownAndJoin();
    return true;
#endif
}

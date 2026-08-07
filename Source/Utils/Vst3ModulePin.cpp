// Vst3ModulePin.cpp — Windows VST3 模块常驻（进程寿命后端的加载期前提）
//
// 进程寿命后端要求推理单例（ProcessF0Runtime / ProcessRenderRuntime）在全部
// 插件实例卸载后仍存活，供后续实例直接复用 ORT Env / Session。Windows 宿主
// 可在任意时刻 FreeLibrary 卸载 .vst3 模块，模块卸载会连同函数内静态对象与
// CRT 一并销毁，因此必须把 OpenTune.vst3 模块 pin 到进程寿命。
//
// 唯一挂点是本 TU 的静态初始化器（DLL 加载期、CRT 初始化时执行）：
//   GetModuleHandleExW(FROM_ADDRESS | PIN) 只对已加载的模块设置 pin 标志，
//   不加载任何模块、不触碰 loader lock 之外的路径，是官方文档支持的在模块
//   加载期调用的操作。禁止新增 DllMain；本文件不创建任何线程。
//
// 本文件只编译进 OpenTune_VST3 target（见 CMakeLists.txt）。Standalone 是
// 独立进程，进程寿命天然成立，不 pin。

#if defined(_WIN32) && JucePlugin_Build_VST3

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

struct Vst3ModulePin
{
    Vst3ModulePin()
    {
        HMODULE module = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&vst3ModulePinAnchor),
            &module);
        (void)module;
    }
};

// 静态初始化器：模块加载时执行一次，此后模块在进程生命周期内永不被卸载。
// 构造函数调用外部 API 有副作用，编译器不会剔除该对象。
Vst3ModulePin g_vst3ModulePin;

} // namespace
} // namespace OpenTune

#endif // _WIN32 && JucePlugin_Build_VST3

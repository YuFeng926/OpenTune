#if defined(_WIN32)

#ifndef OPENTUNE_D3D12_AGILITY_SDK_VERSION
#define OPENTUNE_D3D12_AGILITY_SDK_VERSION 0
#endif

extern "C" {

__declspec(dllexport) extern const unsigned int D3D12SDKVersion = OPENTUNE_D3D12_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";

}

#endif

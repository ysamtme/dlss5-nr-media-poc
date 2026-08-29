#include <windows.h>
#include <d3d12.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

namespace {
using Init = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                           NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using Create = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                             const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                               const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using Release = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
}

// Volatile stores prevent a tail call: the NVIDIA wrapper identifies its caller
// from the return address and requires that module to be named nvngx.dll.
extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DLSSNR_CallInit(
    FARPROC target, unsigned long long app_id, const wchar_t* data_path, ID3D12Device* device,
    NVSDK_NGX_Version version, const NVSDK_NGX_Parameter* params) {
    volatile auto result = reinterpret_cast<Init>(target)(app_id, data_path, device, version, params);
    return result;
}

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DLSSNR_CallCreate(
    FARPROC target, ID3D12GraphicsCommandList* command_list, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle) {
    volatile auto result = reinterpret_cast<Create>(target)(command_list, feature, params, handle);
    return result;
}

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DLSSNR_CallEvaluate(
    FARPROC target, ID3D12GraphicsCommandList* command_list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback) {
    volatile auto result = reinterpret_cast<Evaluate>(target)(command_list, handle, params, callback);
    return result;
}

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DLSSNR_CallRelease(
    FARPROC target, NVSDK_NGX_Handle* handle) {
    volatile auto result = reinterpret_cast<Release>(target)(handle);
    return result;
}

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DLSSNR_CallShutdown(
    FARPROC target, ID3D12Device* device) {
    volatile auto result = reinterpret_cast<Shutdown>(target)(device);
    return result;
}


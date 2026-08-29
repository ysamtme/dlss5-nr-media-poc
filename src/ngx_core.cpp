#include "ngx_core.h"

#include <array>
#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

template <typename T>
T load_export(HMODULE module, const char* name) {
    auto proc = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!proc) throw std::runtime_error(std::format("Missing NGX core export: {}", name));
    return proc;
}

fs::path read_ngx_directory(const wchar_t* subkey) {
    std::array<wchar_t, 32768> directory{};
    DWORD bytes = static_cast<DWORD>(directory.size() * sizeof(wchar_t));
    const auto status = RegGetValueW(HKEY_LOCAL_MACHINE, subkey, L"NGXPath", RRF_RT_REG_SZ,
                                     nullptr, directory.data(), &bytes);
    return status == ERROR_SUCCESS ? fs::path(directory.data()) : fs::path{};
}

fs::path find_driver_ngx_core() {
    auto directory = read_ngx_directory(
        L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore");
    if (directory.empty()) {
        directory = read_ngx_directory(
            L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore");
    }
    if (directory.empty()) {
        throw std::runtime_error(
            "NVIDIA driver NGXPath is missing; install or update the NVIDIA display driver");
    }
    auto core = directory / L"_nvngx.dll";
    if (!fs::is_regular_file(core)) {
        throw std::runtime_error(std::format("NVIDIA driver NGX core was not found: {}",
                                             core.string()));
    }
    return core;
}

} // namespace

DriverNgxCore::DriverNgxCore() : module_path_(find_driver_ngx_core()) {
    HMODULE module = LoadLibraryExW(module_path_.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        throw std::runtime_error(std::format("Loading NVIDIA driver NGX core failed: Win32 {}",
                                             GetLastError()));
    }
    try {
        init_project_id_ =
            load_export<InitProjectId>(module, "NVSDK_NGX_D3D12_Init_ProjectID");
        allocate_parameters_ = load_export<AllocateParameters>(
            module, "NVSDK_NGX_D3D12_AllocateParameters");
        destroy_parameters_ = load_export<DestroyParameters>(
            module, "NVSDK_NGX_D3D12_DestroyParameters");
        get_capability_parameters_ = load_export<GetCapabilityParameters>(
            module, "NVSDK_NGX_D3D12_GetCapabilityParameters");
        create_feature_ =
            load_export<CreateFeature>(module, "NVSDK_NGX_D3D12_CreateFeature");
        release_feature_ =
            load_export<ReleaseFeature>(module, "NVSDK_NGX_D3D12_ReleaseFeature");
    } catch (...) {
        FreeLibrary(module);
        throw;
    }
    module_ = module;
    // ponytail: keep NGX core process-lifetime; Shutdown1 reproducibly hangs after this
    // hybrid feature-18 path. Add unload only when a public core supports clean shutdown.
}

NVSDK_NGX_Result DriverNgxCore::init_project_id(
    const char* project_id, NVSDK_NGX_EngineType engine_type, const char* engine_version,
    const wchar_t* data_path, ID3D12Device* device,
    const NVSDK_NGX_FeatureCommonInfo* feature_info, NVSDK_NGX_Version sdk_version) const {
    return init_project_id_(project_id, engine_type, engine_version, data_path, device,
                            sdk_version, feature_info);
}

NVSDK_NGX_Result DriverNgxCore::allocate_parameters(NVSDK_NGX_Parameter** params) const {
    return allocate_parameters_(params);
}

NVSDK_NGX_Result DriverNgxCore::destroy_parameters(NVSDK_NGX_Parameter* params) const {
    return destroy_parameters_(params);
}

NVSDK_NGX_Result DriverNgxCore::get_capability_parameters(NVSDK_NGX_Parameter** params) const {
    return get_capability_parameters_(params);
}

NVSDK_NGX_Result DriverNgxCore::create_feature(ID3D12GraphicsCommandList* command_list,
                                               NVSDK_NGX_Feature feature,
                                               const NVSDK_NGX_Parameter* params,
                                               NVSDK_NGX_Handle** handle) const {
    return create_feature_(command_list, feature, params, handle);
}

NVSDK_NGX_Result DriverNgxCore::release_feature(NVSDK_NGX_Handle* handle) const {
    return release_feature_(handle);
}

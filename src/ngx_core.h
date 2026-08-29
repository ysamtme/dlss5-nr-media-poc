#pragma once

#include <windows.h>
#include <d3d12.h>

#include <nvsdk_ngx.h>

#include <filesystem>

class DriverNgxCore {
public:
    DriverNgxCore();
    DriverNgxCore(const DriverNgxCore&) = delete;
    DriverNgxCore& operator=(const DriverNgxCore&) = delete;

    const std::filesystem::path& module_path() const { return module_path_; }

    NVSDK_NGX_Result init_project_id(const char* project_id, NVSDK_NGX_EngineType engine_type,
                                     const char* engine_version, const wchar_t* data_path,
                                     ID3D12Device* device,
                                     const NVSDK_NGX_FeatureCommonInfo* feature_info,
                                     NVSDK_NGX_Version sdk_version) const;
    NVSDK_NGX_Result allocate_parameters(NVSDK_NGX_Parameter** params) const;
    NVSDK_NGX_Result destroy_parameters(NVSDK_NGX_Parameter* params) const;
    NVSDK_NGX_Result get_capability_parameters(NVSDK_NGX_Parameter** params) const;
    NVSDK_NGX_Result create_feature(ID3D12GraphicsCommandList* command_list,
                                    NVSDK_NGX_Feature feature,
                                    const NVSDK_NGX_Parameter* params,
                                    NVSDK_NGX_Handle** handle) const;
    NVSDK_NGX_Result release_feature(NVSDK_NGX_Handle* handle) const;

private:
    using InitProjectId = NVSDK_NGX_Result(NVSDK_CONV*)(
        const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D12Device*,
        NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
    using AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
    using DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
    using GetCapabilityParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
    using CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
        ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
        NVSDK_NGX_Handle**);
    using ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

    HMODULE module_ = nullptr;
    std::filesystem::path module_path_;
    InitProjectId init_project_id_ = nullptr;
    AllocateParameters allocate_parameters_ = nullptr;
    DestroyParameters destroy_parameters_ = nullptr;
    GetCapabilityParameters get_capability_parameters_ = nullptr;
    CreateFeature create_feature_ = nullptr;
    ReleaseFeature release_feature_ = nullptr;
};

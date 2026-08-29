#include "gui_processor.h"
#include "ngx_core.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr auto kFeature = NVSDK_NGX_Feature_Reserved18;
constexpr char kProjectId[] = "53f803cc-a12f-4d69-90d5-19b7599cad19";
constexpr std::uint64_t kGenericCustomAppId = 0x0876232c;

void check_hr(HRESULT hr, std::string_view operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::format("{} failed: HRESULT 0x{:08X}", operation,
                                             static_cast<std::uint32_t>(hr)));
    }
}

std::string ngx_name(NVSDK_NGX_Result result) {
    switch (result) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_Fail: return "Fail";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
    default: return "Unknown";
    }
}

[[noreturn]] void throw_ngx(std::string_view operation, NVSDK_NGX_Result result) {
    throw std::runtime_error(std::format("{}: {} (0x{:08X})", operation, ngx_name(result),
                                         static_cast<std::uint32_t>(result)));
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f
                             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) {
    value = std::max(value, 0.0f);
    return value <= 0.0031308f ? value * 12.92f
                               : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

template <typename T>
T load_export(HMODULE module, const char* name) {
    auto proc = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!proc) throw std::runtime_error(std::format("Missing DLL export: {}", name));
    return proc;
}

struct D3D12Context {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    std::uint64_t fence_value = 0;

    D3D12Context(ID3D12Device* shared_device, ID3D12CommandQueue* shared_queue)
        : device(shared_device), queue(shared_queue) {
        if (!device || !queue) throw std::runtime_error("Live preview D3D12 device/queue is null");
        check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator)),
                 "Create processing command allocator");
        check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                            nullptr, IID_PPV_ARGS(&command_list)),
                 "Create processing command list");
        check_hr(command_list->Close(), "Close initial processing command list");
        check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                 "Create processing fence");
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) check_hr(HRESULT_FROM_WIN32(GetLastError()), "Create fence event");
    }

    ~D3D12Context() {
        if (fence_event) CloseHandle(fence_event);
    }

    void reset() {
        check_hr(allocator->Reset(), "Reset processing command allocator");
        check_hr(command_list->Reset(allocator.Get(), nullptr), "Reset processing command list");
    }

    void discard() { (void)command_list->Close(); }

    void execute_and_wait() {
        check_hr(command_list->Close(), "Close processing command list");
        ID3D12CommandList* lists[] = {command_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check_hr(queue->Signal(fence.Get(), ++fence_value), "Signal processing fence");
        if (fence->GetCompletedValue() < fence_value) {
            check_hr(fence->SetEventOnCompletion(fence_value, fence_event),
                     "Wait for processing fence");
            WaitForSingleObject(fence_event, INFINITE);
        }
        check_hr(device->GetDeviceRemovedReason(), "D3D12 processing execution");
    }
};

struct DirectNgx {
    using Init = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                               NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using Create = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                 const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
                                                   const NVSDK_NGX_Handle*,
                                                   const NVSDK_NGX_Parameter*,
                                                   PFN_NVSDK_NGX_ProgressCallback);
    using Release = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
    using Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

    HMODULE module = nullptr;
    Init init = nullptr;
    Create create = nullptr;
    Evaluate evaluate = nullptr;
    Release release = nullptr;
    Shutdown shutdown = nullptr;

    explicit DirectNgx(const fs::path& path)
        : module(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                         LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module) {
            throw std::runtime_error(std::format("Loading nvngx_dlssnr.dll failed: Win32 {}",
                                                 GetLastError()));
        }
        init = load_export<Init>(module, "NVSDK_NGX_D3D12_Init_Ext");
        create = load_export<Create>(module, "NVSDK_NGX_D3D12_CreateFeature");
        evaluate = load_export<Evaluate>(module, "NVSDK_NGX_D3D12_EvaluateFeature");
        release = load_export<Release>(module, "NVSDK_NGX_D3D12_ReleaseFeature");
        shutdown = load_export<Shutdown>(module, "NVSDK_NGX_D3D12_Shutdown1");
    }

    ~DirectNgx() { if (module) FreeLibrary(module); }
};

struct NgxCaller {
    using Init = NVSDK_NGX_Result(*)(FARPROC, unsigned long long, const wchar_t*, ID3D12Device*,
                                     NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using Create = NVSDK_NGX_Result(*)(FARPROC, ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                       const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using Evaluate = NVSDK_NGX_Result(*)(FARPROC, ID3D12GraphicsCommandList*,
                                         const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                         PFN_NVSDK_NGX_ProgressCallback);
    using Release = NVSDK_NGX_Result(*)(FARPROC, NVSDK_NGX_Handle*);
    using Shutdown = NVSDK_NGX_Result(*)(FARPROC, ID3D12Device*);

    HMODULE module = nullptr;
    Init init = nullptr;
    Create create = nullptr;
    Evaluate evaluate = nullptr;
    Release release = nullptr;
    Shutdown shutdown = nullptr;

    explicit NgxCaller(const fs::path& path)
        : module(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                         LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module) {
            throw std::runtime_error(std::format("Loading caller shim failed: Win32 {}",
                                                 GetLastError()));
        }
        init = load_export<Init>(module, "DLSSNR_CallInit");
        create = load_export<Create>(module, "DLSSNR_CallCreate");
        evaluate = load_export<Evaluate>(module, "DLSSNR_CallEvaluate");
        release = load_export<Release>(module, "DLSSNR_CallRelease");
        shutdown = load_export<Shutdown>(module, "DLSSNR_CallShutdown");
    }

    ~NgxCaller() { if (module) FreeLibrary(module); }
};

struct UploadedTexture {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 bytes = 0;
    unsigned width = 0;
    unsigned height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC texture_desc(unsigned width, unsigned height, DXGI_FORMAT format,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

UploadedTexture make_texture(D3D12Context& d3d, unsigned width, unsigned height,
                             DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                             const LiveImage* source, bool decode_srgb) {
    UploadedTexture image;
    image.width = width;
    image.height = height;
    image.format = format;
    const auto desc = texture_desc(width, height, format, flags);
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    check_hr(d3d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&image.texture)),
             "Create live texture");
    UINT64 row_bytes = 0;
    d3d.device->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, &image.rows,
                                      &row_bytes, &image.bytes);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = image.bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    check_hr(d3d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&image.upload)),
             "Create live texture upload");

    std::byte* mapped = nullptr;
    check_hr(image.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)),
             "Map live texture upload");
    std::fill_n(mapped, static_cast<std::size_t>(image.bytes), std::byte{});
    if (source) {
        const std::size_t expected = static_cast<std::size_t>(width) * height * 4;
        if (source->rgba.size() != expected) {
            image.upload->Unmap(0, nullptr);
            throw std::runtime_error("Live input RGBA8 size does not match its dimensions");
        }
        for (unsigned y = 0; y < height; ++y) {
            auto* row = mapped + y * image.footprint.Footprint.RowPitch;
            for (unsigned x = 0; x < width; ++x) {
                for (unsigned channel = 0; channel < 4; ++channel) {
                    const auto index = (static_cast<std::size_t>(y) * width + x) * 4 + channel;
                    float value = source->rgba[index] / 255.0f;
                    if (decode_srgb && channel != 3) value = srgb_to_linear(value);
                    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                        reinterpret_cast<std::uint8_t*>(row)[x * 4 + channel] =
                            static_cast<std::uint8_t>(std::lround(
                                std::clamp(value, 0.0f, 1.0f) * 255.0f));
                    } else {
                        reinterpret_cast<std::uint16_t*>(row)[x * 4 + channel] =
                            DirectX::PackedVector::XMConvertFloatToHalf(value);
                    }
                }
            }
        }
    }
    image.upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = image.upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = image.footprint;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = image.texture.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d3d.command_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    return image;
}

UploadedTexture make_output_texture(D3D12Context& d3d, unsigned width, unsigned height,
                                    DXGI_FORMAT format) {
    UploadedTexture image;
    image.width = width;
    image.height = height;
    image.format = format;
    const auto desc = texture_desc(width, height, format,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    check_hr(d3d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(&image.texture)),
             "Create live output texture");
    return image;
}

UploadedTexture make_constant_texture(D3D12Context& d3d, unsigned width, unsigned height,
                                      DXGI_FORMAT format, std::span<const std::byte> texel,
                                      bool zero_left_half = false) {
    UploadedTexture image;
    image.width = width;
    image.height = height;
    image.format = format;
    const auto desc = texture_desc(width, height, format);
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    check_hr(d3d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&image.texture)),
             "Create live constant texture");
    UINT64 row_bytes = 0;
    d3d.device->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, &image.rows,
                                      &row_bytes, &image.bytes);
    if (row_bytes != static_cast<UINT64>(width) * texel.size()) {
        throw std::runtime_error("Unexpected live constant texture texel size");
    }
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = image.bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    check_hr(d3d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&image.upload)),
             "Create live constant upload");
    std::byte* mapped = nullptr;
    check_hr(image.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)),
             "Map live constant upload");
    std::fill_n(mapped, static_cast<std::size_t>(image.bytes), std::byte{});
    for (unsigned y = 0; y < height; ++y) {
        auto* row = mapped + y * image.footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < width; ++x) {
            if (!zero_left_half || x >= width / 2) {
                std::copy(texel.begin(), texel.end(), row + x * texel.size());
            }
        }
    }
    image.upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = image.upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = image.footprint;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = image.texture.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d3d.command_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    return image;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

NVSDK_NGX_Result NVSDK_CONV compute_scaling_ratio(NVSDK_NGX_Parameter* params) {
    if (!params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    unsigned upscaling = 0;
    const auto result = params->Get("DLSSNR.Upscaling", &upscaling);
    params->Set("DLSSNR.ScalingRatio", NVSDK_NGX_SUCCEED(result) && upscaling ? 0.5f : 1.0f);
    return NVSDK_NGX_Result_Success;
}

void set_create_params(NVSDK_NGX_Parameter* params, unsigned width, unsigned height,
                       unsigned out_width, unsigned out_height, unsigned preset) {
    params->Set(NVSDK_NGX_Parameter_Width, width);
    params->Set(NVSDK_NGX_Parameter_Height, height);
    params->Set(NVSDK_NGX_Parameter_OutWidth, out_width);
    params->Set(NVSDK_NGX_Parameter_OutHeight, out_height);
    params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue, 0);
    params->Set("DLSSNR.Width", out_width);
    params->Set("DLSSNR.Height", out_height);
    params->Set("DLSSNR.InputWidth", width);
    params->Set("DLSSNR.InputHeight", height);
    params->Set("DLSSNR.OutputWidth", out_width);
    params->Set("DLSSNR.OutputHeight", out_height);
    params->Set("DLSSNR.Output.Width", out_width);
    params->Set("DLSSNR.Output.Height", out_height);
    params->Set("DLSSNR.Upscaling", out_width != width || out_height != height ? 1 : 0);
    params->Set("DLSSNR.ScalingRatio", static_cast<float>(width) / out_width);
    params->Set("DLSSNR.Scale", static_cast<float>(width) / out_width);
    params->Set("DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&compute_scaling_ratio));
    params->Set("DLSSNR.Hint.Render.Preset", static_cast<int>(preset));
    params->Set("DLSS.Feature.Create.Flags", 0);
}

struct PipelineKey {
    const LiveImage* input = nullptr;
    int preset = 0;
    int motion = 0;
    int depth = 0;
    int control_mask = 0;
    int control_mask_format = 0;
    float control_mask_value = 0;
    int transfer = 0;
    int gpu_format = 0;
    int output_scale = 1;

    bool operator==(const PipelineKey&) const = default;
};

PipelineKey pipeline_key(const std::shared_ptr<const LiveImage>& input,
                         const LiveSettings& settings) {
    return {input.get(), settings.preset, settings.motion, settings.depth,
            settings.control_mask, settings.control_mask_format,
            settings.control_mask_value, settings.transfer, settings.gpu_format,
            settings.output_scale};
}

struct Pipeline {
    PipelineKey key;
    std::shared_ptr<const LiveImage> input;
    NVSDK_NGX_Handle* handle = nullptr;
    UploadedTexture color;
    std::array<UploadedTexture, 2> outputs;
    std::optional<UploadedTexture> motion;
    std::optional<UploadedTexture> depth;
    std::optional<UploadedTexture> control_mask;
    std::array<bool, 2> evaluated{};
    unsigned next_slot = 0;
    std::uint64_t generation = 0;
};

struct Runtime {
    fs::path dll_directory;
    fs::path executable_directory;
    fs::path snippet;
    fs::path log_directory;
    D3D12Context d3d;
    DriverNgxCore core;
    NVSDK_NGX_Parameter* params = nullptr;
    std::unique_ptr<NgxCaller> caller;
    std::unique_ptr<DirectNgx> ngx;
    Pipeline pipeline;
    bool direct_initialized = false;
    std::uint64_t next_generation = 1;

    Runtime(fs::path dll_dir, fs::path exe_dir, ID3D12Device* device,
            ID3D12CommandQueue* queue)
        : dll_directory(std::move(dll_dir)), executable_directory(std::move(exe_dir)),
          snippet(fs::absolute(dll_directory / L"nvngx_dlssnr.dll")),
          log_directory(fs::absolute(dll_directory / L"logs")), d3d(device, queue) {
        try {
            if (!fs::is_regular_file(snippet)) {
                throw std::runtime_error("nvngx_dlssnr.dll was not found for live preview");
            }
            fs::create_directories(log_directory);
            const std::wstring search_path = fs::absolute(dll_directory).wstring();
            const wchar_t* search_paths[] = {search_path.c_str()};
            NVSDK_NGX_FeatureCommonInfo common{};
            common.PathListInfo = {search_paths, 1};
            auto result = core.init_project_id(
                kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1", log_directory.c_str(),
                d3d.device.Get(), &common, NVSDK_NGX_Version_API);
            if (NVSDK_NGX_FAILED(result)) throw_ngx("NGX core init", result);
            result = core.allocate_parameters(&params);
            if (NVSDK_NGX_FAILED(result)) throw_ngx("NGX parameter allocation", result);

            caller = std::make_unique<NgxCaller>(executable_directory / L"caller" / L"nvngx.dll");
            ngx = std::make_unique<DirectNgx>(snippet);
            result = caller->init(reinterpret_cast<FARPROC>(ngx->init), kGenericCustomAppId,
                                  log_directory.c_str(), d3d.device.Get(), NVSDK_NGX_Version_API,
                                  nullptr);
            if (NVSDK_NGX_FAILED(result)) throw_ngx("DLSSNR direct init", result);
            direct_initialized = true;
        } catch (...) {
            if (params) core.destroy_parameters(params);
            params = nullptr;
            throw;
        }
    }

    ~Runtime() {
        release_pipeline();
        if (params) core.destroy_parameters(params);
        if (direct_initialized && caller && ngx) {
            (void)caller->shutdown(reinterpret_cast<FARPROC>(ngx->shutdown), d3d.device.Get());
        }
        // NGX core Shutdown1 is intentionally skipped: the proven CLI path documents a
        // reproducible hang for this hybrid feature-18 parameter-map setup.
    }

    void release_pipeline() {
        if (pipeline.handle && caller && ngx) {
            (void)caller->release(reinterpret_cast<FARPROC>(ngx->release), pipeline.handle);
        }
        pipeline = {};
    }

    void prepare(const std::shared_ptr<const LiveImage>& input, const LiveSettings& settings) {
        const PipelineKey wanted = pipeline_key(input, settings);
        if (pipeline.handle && pipeline.key == wanted) return;
        release_pipeline();
        const unsigned out_width = input->width * static_cast<unsigned>(settings.output_scale);
        const unsigned out_height = input->height * static_cast<unsigned>(settings.output_scale);
        if (!input->width || !input->height || settings.output_scale < 1 ||
            out_width / static_cast<unsigned>(settings.output_scale) != input->width ||
            out_height / static_cast<unsigned>(settings.output_scale) != input->height ||
            out_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            out_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            throw std::runtime_error("Live preview dimensions exceed the D3D12 texture limit");
        }

        set_create_params(params, input->width, input->height, out_width, out_height,
                          static_cast<unsigned>(settings.preset));
        d3d.reset();
        NVSDK_NGX_Handle* handle = nullptr;
        const auto create_result = caller->create(reinterpret_cast<FARPROC>(ngx->create),
                                                   d3d.command_list.Get(), kFeature, params,
                                                   &handle);
        if (NVSDK_NGX_FAILED(create_result)) {
            d3d.discard();
            throw_ngx("DLSSNR live feature creation", create_result);
        }
        d3d.execute_and_wait();

        pipeline.key = wanted;
        pipeline.input = input;
        pipeline.handle = handle;
        pipeline.generation = next_generation++;
        try {
        d3d.reset();
        const auto rgba_format = settings.gpu_format == 1
            ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipeline.color = make_texture(d3d, input->width, input->height, rgba_format,
                                      D3D12_RESOURCE_FLAG_NONE, input.get(), settings.transfer == 1);
        for (auto& output : pipeline.outputs) {
            output = make_output_texture(d3d, out_width, out_height, rgba_format);
        }
        if (settings.motion != 0) {
            const float x = settings.motion == 2 ? 0.125f : 0.0f;
            const std::array<std::uint16_t, 2> texel{
                DirectX::PackedVector::XMConvertFloatToHalf(x),
                DirectX::PackedVector::XMConvertFloatToHalf(0.0f)};
            pipeline.motion = make_constant_texture(d3d, input->width, input->height,
                DXGI_FORMAT_R16G16_FLOAT, std::as_bytes(std::span{texel}));
        }
        if (settings.depth != 0) {
            const float value = settings.depth == 2 ? 1.0f : 0.0f;
            pipeline.depth = make_constant_texture(d3d, input->width, input->height,
                DXGI_FORMAT_R32_FLOAT, std::as_bytes(std::span{&value, 1}));
        }
        if (settings.control_mask != 0) {
            const float mask_value = settings.control_mask == 1
                ? 0.0f : settings.control_mask_value;
            const bool half = settings.control_mask == 3;
            if (settings.control_mask_format == 0) {
                const std::uint8_t value = static_cast<std::uint8_t>(std::lround(
                    std::clamp(mask_value, 0.0f, 1.0f) * 255.0f));
                pipeline.control_mask = make_constant_texture(d3d, input->width, input->height,
                    DXGI_FORMAT_R8_UNORM, std::as_bytes(std::span{&value, 1}), half);
            } else {
                const std::uint16_t value =
                    DirectX::PackedVector::XMConvertFloatToHalf(mask_value);
                pipeline.control_mask = make_constant_texture(d3d, input->width, input->height,
                    DXGI_FORMAT_R16_FLOAT, std::as_bytes(std::span{&value, 1}), half);
            }
        }
        transition(d3d.command_list.Get(), pipeline.color.texture.Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (pipeline.motion) transition(d3d.command_list.Get(), pipeline.motion->texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (pipeline.depth) transition(d3d.command_list.Get(), pipeline.depth->texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (pipeline.control_mask) transition(d3d.command_list.Get(),
            pipeline.control_mask->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        params->Set("DLSSNR.Color", pipeline.color.texture.Get());
        ID3D12Resource* null_resource = nullptr;
        params->Set("DLSSNR.MVec", pipeline.motion ? pipeline.motion->texture.Get() : null_resource);
        params->Set("DLSSNR.Depth", pipeline.depth ? pipeline.depth->texture.Get() : null_resource);
        params->Set("DLSSNR.ControlMask", pipeline.control_mask
            ? pipeline.control_mask->texture.Get() : null_resource);
        for (const char* key : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
                                "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY"}) {
            params->Set(key, 0u);
        }
        params->Set("DLSSNR.ColorSubrectWidth", input->width);
        params->Set("DLSSNR.ColorSubrectHeight", input->height);
        params->Set("DLSSNR.OutputSubrectWidth", out_width);
        params->Set("DLSSNR.OutputSubrectHeight", out_height);
        if (pipeline.motion) set_optional_subrect("MVec", input->width, input->height);
        if (pipeline.depth) set_optional_subrect("Depth", input->width, input->height);
        if (pipeline.control_mask) set_optional_subrect("ControlMask", input->width, input->height);
        } catch (...) {
            d3d.discard();
            release_pipeline();
            throw;
        }
    }

    void set_optional_subrect(std::string_view name, unsigned width, unsigned height) {
        const std::string prefix = std::format("DLSSNR.{}Subrect", name);
        params->Set((prefix + "BaseX").c_str(), 0u);
        params->Set((prefix + "BaseY").c_str(), 0u);
        params->Set((prefix + "Width").c_str(), width);
        params->Set((prefix + "Height").c_str(), height);
    }

    LiveResult evaluate(const std::shared_ptr<const LiveImage>& input,
                        const LiveSettings& settings) {
        prepare(input, settings);
        const unsigned slot = pipeline.next_slot;
        pipeline.next_slot ^= 1u;
        auto& output = pipeline.outputs[slot];
        if (pipeline.evaluated[0] || pipeline.evaluated[1]) {
            d3d.reset();
        }
        if (pipeline.evaluated[slot]) {
            transition(d3d.command_list.Get(), output.texture.Get(),
                       D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        params->Set("DLSSNR.Output", output.texture.Get());
        params->Set("DLSSNR.Reset", 1);
        params->Set("DLSSNR.Enabled", settings.enabled ? 1 : 0);
        params->Set("DLSSNR.UICorrection", settings.ui_correction ? 1 : 0);
        params->Set("DLSSNR.DepthInverted", settings.depth_inverted ? 1 : 0);
        params->Set("DLSSNR.MVecScaleX", settings.mv_scale_x);
        params->Set("DLSSNR.MVecScaleY", settings.mv_scale_y);
        params->Set("DLSSNR.Style", settings.style);
        params->Set("DLSSNR.Intensity", settings.intensity);
        params->Set("DLSSNR.LocalToneStrength", settings.tone);
        params->Set("DLSSNR.LocalStructureStrength", settings.structure);
        params->Set("DLSSNR.SkinStructureStrength", settings.skin);
        params->Set("DLSSNR.UseAutoMask", settings.auto_mask ? 1 : 0);
        const auto result = caller->evaluate(reinterpret_cast<FARPROC>(ngx->evaluate),
                                              d3d.command_list.Get(), pipeline.handle, params,
                                              nullptr);
        if (NVSDK_NGX_FAILED(result)) {
            d3d.discard();
            release_pipeline();
            throw_ngx("DLSSNR live evaluation", result);
        }
        transition(d3d.command_list.Get(), output.texture.Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        d3d.execute_and_wait();
        pipeline.evaluated[slot] = true;
        LiveResult completed;
        for (unsigned index = 0; index < pipeline.outputs.size(); ++index) {
            completed.resources[index] = pipeline.outputs[index].texture;
        }
        completed.width = output.width;
        completed.height = output.height;
        completed.format = output.format;
        completed.encode_srgb = settings.transfer == 1;
        completed.slot = slot;
        completed.generation = pipeline.generation;
        return completed;
    }
};

struct Request {
    std::uint64_t revision = 0;
    std::shared_ptr<const LiveImage> image;
    LiveSettings settings;
};

} // namespace

struct LiveProcessor::Impl {
    fs::path dll_directory;
    fs::path executable_directory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::optional<LiveResult> result;
    std::atomic_bool is_busy = false;
    bool stopping = false;
    std::thread worker;

    Impl(fs::path dll_dir, fs::path exe_dir, ID3D12Device* shared_device,
         ID3D12CommandQueue* shared_queue)
        : dll_directory(std::move(dll_dir)), executable_directory(std::move(exe_dir)),
          device(shared_device), queue(shared_queue),
          worker([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }

    void run() {
        std::unique_ptr<Runtime> runtime;
        while (true) {
            Request request;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping) return;
                request = std::move(*pending);
                pending.reset();
            }

            LiveResult completed;
            completed.revision = request.revision;
            const auto started = std::chrono::steady_clock::now();
            try {
                if (!runtime) {
                    runtime = std::make_unique<Runtime>(dll_directory, executable_directory,
                                                        device.Get(), queue.Get());
                }
                completed = runtime->evaluate(request.image, request.settings);
                completed.revision = request.revision;
            } catch (const std::exception& error) {
                completed.error = error.what();
            }
            completed.milliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            std::unique_lock lock(mutex);
            result = std::move(completed);
            is_busy = true;
            wake.wait(lock, [&] { return stopping || !result.has_value(); });
            if (stopping) return;
            is_busy = pending.has_value();
        }
    }
};

LiveProcessor::LiveProcessor(fs::path dll_directory, fs::path executable_directory,
                             ID3D12Device* device, ID3D12CommandQueue* queue)
    : impl_(std::make_unique<Impl>(std::move(dll_directory),
                                   std::move(executable_directory), device, queue)) {}

LiveProcessor::~LiveProcessor() = default;

void LiveProcessor::request(std::uint64_t revision, std::shared_ptr<const LiveImage> image,
                            LiveSettings settings) {
    if (!image) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->pending = Request{revision, std::move(image), settings};
        impl_->is_busy = true;
    }
    impl_->wake.notify_one();
}

std::optional<LiveResult> LiveProcessor::take_result() {
    std::lock_guard lock(impl_->mutex);
    auto result = std::move(impl_->result);
    impl_->result.reset();
    impl_->is_busy = impl_->pending.has_value();
    impl_->wake.notify_one();
    return result;
}

bool LiveProcessor::busy() const { return impl_->is_busy.load(); }

LiveImage readback_live_image(ID3D12CommandQueue* queue, const LiveResult& result) {
    if (result.slot >= result.resources.size()) {
        throw std::runtime_error("Invalid live GPU output slot");
    }
    const auto& resource = result.resources[result.slot];
    if (!queue || !resource) throw std::runtime_error("No live GPU output to save");
    ComPtr<ID3D12Device> device;
    check_hr(resource->GetDevice(IID_PPV_ARGS(&device)), "Get live output device");
    const D3D12_RESOURCE_DESC texture = resource->GetDesc();
    if (texture.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        texture.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        throw std::runtime_error("Unsupported live output format for save readback");
    }
    if (!result.width || !result.height || texture.Width != result.width ||
        texture.Height != result.height) {
        throw std::runtime_error("Live output metadata does not match its GPU texture");
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, &rows, &row_bytes, &bytes);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> readback;
    check_hr(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&readback)),
             "Create explicit-save readback");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator)),
             "Create explicit-save allocator");
    check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                       nullptr, IID_PPV_ARGS(&list)),
             "Create explicit-save command list");
    transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = resource.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    check_hr(list->Close(), "Close explicit-save command list");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
             "Create explicit-save fence");
    check_hr(queue->Signal(fence.Get(), 1), "Signal explicit-save fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) check_hr(HRESULT_FROM_WIN32(GetLastError()), "Create explicit-save event");
    const HRESULT event_result = fence->SetEventOnCompletion(1, event);
    if (FAILED(event_result)) {
        CloseHandle(event);
        check_hr(event_result, "Wait for explicit-save fence");
    }
    const DWORD wait_result = WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    if (wait_result != WAIT_OBJECT_0) {
        check_hr(HRESULT_FROM_WIN32(GetLastError()), "Wait for explicit-save event");
    }
    check_hr(device->GetDeviceRemovedReason(), "Explicit-save GPU execution");

    LiveImage image{result.width, result.height,
                    std::vector<std::uint8_t>(static_cast<std::size_t>(result.width) *
                                              result.height * 4)};
    std::byte* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check_hr(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)),
             "Map explicit-save readback");
    for (unsigned y = 0; y < result.height; ++y) {
        const auto* row = mapped + y * footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < result.width; ++x) {
            for (unsigned channel = 0; channel < 4; ++channel) {
                const auto index = (static_cast<std::size_t>(y) * result.width + x) * 4 + channel;
                float value = texture.Format == DXGI_FORMAT_R8G8B8A8_UNORM
                    ? reinterpret_cast<const std::uint8_t*>(row)[x * 4 + channel] / 255.0f
                    : DirectX::PackedVector::XMConvertHalfToFloat(
                          reinterpret_cast<const std::uint16_t*>(row)[x * 4 + channel]);
                if (result.encode_srgb && channel != 3) value = linear_to_srgb(value);
                value = std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
                image.rgba[index] = static_cast<std::uint8_t>(std::lround(value * 255.0f));
            }
        }
    }
    readback->Unmap(0, nullptr);
    return image;
}

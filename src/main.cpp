#include "ngx_core.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr auto kFeature = NVSDK_NGX_Feature_Reserved18;
constexpr char kProjectId[] = "53f803cc-a12f-4d69-90d5-19b7599cad19";
// Driver 616.56 maps any custom-engine Project ID to this generic CMS/App ID.
constexpr std::uint64_t kGenericCustomAppId = 0x0876232c;
constexpr unsigned kProbeWidth = 512;
constexpr unsigned kProbeHeight = 512;

struct NeuralSettings {
    enum class Motion { absent, zero, tiny } motion = Motion::absent;
    enum class Depth { absent, zero, one } depth = Depth::absent;
    enum class Transfer { code_values, srgb_linear } transfer = Transfer::code_values;
    enum class GpuFormat { rgba16f, rgba8 } gpu_format = GpuFormat::rgba16f;
    enum class Mask { absent, zero, one, half } control_mask = Mask::absent;
    enum class MaskFormat { r8, r16f } mask_format = MaskFormat::r16f;
    float control_mask_value = 1.0f;
    unsigned output_scale = 1;
    unsigned preset = 0;
    unsigned style = 0;
    float intensity = 1.0f;
    float local_tone = 1.0f;
    float local_structure = 1.0f;
    float skin_structure = -1.0f;
    int auto_mask = 0;
    int enabled = 1;
    int ui_correction = 0;
    int depth_inverted = 1;
    float mv_scale_x = 1.0f;
    float mv_scale_y = 1.0f;
    bool diagnostics = true;
};

std::string_view motion_name(NeuralSettings::Motion motion) {
    switch (motion) {
    case NeuralSettings::Motion::absent: return "absent";
    case NeuralSettings::Motion::zero: return "zero";
    case NeuralSettings::Motion::tiny: return "tiny";
    }
    return "unknown";
}

std::string_view depth_name(NeuralSettings::Depth depth) {
    switch (depth) {
    case NeuralSettings::Depth::absent: return "absent";
    case NeuralSettings::Depth::zero: return "zero";
    case NeuralSettings::Depth::one: return "one";
    }
    return "unknown";
}

std::string_view transfer_name(NeuralSettings::Transfer transfer) {
    return transfer == NeuralSettings::Transfer::srgb_linear ? "sRGB EOTF -> linear" :
                                                               "RGBA8 code values / 255";
}

std::string_view gpu_format_name(NeuralSettings::GpuFormat format) {
    return format == NeuralSettings::GpuFormat::rgba8 ? "DXGI_FORMAT_R8G8B8A8_UNORM" :
                                                       "DXGI_FORMAT_R16G16B16A16_FLOAT";
}

std::string_view mask_name(NeuralSettings::Mask mask) {
    switch (mask) {
    case NeuralSettings::Mask::absent: return "absent";
    case NeuralSettings::Mask::zero: return "zero";
    case NeuralSettings::Mask::one: return "one";
    case NeuralSettings::Mask::half: return "half";
    }
    return "unknown";
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f :
                              std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) {
    value = std::max(value, 0.0f);
    return value <= 0.0031308f ? value * 12.92f :
                                1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
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
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented: return "NotImplemented";
    default: return "Unknown";
    }
}

std::string ngx_result(NVSDK_NGX_Result result) {
    return std::format("{} (0x{:08X})", ngx_name(result), static_cast<std::uint32_t>(result));
}

void check_hr(HRESULT hr, std::string_view operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::format("{} failed: HRESULT 0x{:08X}", operation,
                                             static_cast<std::uint32_t>(hr)));
    }
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level level,
                        NVSDK_NGX_Feature source) {
    std::clog << std::format("[NGX level={} feature={}] {}", static_cast<int>(level),
                             static_cast<int>(source), message ? message : "<null>\n");
}

std::wstring file_version(const fs::path& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return L"unavailable";
    std::string data(size, '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"unavailable";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        !info || info_size < sizeof(*info)) return L"unavailable";
    return std::format(L"{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("WideCharToMultiByte failed");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::string windows_version() {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const auto rtl_get_version = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtl_get_version || rtl_get_version(&info) != 0) return "unavailable";
    return std::format("{}.{}.{}{}{}", info.dwMajorVersion, info.dwMinorVersion,
                       info.dwBuildNumber, info.szCSDVersion[0] ? " " : "",
                       utf8(info.szCSDVersion));
}

struct DebugState {
    bool layer = false;
    bool dred = false;
};

DebugState enable_debug_layer() {
    DebugState state;
#if !defined(NDEBUG) || defined(DLSSNR_D3D_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        state.layer = true;
        std::cout << "D3D12 debug layer: ON\n";
    } else {
        std::cout << "D3D12 debug layer: unavailable\n";
    }

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        state.dred = true;
        std::cout << "DRED: ON\n";
    }
#endif
    return state;
}

struct D3D12Context {
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    std::uint64_t fence_value = 0;

    D3D12Context() {
        UINT factory_flags = 0;
#ifndef NDEBUG
        factory_flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
        ComPtr<IDXGIFactory6> factory;
        check_hr(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

        for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))); ++i) {
            DXGI_ADAPTER_DESC3 desc{};
            check_hr(adapter->GetDesc3(&desc), "IDXGIAdapter4::GetDesc3");
            if (!(desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) && desc.VendorId == 0x10DE &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                            IID_PPV_ARGS(&device)))) break;
            adapter.Reset();
            device.Reset();
        }
        if (!device) throw std::runtime_error("No NVIDIA D3D12 FL12_0 adapter found");

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check_hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)),
                 "CreateCommandQueue");
        check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator)),
                 "CreateCommandAllocator");
        check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&command_list)),
                 "CreateCommandList");
        check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) check_hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
    }

    ~D3D12Context() {
        if (fence_event) CloseHandle(fence_event);
    }

    void execute_and_wait() {
        check_hr(command_list->Close(), "Close command list");
        ID3D12CommandList* lists[] = {command_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ++fence_value;
        check_hr(queue->Signal(fence.Get(), fence_value), "Signal fence");
        if (fence->GetCompletedValue() < fence_value) {
            check_hr(fence->SetEventOnCompletion(fence_value, fence_event), "SetEventOnCompletion");
            WaitForSingleObject(fence_event, INFINITE);
        }
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) check_hr(removed, "D3D12 device execution");
    }

    void reset() {
        check_hr(allocator->Reset(), "Reset command allocator");
        check_hr(command_list->Reset(allocator.Get(), nullptr), "Reset command list");
    }
};

void print_system(const D3D12Context& d3d, const fs::path& snippet,
                  const fs::path& core_path) {
    DXGI_ADAPTER_DESC3 desc{};
    check_hr(d3d.adapter->GetDesc3(&desc), "IDXGIAdapter4::GetDesc3");
    LARGE_INTEGER driver{};
    const HRESULT driver_hr = d3d.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);

    std::wcout << L"GPU: " << desc.Description << L"\n"
               << std::format(L"PCI: vendor=0x{:04X} device=0x{:04X}\n", desc.VendorId, desc.DeviceId)
               << std::format(L"VRAM: {} MiB\n", desc.DedicatedVideoMemory / (1024 * 1024))
               << L"DLSSNR DLL: " << snippet << L"\n"
               << L"DLSSNR DLL version: " << file_version(snippet) << L"\n"
               << L"NVIDIA driver NGX core: " << core_path << L"\n";
    if (SUCCEEDED(driver_hr)) {
        std::wcout << std::format(L"DXGI driver version: {}.{}.{}.{} (raw 0x{:016X})\n",
            HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart),
            static_cast<std::uint64_t>(driver.QuadPart));
    } else {
        std::wcout << std::format(L"DXGI driver version: unavailable (0x{:08X})\n",
                                  static_cast<std::uint32_t>(driver_hr));
    }
    std::cout << "D3D12 feature level: 12_0 or newer\n";
}

std::optional<DXGI_QUERY_VIDEO_MEMORY_INFO> video_memory_info(const D3D12Context& d3d) {
    ComPtr<IDXGIAdapter3> adapter;
    if (FAILED(d3d.adapter.As(&adapter))) return std::nullopt;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return std::nullopt;
    }
    return info;
}

struct Rgba8Image {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> pixels;
};

ComPtr<IWICImagingFactory> make_wic_factory() {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
    }
    check_hr(hr, "Create WIC factory");
    return factory;
}

Rgba8Image load_rgba8(IWICImagingFactory* factory, const fs::path& path) {
    ComPtr<IWICBitmapDecoder> decoder;
    check_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnLoad, &decoder),
             "Open input image");
    ComPtr<IWICBitmapFrameDecode> frame;
    check_hr(decoder->GetFrame(0, &frame), "Decode first image frame");

    Rgba8Image image;
    check_hr(frame->GetSize(&image.width, &image.height), "Read image dimensions");
    if (!image.width || !image.height) throw std::runtime_error("Input image is empty");
    const std::size_t byte_count = static_cast<std::size_t>(image.width) * image.height * 4;
    if (byte_count > UINT_MAX) throw std::runtime_error("Input image exceeds WIC's 4 GiB buffer limit");

    ComPtr<IWICFormatConverter> converter;
    check_hr(factory->CreateFormatConverter(&converter), "Create WIC RGBA converter");
    check_hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom),
             "Convert input image to RGBA8");
    image.pixels.resize(byte_count);
    check_hr(converter->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(byte_count),
                                   image.pixels.data()),
             "Copy decoded RGBA8 pixels");
    return image;
}

Rgba8Image resize_rgba8(IWICImagingFactory* factory, const Rgba8Image& input,
                        unsigned width, unsigned height) {
    ComPtr<IWICBitmap> bitmap;
    check_hr(factory->CreateBitmapFromMemory(input.width, input.height,
                                              GUID_WICPixelFormat32bppRGBA,
                                              input.width * 4,
                                              static_cast<UINT>(input.pixels.size()),
                                              const_cast<BYTE*>(input.pixels.data()), &bitmap),
             "Create WIC resize source");
    ComPtr<IWICBitmapScaler> scaler;
    check_hr(factory->CreateBitmapScaler(&scaler), "Create WIC scaler");
    check_hr(scaler->Initialize(bitmap.Get(), width, height, WICBitmapInterpolationModeFant),
             "Resize baseline image");
    Rgba8Image output{width, height,
                      std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    check_hr(scaler->CopyPixels(nullptr, width * 4, static_cast<UINT>(output.pixels.size()),
                                output.pixels.data()),
             "Copy resized baseline image");
    return output;
}

std::vector<float> rgba8_to_float(const Rgba8Image& image, bool decode_srgb) {
    std::vector<float> output(image.pixels.size());
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = image.pixels[i] / 255.0f;
        if (decode_srgb && i % 4 != 3) output[i] = srgb_to_linear(output[i]);
    }
    return output;
}

void save_png(IWICImagingFactory* factory, const fs::path& path, const Rgba8Image& image) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    ComPtr<IWICStream> stream;
    check_hr(factory->CreateStream(&stream), "Create PNG output stream");
    check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open PNG output");

    ComPtr<IWICBitmapEncoder> encoder;
    check_hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
    check_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    check_hr(encoder->CreateNewFrame(&frame, &properties), "Create PNG frame");
    check_hr(frame->Initialize(properties.Get()), "Initialize PNG frame");
    check_hr(frame->SetSize(image.width, image.height), "Set PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check_hr(frame->SetPixelFormat(&format), "Set PNG BGRA8 format");
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        throw std::runtime_error("WIC PNG encoder rejected BGRA8");
    }
    auto bgra = image.pixels;
    for (std::size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    check_hr(frame->WritePixels(image.height, image.width * 4,
                                static_cast<UINT>(bgra.size()), bgra.data()),
             "Write PNG pixels");
    check_hr(frame->Commit(), "Commit PNG frame");
    check_hr(encoder->Commit(), "Commit PNG file");
}

Rgba8Image to_rgba8(unsigned width, unsigned height, const std::vector<float>& pixels,
                    bool encode_srgb = false) {
    Rgba8Image image{width, height, std::vector<std::uint8_t>(pixels.size())};
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        float value = std::isfinite(pixels[i]) ? pixels[i] : 0.0f;
        if (encode_srgb && i % 4 != 3) value = linear_to_srgb(value);
        value = std::clamp(value, 0.0f, 1.0f);
        image.pixels[i] = static_cast<std::uint8_t>(std::lround(value * 255.0f));
    }
    return image;
}

fs::path artifact_path(const fs::path& output, std::wstring_view suffix,
                       std::wstring_view extension = L".png") {
    return output.parent_path() / (output.stem().wstring() + std::wstring(suffix) +
                                   std::wstring(extension));
}

NVSDK_NGX_Result NVSDK_CONV compute_scaling_ratio(NVSDK_NGX_Parameter* params) {
    if (!params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    unsigned upscaling = 0;
    const auto result = params->Get("DLSSNR.Upscaling", &upscaling);
    params->Set("DLSSNR.ScalingRatio", NVSDK_NGX_SUCCEED(result) && upscaling ? 0.5f : 1.0f);
    return NVSDK_NGX_Result_Success;
}

void set_create_params(NVSDK_NGX_Parameter* params, unsigned width, unsigned height,
                       unsigned out_width, unsigned out_height, unsigned preset = 0) {
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
    params->Set("DLSSNRComputeScalingRatioCallback",
                reinterpret_cast<void*>(&compute_scaling_ratio));
    params->Set("DLSSNR.Hint.Render.Preset", static_cast<int>(preset));
    params->Set("DLSS.Feature.Create.Flags", 0);
}

template <typename T>
T load_export(HMODULE module, const char* name) {
    auto proc = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!proc) throw std::runtime_error(std::format("Missing DLL export: {}", name));
    return proc;
}

struct DirectNgx {
    using InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                                  NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using Create = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                 const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                   const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
    using Release = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
    using Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

    HMODULE module;
    InitExt init;
    Create create;
    Evaluate evaluate;
    Release release;
    Shutdown shutdown;

    explicit DirectNgx(const fs::path& path)
        : module(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                         LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module) {
            throw std::runtime_error(std::format("LoadLibraryExW failed: {}", GetLastError()));
        }
        init = load_export<InitExt>(module, "NVSDK_NGX_D3D12_Init_Ext");
        create = load_export<Create>(module, "NVSDK_NGX_D3D12_CreateFeature");
        evaluate = load_export<Evaluate>(module, "NVSDK_NGX_D3D12_EvaluateFeature");
        release = load_export<Release>(module, "NVSDK_NGX_D3D12_ReleaseFeature");
        shutdown = load_export<Shutdown>(module, "NVSDK_NGX_D3D12_Shutdown1");
    }

    ~DirectNgx() {
        if (module) FreeLibrary(module);
    }
};

fs::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("GetModuleFileNameW failed");
    return fs::path(std::wstring_view(path.data(), length)).parent_path();
}

fs::path default_runtime_directory() {
    const fs::path application = executable_directory();
    const std::array candidates = {application, fs::current_path()};
    std::error_code error;
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate / L"nvngx_dlssnr.dll", error)) return candidate;
        error.clear();
    }
    return application;
}

struct NgxCaller {
    using CallInit = NVSDK_NGX_Result(*)(FARPROC, unsigned long long, const wchar_t*, ID3D12Device*,
                                         NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using CallCreate = NVSDK_NGX_Result(*)(FARPROC, ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                           const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using CallEvaluate = NVSDK_NGX_Result(*)(FARPROC, ID3D12GraphicsCommandList*,
                                              const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                              PFN_NVSDK_NGX_ProgressCallback);
    using CallRelease = NVSDK_NGX_Result(*)(FARPROC, NVSDK_NGX_Handle*);
    using CallShutdown = NVSDK_NGX_Result(*)(FARPROC, ID3D12Device*);

    HMODULE module;
    CallInit init;
    CallCreate create;
    CallEvaluate evaluate;
    CallRelease release;
    CallShutdown shutdown;

    explicit NgxCaller(const fs::path& path) : module(LoadLibraryExW(
        path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module) throw std::runtime_error(std::format("Loading caller shim failed: {}", GetLastError()));
        init = load_export<CallInit>(module, "DLSSNR_CallInit");
        create = load_export<CallCreate>(module, "DLSSNR_CallCreate");
        evaluate = load_export<CallEvaluate>(module, "DLSSNR_CallEvaluate");
        release = load_export<CallRelease>(module, "DLSSNR_CallRelease");
        shutdown = load_export<CallShutdown>(module, "DLSSNR_CallShutdown");
    }

    ~NgxCaller() {
        if (module) FreeLibrary(module);
    }
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

D3D12_RESOURCE_DESC texture2d_desc(unsigned width, unsigned height, DXGI_FORMAT format,
                                   D3D12_RESOURCE_FLAGS flags) {
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

UploadedTexture make_uploaded_constant_texture(D3D12Context& d3d, unsigned width, unsigned height,
                                                DXGI_FORMAT format,
                                                std::span<const std::byte> texel,
                                                bool zero_left_half = false) {
    UploadedTexture image;
    image.width = width;
    image.height = height;
    image.format = format;
    const auto desc = texture2d_desc(width, height, format, D3D12_RESOURCE_FLAG_NONE);
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    check_hr(d3d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&image.texture)),
             "Create constant texture");

    UINT64 row_bytes = 0;
    d3d.device->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, &image.rows,
                                      &row_bytes, &image.bytes);
    if (row_bytes != static_cast<UINT64>(width) * texel.size()) {
        throw std::runtime_error("Unexpected constant texture texel size");
    }
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = image.bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    check_hr(d3d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&image.upload)),
             "Create constant texture upload buffer");

    std::byte* mapped = nullptr;
    check_hr(image.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)),
             "Map constant texture upload buffer");
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

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = image.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = image.footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = image.texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d3d.command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return image;
}

UploadedTexture make_uploaded_texture(D3D12Context& d3d, unsigned width, unsigned height,
                                      D3D12_RESOURCE_FLAGS flags, const std::vector<std::uint8_t>* rgba8,
                                      bool make_test_pattern, std::vector<float>* cpu_pixels,
                                      bool decode_srgb = false,
                                      NeuralSettings::GpuFormat gpu_format = NeuralSettings::GpuFormat::rgba16f) {
    UploadedTexture image;
    image.width = width;
    image.height = height;
    image.format = gpu_format == NeuralSettings::GpuFormat::rgba8
        ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto desc = texture2d_desc(width, height, image.format, flags);
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    check_hr(d3d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&image.texture)),
             "Create RGBA16F texture");

    UINT64 row_bytes = 0;
    d3d.device->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, &image.rows,
                                      &row_bytes, &image.bytes);
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = image.bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    check_hr(d3d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&image.upload)),
             "Create texture upload buffer");

    std::byte* mapped = nullptr;
    check_hr(image.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map upload buffer");
    std::fill_n(mapped, static_cast<std::size_t>(image.bytes), std::byte{});
    if (cpu_pixels) cpu_pixels->resize(static_cast<std::size_t>(width) * height * 4);
    for (unsigned y = 0; y < height; ++y) {
        auto* row = mapped + y * image.footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < width; ++x) {
            std::array<float, 4> rgba{};
            if (rgba8) {
                for (unsigned channel = 0; channel < 4; ++channel) {
                    rgba[channel] = (*rgba8)[(static_cast<std::size_t>(y) * width + x) * 4 + channel] /
                                    255.0f;
                    if (decode_srgb && channel != 3) rgba[channel] = srgb_to_linear(rgba[channel]);
                }
            } else if (make_test_pattern) {
                const float fx = static_cast<float>(x) / static_cast<float>(width - 1);
                const float fy = static_cast<float>(y) / static_cast<float>(height - 1);
                const float checker = ((x / 16 + y / 16) & 1) ? 0.08f : -0.08f;
                rgba = {std::clamp(0.1f + 0.8f * fx + checker, 0.0f, 1.0f),
                        std::clamp(0.1f + 0.8f * fy - checker, 0.0f, 1.0f),
                        std::clamp(0.15f + 0.7f * (1.0f - fx) + checker, 0.0f, 1.0f), 1.0f};
            }
            for (unsigned channel = 0; channel < 4; ++channel) {
                if (image.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    reinterpret_cast<std::uint8_t*>(row)[x * 4 + channel] =
                        static_cast<std::uint8_t>(std::lround(std::clamp(rgba[channel], 0.0f, 1.0f) * 255.0f));
                } else {
                    reinterpret_cast<std::uint16_t*>(row)[x * 4 + channel] =
                        DirectX::PackedVector::XMConvertFloatToHalf(rgba[channel]);
                }
                if (cpu_pixels) (*cpu_pixels)[(static_cast<std::size_t>(y) * width + x) * 4 + channel] = rgba[channel];
            }
        }
    }
    image.upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = image.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = image.footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = image.texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d3d.command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
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

ComPtr<ID3D12Resource> make_readback(D3D12Context& d3d, const UploadedTexture& source) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = source.bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> readback;
    check_hr(d3d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&readback)),
             "Create readback buffer");
    return readback;
}

std::vector<float> readback_rgba(ID3D12Resource* readback, const UploadedTexture& image) {
    std::byte* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(image.bytes)};
    check_hr(readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped)),
             "Map readback buffer");
    std::vector<float> pixels(static_cast<std::size_t>(image.width) * image.height * 4);
    for (unsigned y = 0; y < image.height; ++y) {
        const auto* row = mapped + y * image.footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < image.width; ++x) {
            for (unsigned channel = 0; channel < 4; ++channel) {
                const auto index = (static_cast<std::size_t>(y) * image.width + x) * 4 + channel;
                pixels[index] = image.format == DXGI_FORMAT_R8G8B8A8_UNORM
                    ? reinterpret_cast<const std::uint8_t*>(row)[x * 4 + channel] / 255.0f
                    : DirectX::PackedVector::XMConvertHalfToFloat(
                          reinterpret_cast<const std::uint16_t*>(row)[x * 4 + channel]);
            }
        }
    }
    readback->Unmap(0, nullptr);
    return pixels;
}

void copy_to_readback(ID3D12GraphicsCommandList* list, const UploadedTexture& image,
                      ID3D12Resource* readback) {
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = image.texture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = image.footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

std::string print_metrics(const std::vector<float>& reference, const std::vector<float>& output,
                          std::string_view label) {
    if (reference.size() != output.size() || output.empty()) {
        throw std::runtime_error("Metric inputs have different or empty pixel arrays");
    }
    double absolute_sum = 0.0;
    double squared_sum = 0.0;
    double output_sum = 0.0;
    double maximum = 0.0;
    std::size_t finite_values = 0;
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < output.size(); ++i) {
        const float value = output[i];
        if (std::isfinite(value)) ++finite_values;
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash = (hash ^ static_cast<std::uint8_t>(bits >> shift)) * 1099511628211ull;
        }
        const double difference = static_cast<double>(value) - reference[i];
        absolute_sum += std::abs(difference);
        squared_sum += difference * difference;
        output_sum += value;
        maximum = std::max(maximum, std::abs(difference));
    }
    const auto values = output.size();
    const double mae = absolute_sum / values;
    const double mse = squared_sum / values;
    const double psnr = mse > 0.0 ? 10.0 * std::log10(1.0 / mse) : INFINITY;
    auto line = std::format(
        "{}: finite={}/{} mean={:.6f} hash=0x{:016X} MAE={:.8f} MSE={:.8f} "
        "PSNR={:.3f} dB max_abs={:.8f}\n",
        label, finite_values, values, output_sum / values, hash, mae, mse, psnr, maximum);
    std::cout << line;
    return line;
}

NVSDK_NGX_Result evaluate_synthetic(D3D12Context& d3d, DirectNgx& ngx, NgxCaller& caller,
                                    NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params) {
    d3d.reset();
    std::vector<float> input_pixels;
    auto color = make_uploaded_texture(d3d, kProbeWidth, kProbeHeight, D3D12_RESOURCE_FLAG_NONE,
                                       nullptr, true, &input_pixels);
    auto output = make_uploaded_texture(d3d, kProbeWidth, kProbeHeight,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        nullptr, false, nullptr);
    transition(d3d.command_list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(d3d.command_list.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    params->Set("DLSSNR.Color", color.texture.Get());
    params->Set("DLSSNR.Output", output.texture.Get());
    for (const char* key : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
                            "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY"}) {
        params->Set(key, 0u);
    }
    params->Set("DLSSNR.ColorSubrectWidth", kProbeWidth);
    params->Set("DLSSNR.ColorSubrectHeight", kProbeHeight);
    params->Set("DLSSNR.OutputSubrectWidth", kProbeWidth);
    params->Set("DLSSNR.OutputSubrectHeight", kProbeHeight);
    params->Set("DLSSNR.Reset", 1);
    params->Set("DLSSNR.Enabled", 1);
    std::cout << "Evaluate frame=0 reset=1 Color/Output=RGBA16F 512x512 "
                 "states=NON_PIXEL_SHADER_RESOURCE/UAV MVec=null Depth=null\n";

    auto result = caller.evaluate(reinterpret_cast<FARPROC>(ngx.evaluate), d3d.command_list.Get(),
                                  handle, params, nullptr);
    std::cout << "DLSSNR direct EvaluateFeature: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_FAILED(result)) return result;

    transition(d3d.command_list.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto readback = make_readback(d3d, output);
    copy_to_readback(d3d.command_list.Get(), output, readback.Get());
    d3d.execute_and_wait();
    const auto output_pixels = readback_rgba(readback.Get(), output);
    print_metrics(input_pixels, output_pixels, "Synthetic input vs DLSSNR raw output");
    return result;
}

NVSDK_NGX_Result evaluate_image(D3D12Context& d3d, DirectNgx& ngx, NgxCaller& caller,
                                 NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params,
                                 IWICImagingFactory* wic, const Rgba8Image& input,
                                 const fs::path& output_path, const fs::path& snippet,
                                 const fs::path& caller_path, const fs::path& core_path,
                                 DebugState debug_state,
                                 unsigned output_width, unsigned output_height,
                                unsigned frame_count, bool reset_every_frame, bool sequence,
                                const NeuralSettings& settings) {
    const auto vram_before = settings.diagnostics ? video_memory_info(d3d) : std::nullopt;
    d3d.reset();
    std::vector<float> original_pixels;
    auto color = make_uploaded_texture(d3d, input.width, input.height, D3D12_RESOURCE_FLAG_NONE,
                                       &input.pixels, false,
                                       settings.diagnostics ? &original_pixels : nullptr,
                                       settings.transfer == NeuralSettings::Transfer::srgb_linear,
                                       settings.gpu_format);
    auto output = make_uploaded_texture(d3d, output_width, output_height,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        nullptr, false, nullptr, false, settings.gpu_format);
    std::optional<UploadedTexture> motion;
    if (settings.motion != NeuralSettings::Motion::absent) {
        const float x = settings.motion == NeuralSettings::Motion::tiny ? 0.125f : 0.0f;
        const std::array<std::uint16_t, 2> texel{
            DirectX::PackedVector::XMConvertFloatToHalf(x),
            DirectX::PackedVector::XMConvertFloatToHalf(0.0f)};
        motion = make_uploaded_constant_texture(d3d, input.width, input.height,
                                                 DXGI_FORMAT_R16G16_FLOAT,
                                                 std::as_bytes(std::span{texel}));
    }
    std::optional<UploadedTexture> depth;
    if (settings.depth != NeuralSettings::Depth::absent) {
        const float value = settings.depth == NeuralSettings::Depth::one ? 1.0f : 0.0f;
        depth = make_uploaded_constant_texture(d3d, input.width, input.height,
                                                DXGI_FORMAT_R32_FLOAT,
                                                std::as_bytes(std::span{&value, 1}));
    }
    std::optional<UploadedTexture> control_mask;
    if (settings.control_mask != NeuralSettings::Mask::absent) {
        const float mask_value = settings.control_mask == NeuralSettings::Mask::zero
            ? 0.0f : settings.control_mask_value;
        const bool half = settings.control_mask == NeuralSettings::Mask::half;
        if (settings.mask_format == NeuralSettings::MaskFormat::r8) {
            const std::uint8_t value = static_cast<std::uint8_t>(
                std::lround(std::clamp(mask_value, 0.0f, 1.0f) * 255.0f));
            control_mask = make_uploaded_constant_texture(d3d, input.width, input.height,
                DXGI_FORMAT_R8_UNORM, std::as_bytes(std::span{&value, 1}), half);
        } else {
            const std::uint16_t value = DirectX::PackedVector::XMConvertFloatToHalf(mask_value);
            control_mask = make_uploaded_constant_texture(d3d, input.width, input.height,
                DXGI_FORMAT_R16_FLOAT, std::as_bytes(std::span{&value, 1}), half);
        }
    }
    transition(d3d.command_list.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (motion) {
        transition(d3d.command_list.Get(), motion->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (depth) {
        transition(d3d.command_list.Get(), depth->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (control_mask) {
        transition(d3d.command_list.Get(), control_mask->texture.Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ComPtr<ID3D12Resource> baseline_readback;
    if (settings.diagnostics) {
        transition(d3d.command_list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        baseline_readback = make_readback(d3d, color);
        copy_to_readback(d3d.command_list.Get(), color, baseline_readback.Get());
        transition(d3d.command_list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
        transition(d3d.command_list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    auto output_readback = make_readback(d3d, output);

    params->Set("DLSSNR.Color", color.texture.Get());
    params->Set("DLSSNR.Output", output.texture.Get());
    ID3D12Resource* null_resource = nullptr;
    params->Set("DLSSNR.MVec", motion ? motion->texture.Get() : null_resource);
    params->Set("DLSSNR.Depth", depth ? depth->texture.Get() : null_resource);
    params->Set("DLSSNR.ControlMask", control_mask ? control_mask->texture.Get() : null_resource);
    for (const char* key : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
                            "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY"}) {
        params->Set(key, 0u);
    }
    params->Set("DLSSNR.ColorSubrectWidth", input.width);
    params->Set("DLSSNR.ColorSubrectHeight", input.height);
    params->Set("DLSSNR.OutputSubrectWidth", output_width);
    params->Set("DLSSNR.OutputSubrectHeight", output_height);
    if (motion) {
        for (const char* key : {"DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY"}) {
            params->Set(key, 0u);
        }
        params->Set("DLSSNR.MVecSubrectWidth", input.width);
        params->Set("DLSSNR.MVecSubrectHeight", input.height);
    }
    if (depth) {
        for (const char* key : {"DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY"}) {
            params->Set(key, 0u);
        }
        params->Set("DLSSNR.DepthSubrectWidth", input.width);
        params->Set("DLSSNR.DepthSubrectHeight", input.height);
    }
    if (control_mask) {
        for (const char* key : {"DLSSNR.ControlMaskSubrectBaseX",
                                "DLSSNR.ControlMaskSubrectBaseY"}) {
            params->Set(key, 0u);
        }
        params->Set("DLSSNR.ControlMaskSubrectWidth", input.width);
        params->Set("DLSSNR.ControlMaskSubrectHeight", input.height);
    }
    params->Set("DLSSNR.Enabled", settings.enabled);
    params->Set("DLSSNR.UICorrection", settings.ui_correction);
    params->Set("DLSSNR.DepthInverted", settings.depth_inverted);
    params->Set("DLSSNR.MVecScaleX", settings.mv_scale_x);
    params->Set("DLSSNR.MVecScaleY", settings.mv_scale_y);
    params->Set("DLSSNR.Style", settings.style);
    params->Set("DLSSNR.Intensity", settings.intensity);
    params->Set("DLSSNR.LocalToneStrength", settings.local_tone);
    params->Set("DLSSNR.LocalStructureStrength", settings.local_structure);
    params->Set("DLSSNR.SkinStructureStrength", settings.skin_structure);
    params->Set("DLSSNR.UseAutoMask", settings.auto_mask);
    const auto original_path = sequence ? output_path / L"original.png"
                                        : artifact_path(output_path, L"_original");
    const auto baseline_path = sequence ? output_path / L"baseline.png"
                                        : artifact_path(output_path, L"_baseline");
    const auto log_path = sequence ? output_path / L"diagnostic.log"
                                   : artifact_path(output_path, L"_diagnostic", L".log");
    std::vector<std::string> metric_lines;
    std::vector<std::string> timing_lines;
    std::vector<float> baseline_pixels;
    std::vector<float> previous_output;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;

    ComPtr<ID3D12QueryHeap> query_heap;
    ComPtr<ID3D12Resource> timestamp_readback;
    std::uint64_t timestamp_frequency = 0;
    if (settings.diagnostics) {
        D3D12_QUERY_HEAP_DESC query_desc{};
        query_desc.Count = frame_count * 2;
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        check_hr(d3d.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query_heap)),
                 "Create timestamp query heap");
        D3D12_RESOURCE_DESC timestamp_desc{};
        timestamp_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        timestamp_desc.Width = static_cast<UINT64>(query_desc.Count) * sizeof(std::uint64_t);
        timestamp_desc.Height = 1;
        timestamp_desc.DepthOrArraySize = 1;
        timestamp_desc.MipLevels = 1;
        timestamp_desc.SampleDesc.Count = 1;
        timestamp_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        check_hr(d3d.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                      &timestamp_desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&timestamp_readback)),
                 "Create timestamp readback buffer");
        check_hr(d3d.queue->GetTimestampFrequency(&timestamp_frequency),
                 "Get timestamp frequency");
    }

    for (unsigned frame_index = 0; frame_index < frame_count; ++frame_index) {
        if (frame_index) {
            d3d.reset();
            transition(d3d.command_list.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const int reset = (frame_index == 0 || reset_every_frame) ? 1 : 0;
        params->Set("DLSSNR.Reset", reset);
        std::cout << std::format(
            "Evaluate frame={} reset={} Color/Output={} {}x{} -> {}x{} "
            "states=NON_PIXEL_SHADER_RESOURCE/UAV MVec={} Depth={} ControlMask={}\n",
            frame_index, reset, gpu_format_name(settings.gpu_format), input.width, input.height,
            output_width, output_height,
            motion_name(settings.motion), depth_name(settings.depth), mask_name(settings.control_mask));
        std::chrono::steady_clock::time_point wall_start;
        std::chrono::steady_clock::time_point call_start;
        if (settings.diagnostics) {
            wall_start = std::chrono::steady_clock::now();
            d3d.command_list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       frame_index * 2);
            call_start = std::chrono::steady_clock::now();
        }
        result = caller.evaluate(reinterpret_cast<FARPROC>(ngx.evaluate), d3d.command_list.Get(),
                                 handle, params, nullptr);
        const auto call_end = settings.diagnostics ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
        if (settings.diagnostics) {
            d3d.command_list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       frame_index * 2 + 1);
        }
        std::cout << "DLSSNR direct EvaluateFeature: " << ngx_result(result) << "\n";
        if (NVSDK_NGX_FAILED(result)) return result;

        transition(d3d.command_list.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_to_readback(d3d.command_list.Get(), output, output_readback.Get());
        if (settings.diagnostics) {
            d3d.command_list->ResolveQueryData(
                query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_index * 2, 2,
                timestamp_readback.Get(),
                static_cast<UINT64>(frame_index) * 2 * sizeof(std::uint64_t));
        }
        d3d.execute_and_wait();
        if (settings.diagnostics) {
            const auto wall_end = std::chrono::steady_clock::now();
            const D3D12_RANGE timestamp_range{
                static_cast<SIZE_T>(frame_index) * 2 * sizeof(std::uint64_t),
                static_cast<SIZE_T>(frame_index + 1) * 2 * sizeof(std::uint64_t)};
            std::uint64_t* timestamps = nullptr;
            check_hr(timestamp_readback->Map(0, &timestamp_range,
                                             reinterpret_cast<void**>(&timestamps)),
                     "Map timestamp readback buffer");
            const auto gpu_ticks = timestamps[frame_index * 2 + 1] - timestamps[frame_index * 2];
            timestamp_readback->Unmap(0, nullptr);
            const double call_ms =
                std::chrono::duration<double, std::milli>(call_end - call_start).count();
            const double wall_ms =
                std::chrono::duration<double, std::milli>(wall_end - wall_start).count();
            const double gpu_ms = static_cast<double>(gpu_ticks) * 1000.0 / timestamp_frequency;
            auto timing = std::format(
                "Timing frame {:03}: CPU API={:.3f} ms GPU={:.3f} ms wall={:.3f} ms\n",
                frame_index, call_ms, gpu_ms, wall_ms);
            std::cout << timing;
            timing_lines.push_back(std::move(timing));
        }
        if (settings.diagnostics && frame_index == 0) {
            const auto input_baseline = readback_rgba(baseline_readback.Get(), color);
            metric_lines.push_back(print_metrics(original_pixels, input_baseline,
                                                  "Original vs input baseline roundtrip"));
            save_png(wic, original_path, input);
            const auto encoded_baseline = to_rgba8(
                input.width, input.height, input_baseline,
                settings.transfer == NeuralSettings::Transfer::srgb_linear);
            if (output_width == input.width && output_height == input.height) {
                baseline_pixels = input_baseline;
                save_png(wic, baseline_path, encoded_baseline);
            } else {
                const auto resized = resize_rgba8(wic, encoded_baseline, output_width, output_height);
                baseline_pixels = rgba8_to_float(
                    resized, settings.transfer == NeuralSettings::Transfer::srgb_linear);
                save_png(wic, baseline_path, resized);
            }
        }

        const auto output_pixels = readback_rgba(output_readback.Get(), output);
        const auto frame_path = sequence
            ? output_path / std::format(L"out_{:03}.png", frame_index)
            : output_path;
        save_png(wic, frame_path, to_rgba8(
            output_width, output_height, output_pixels,
            settings.transfer == NeuralSettings::Transfer::srgb_linear));
        if (settings.diagnostics) {
            metric_lines.push_back(print_metrics(
                baseline_pixels, output_pixels,
                std::format("Baseline vs DLSSNR frame {:03}", frame_index)));
            if (original_pixels.size() == output_pixels.size()) {
                metric_lines.push_back(print_metrics(
                    original_pixels, output_pixels,
                    std::format("Original vs DLSSNR frame {:03}", frame_index)));
            }
            if (!previous_output.empty()) {
                metric_lines.push_back(print_metrics(
                    previous_output, output_pixels,
                    std::format("DLSSNR frame {:03} vs {:03}", frame_index - 1, frame_index)));
            }
            std::vector<float> difference(output_pixels.size());
            std::transform(baseline_pixels.begin(), baseline_pixels.end(), output_pixels.begin(),
                           difference.begin(), [](float baseline, float processed) {
                               return std::abs(processed - baseline);
                           });
            const auto difference_path = sequence
                ? output_path / std::format(L"difference_{:03}.png", frame_index)
                : artifact_path(output_path, L"_difference");
            save_png(wic, difference_path,
                     to_rgba8(output_width, output_height, difference));
            previous_output = output_pixels;
        }
    }

    if (!settings.diagnostics) {
        std::cout << "Output: " << output_path.string() << "\n";
        return result;
    }
    const auto vram_after = video_memory_info(d3d);
    DXGI_ADAPTER_DESC3 adapter{};
    check_hr(d3d.adapter->GetDesc3(&adapter), "Get adapter diagnostics");
    LARGE_INTEGER driver{};
    const HRESULT driver_hr = d3d.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    const HRESULT device_reason = d3d.device->GetDeviceRemovedReason();
    std::ofstream log(log_path, std::ios::trunc);
    if (!log) throw std::runtime_error("Cannot create diagnostic log");
    log << "Windows: " << windows_version() << "\n"
        << "GPU: " << utf8(adapter.Description) << "\n"
        << std::format("PCI vendor=0x{:04X} device=0x{:04X}\n", adapter.VendorId, adapter.DeviceId)
        << "Dedicated VRAM bytes: " << adapter.DedicatedVideoMemory << "\n"
        << "DXGI driver: "
        << (SUCCEEDED(driver_hr)
                ? std::format("{}.{}.{}.{} (raw 0x{:016X})", HIWORD(driver.HighPart),
                              LOWORD(driver.HighPart), HIWORD(driver.LowPart),
                              LOWORD(driver.LowPart), static_cast<std::uint64_t>(driver.QuadPart))
                : std::format("unavailable HRESULT 0x{:08X}", static_cast<std::uint32_t>(driver_hr)))
        << "\nD3D12 feature level: 12_0 or newer"
        << "\nD3D12 debug layer: " << (debug_state.layer ? "ON" : "OFF/unavailable")
        << "\nDRED: " << (debug_state.dred ? "ON" : "OFF/unavailable")
        << "\nD3D12 device removed reason after execution: "
        << std::format("HRESULT 0x{:08X}", static_cast<std::uint32_t>(device_reason))
        << "\nDLSSNR DLL module: " << snippet.string()
        << "\nDLSSNR DLL version: " << utf8(file_version(snippet))
        << "\nImmediate-caller shim module: " << caller_path.string()
        << "\nNGX core: " << core_path.string() << " (driver, dynamically loaded)"
        << "\nNGX project ID: " << kProjectId
        << "\nNGX engine: CUSTOM 0.1; direct generic CMS/App ID: "
        << std::format("0x{:08X}", kGenericCustomAppId)
        << "\nNGX application data path: " << fs::absolute(L"logs").string()
        << "\nNGX feature search path: " << snippet.parent_path().string()
        << "\nNGX core init for parameter map: " << ngx_result(NVSDK_NGX_Result_Success)
        << "\nNGX core parameter allocation: " << ngx_result(NVSDK_NGX_Result_Success)
        << "\nCapability query: not called; direct CreateFeature is the bounded availability test"
        << "\nDLSSNR module loading: OK"
        << "\nDLSSNR direct init: " << ngx_result(NVSDK_NGX_Result_Success)
        << "\nFeature detected: YES"
        << "\nFeature create: " << ngx_result(NVSDK_NGX_Result_Success)
        << "\nFeature evaluate: " << ngx_result(result)
        << "\nNGX API version: " << NVSDK_NGX_Version_API
        << "\nFeature ID: " << static_cast<int>(kFeature)
        << "\nResource states during Evaluate: Color=NON_PIXEL_SHADER_RESOURCE "
           "Output=UNORDERED_ACCESS; Output transitions to COPY_SOURCE for readback"
        << std::format("\nInput/output: {}x{} -> {}x{} {}\n", input.width, input.height,
                       output_width, output_height, gpu_format_name(settings.gpu_format))
        << "Color conversion: " << transfer_name(settings.transfer)
        << " -> GPU texture; output uses matching inverse transfer; no tone mapping\n"
        << std::format("Resources: Color/Output={} MVec={}{} Depth={}{} ControlMask={}{}\n",
                       gpu_format_name(settings.gpu_format),
                       motion_name(settings.motion), motion ? " RG16F" : "",
                       depth_name(settings.depth), depth ? " R32F" : "",
                       mask_name(settings.control_mask), control_mask
                           ? (settings.mask_format == NeuralSettings::MaskFormat::r8 ? " R8_UNORM" : " R16_FLOAT")
                           : "")
        << "Frame index key: not supplied; no such key was observed in the DLSSNR parser\n"
        << "Frames: " << frame_count << " Reset: "
        << (reset_every_frame ? "every frame" : "first frame only")
        << " ScalingRatio=" << static_cast<double>(input.width) / output_width << "\n"
        << std::format("Controls: Preset={} Style={} Intensity={} LocalToneStrength={} "
                       "LocalStructureStrength={} SkinStructureStrength={} UseAutoMask={} "
                       "Enabled={} UICorrection={} DepthInverted={} MVecScale=({}, {}) "
                       "ControlMaskValue={}\n",
                       settings.preset, settings.style, settings.intensity, settings.local_tone,
                       settings.local_structure, settings.skin_structure, settings.auto_mask,
                       settings.enabled, settings.ui_correction, settings.depth_inverted,
                       settings.mv_scale_x, settings.mv_scale_y, settings.control_mask_value);
    if (vram_before && vram_after) {
        log << std::format("DXGI local memory before={} after={} delta={} budget={} bytes\n",
                           vram_before->CurrentUsage, vram_after->CurrentUsage,
                           static_cast<std::int64_t>(vram_after->CurrentUsage) -
                               static_cast<std::int64_t>(vram_before->CurrentUsage),
                           vram_after->Budget);
    }
    for (const auto& line : timing_lines) log << line;
    for (const auto& line : metric_lines) log << line;
    std::cout << "Artifacts: " << (sequence ? output_path.string() : output_path.parent_path().string())
              << "\n  " << original_path.string() << "\n  " << baseline_path.string()
              << "\n  " << log_path.string() << "\n";
    return result;
}

int direct_probe(const fs::path& dll_directory, const fs::path* input_path = nullptr,
                 const fs::path* output_path = nullptr, unsigned frame_count = 1,
                 bool reset_every_frame = false, bool sequence = false,
                 NeuralSettings settings = {}) {
    const fs::path snippet = fs::absolute(dll_directory / L"nvngx_dlssnr.dll");
    if (!fs::is_regular_file(snippet)) {
        throw std::runtime_error("nvngx_dlssnr.dll not found in the requested DLL directory");
    }
    const fs::path log_directory = fs::absolute(dll_directory / L"logs");
    fs::create_directories(log_directory);

    ComPtr<IWICImagingFactory> wic;
    std::optional<Rgba8Image> input;
    unsigned width = kProbeWidth;
    unsigned height = kProbeHeight;
    if (input_path) {
        wic = make_wic_factory();
        input = load_rgba8(wic.Get(), *input_path);
        width = input->width;
        height = input->height;
        if (width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            throw std::runtime_error("Input image exceeds the D3D12 2D texture dimension limit");
        }
        std::cout << std::format("Input image: {} ({}x{} RGBA8)\n", input_path->string(), width,
                                 height);
    }
    const unsigned output_width = width * settings.output_scale;
    const unsigned output_height = height * settings.output_scale;
    if (output_width / settings.output_scale != width ||
        output_height / settings.output_scale != height ||
        output_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        throw std::runtime_error("Requested output dimensions exceed the D3D12 texture limit");
    }

    const DebugState debug_state = settings.diagnostics ? enable_debug_layer() : DebugState{};
    D3D12Context d3d;
    DriverNgxCore core;
    print_system(d3d, snippet, core.module_path());

    const std::wstring search_path = fs::absolute(dll_directory).wstring();
    const wchar_t* search_paths[] = {search_path.c_str()};
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo = {search_paths, 1};
    auto result = core.init_project_id(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1", log_directory.c_str(),
        d3d.device.Get(), &common, NVSDK_NGX_Version_API);
    std::cout << "NGX core init for parameter map: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_FAILED(result)) return 2;

    NVSDK_NGX_Parameter* params = nullptr;
    result = core.allocate_parameters(&params);
    std::cout << "NGX core parameter allocation: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_FAILED(result)) return 3;

    const fs::path caller_path = executable_directory() / L"caller" / L"nvngx.dll";
    NgxCaller caller(caller_path);
    DirectNgx ngx(snippet);
    std::wcout << L"Unmodified caller shim: " << caller_path << L"\n";
    std::cout << "DLSSNR DLL loading: OK\n";

    result = caller.init(reinterpret_cast<FARPROC>(ngx.init), kGenericCustomAppId,
                         log_directory.c_str(), d3d.device.Get(), NVSDK_NGX_Version_API, nullptr);
    std::cout << "DLSSNR direct init: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_FAILED(result)) {
        core.destroy_parameters(params);
        return 4;
    }
    set_create_params(params, width, height, output_width, output_height, settings.preset);

    NVSDK_NGX_Handle* handle = nullptr;
    result = caller.create(reinterpret_cast<FARPROC>(ngx.create), d3d.command_list.Get(),
                           kFeature, params, &handle);
    std::cout << "DLSSNR feature detected: " << (NVSDK_NGX_SUCCEED(result) ? "YES" : "NO")
              << "\nDLSSNR direct feature creation: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_SUCCEED(result)) {
        d3d.execute_and_wait();
        result = input ? evaluate_image(d3d, ngx, caller, handle, params, wic.Get(), *input,
                                        *output_path, snippet, caller_path, core.module_path(),
                                        debug_state,
                                        output_width, output_height,
                                        frame_count, reset_every_frame, sequence, settings)
                       : evaluate_synthetic(d3d, ngx, caller, handle, params);
    }

    NVSDK_NGX_Result release_result = NVSDK_NGX_Result_Success;
    if (handle) release_result = caller.release(reinterpret_cast<FARPROC>(ngx.release), handle);
    if (handle) std::cout << "DLSSNR direct feature release: " << ngx_result(release_result) << "\n";
    core.destroy_parameters(params);
    const auto shutdown_result = caller.shutdown(reinterpret_cast<FARPROC>(ngx.shutdown), d3d.device.Get());
    std::cout << "DLSSNR direct shutdown: " << ngx_result(shutdown_result) << "\n";
    // ponytail: process-exit cleanup avoids a reproducible core Shutdown1 hang; remove when a
    // public NGX core maps feature 18 and shuts this hybrid parameter-map path down cleanly.
    std::cout << "NGX core shutdown: SKIPPED (Shutdown1 hangs on this driver/core failure path)\n";
    if (input && output_path && handle && settings.diagnostics) {
        const fs::path log_path = sequence ? *output_path / L"diagnostic.log"
                                           : artifact_path(*output_path, L"_diagnostic", L".log");
        std::ofstream log(log_path, std::ios::app);
        if (!log) throw std::runtime_error("Cannot append diagnostic cleanup results");
        log << "Overall image evaluation result: " << ngx_result(result)
            << "\nFeature release: " << ngx_result(release_result)
            << "\nDLSSNR direct shutdown: " << ngx_result(shutdown_result)
            << "\nNGX core parameter destruction: completed"
            << "\nNGX core shutdown: SKIPPED (reproducible Shutdown1 hang on this hybrid path)\n";
    }
    if (NVSDK_NGX_FAILED(result)) return 5;
    return NVSDK_NGX_FAILED(release_result) || NVSDK_NGX_FAILED(shutdown_result) ? 6 : 0;
}

int core_probe(const fs::path& dll_directory) {
    const fs::path snippet = fs::absolute(dll_directory / L"nvngx_dlssnr.dll");
    if (!fs::is_regular_file(snippet)) {
        throw std::runtime_error("nvngx_dlssnr.dll not found in the requested DLL directory");
    }
    const fs::path log_directory = fs::absolute(dll_directory / L"logs");
    fs::create_directories(log_directory);

    (void)enable_debug_layer();
    D3D12Context d3d;
    DriverNgxCore core;
    print_system(d3d, snippet, core.module_path());

    const std::wstring search_path = fs::absolute(dll_directory).wstring();
    const wchar_t* search_paths[] = {search_path.c_str()};
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo = {search_paths, 1};
    common.LoggingInfo.LoggingCallback = ngx_log;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = false;

    // NVSDK_NGX_D3D12_GetFeatureRequirements(Reserved18) does not return with the
    // 310.7 public core on this host. Creation below is the bounded availability test.
    auto result = core.init_project_id(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1", log_directory.c_str(),
        d3d.device.Get(), &common, NVSDK_NGX_Version_API);
    std::cout << "NGX init: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_FAILED(result)) return 2;

    NVSDK_NGX_Parameter* capabilities = nullptr;
    result = core.get_capability_parameters(&capabilities);
    std::cout << "NGX capability parameters: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_FAILED(result)) {
        return 3;
    }

    NVSDK_NGX_Parameter* params = nullptr;
    result = core.allocate_parameters(&params);
    std::cout << "NGX parameter allocation: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_FAILED(result)) {
        core.destroy_parameters(capabilities);
        return 4;
    }

    set_create_params(params, kProbeWidth, kProbeHeight, kProbeWidth, kProbeHeight);

    NVSDK_NGX_Handle* handle = nullptr;
    result = core.create_feature(d3d.command_list.Get(), kFeature, params, &handle);
    std::cout << "DLSSNR feature detected: " << (NVSDK_NGX_SUCCEED(result) ? "YES" : "NO")
              << "\nDLSSNR feature creation: " << ngx_result(result) << "\n";
    if (NVSDK_NGX_SUCCEED(result)) d3d.execute_and_wait();

    if (handle) {
        const auto release = core.release_feature(handle);
        std::cout << "DLSSNR feature release: " << ngx_result(release) << "\n";
    }
    core.destroy_parameters(params);
    core.destroy_parameters(capabilities);
    std::cout << "NGX shutdown: SKIPPED (process-lifetime driver core)\n";
    return NVSDK_NGX_SUCCEED(result) ? 0 : 5;
}

float parse_float(std::wstring_view text, float minimum, float maximum, std::string_view name) {
    std::size_t used = 0;
    const float value = std::stof(std::wstring(text), &used);
    if (used != text.size() || !std::isfinite(value) || value < minimum || value > maximum) {
        throw std::runtime_error(std::format("{} must be a finite value in [{}, {}]", name,
                                             minimum, maximum));
    }
    return value;
}

NeuralSettings parse_settings(int argc, wchar_t** argv, int first) {
    NeuralSettings settings;
    for (int i = first; i < argc; i += 2) {
        if (i + 1 == argc) throw std::runtime_error("Missing value for the final control option");
        const std::wstring_view option = argv[i];
        const std::wstring_view value = argv[i + 1];
        if (option == L"--style") {
            if (value == L"default") settings.style = 0;
            else if (value == L"natural") settings.style = 1;
            else if (value == L"cinematic") settings.style = 2;
            else {
                std::size_t used = 0;
                const auto style = std::stoul(std::wstring(value), &used);
                if (used != value.size() || style > 6) {
                    throw std::runtime_error("--style must be default, natural, cinematic, or 0..6");
                }
                settings.style = static_cast<unsigned>(style);
            }
        } else if (option == L"--preset") {
            std::size_t used = 0;
            const auto preset = std::stoul(std::wstring(value), &used);
            if (used != value.size() || preset > 3) throw std::runtime_error("--preset must be 0..3");
            settings.preset = static_cast<unsigned>(preset);
        } else if (option == L"--intensity") {
            settings.intensity = parse_float(value, 0.0f, 2.0f, "--intensity");
        } else if (option == L"--tone") {
            settings.local_tone = parse_float(value, 0.0f, 2.0f, "--tone");
        } else if (option == L"--structure") {
            settings.local_structure = parse_float(value, 0.0f, 2.0f, "--structure");
        } else if (option == L"--skin") {
            settings.skin_structure = parse_float(value, -1.0f, 2.0f, "--skin");
        } else if (option == L"--auto-mask") {
            if (value != L"0" && value != L"1") throw std::runtime_error("--auto-mask must be 0 or 1");
            settings.auto_mask = value == L"1";
        } else if (option == L"--enabled") {
            if (value != L"0" && value != L"1") throw std::runtime_error("--enabled must be 0 or 1");
            settings.enabled = value == L"1";
        } else if (option == L"--ui-correction") {
            if (value != L"0" && value != L"1") throw std::runtime_error("--ui-correction must be 0 or 1");
            settings.ui_correction = value == L"1";
        } else if (option == L"--depth-inverted") {
            if (value != L"0" && value != L"1") throw std::runtime_error("--depth-inverted must be 0 or 1");
            settings.depth_inverted = value == L"1";
        } else if (option == L"--mv") {
            if (value == L"absent") settings.motion = NeuralSettings::Motion::absent;
            else if (value == L"zero") settings.motion = NeuralSettings::Motion::zero;
            else if (value == L"tiny") settings.motion = NeuralSettings::Motion::tiny;
            else throw std::runtime_error("--mv must be absent, zero, or tiny");
        } else if (option == L"--depth") {
            if (value == L"absent") settings.depth = NeuralSettings::Depth::absent;
            else if (value == L"zero") settings.depth = NeuralSettings::Depth::zero;
            else if (value == L"one") settings.depth = NeuralSettings::Depth::one;
            else throw std::runtime_error("--depth must be absent, zero, or one");
        } else if (option == L"--transfer") {
            if (value == L"code") settings.transfer = NeuralSettings::Transfer::code_values;
            else if (value == L"srgb-linear") settings.transfer = NeuralSettings::Transfer::srgb_linear;
            else throw std::runtime_error("--transfer must be code or srgb-linear");
        } else if (option == L"--gpu-format") {
            if (value == L"fp16") settings.gpu_format = NeuralSettings::GpuFormat::rgba16f;
            else if (value == L"rgba8") settings.gpu_format = NeuralSettings::GpuFormat::rgba8;
            else throw std::runtime_error("--gpu-format must be fp16 or rgba8");
        } else if (option == L"--control-mask") {
            if (value == L"absent") settings.control_mask = NeuralSettings::Mask::absent;
            else if (value == L"zero") settings.control_mask = NeuralSettings::Mask::zero;
            else if (value == L"one") settings.control_mask = NeuralSettings::Mask::one;
            else if (value == L"half") settings.control_mask = NeuralSettings::Mask::half;
            else throw std::runtime_error("--control-mask must be absent, zero, one, or half");
        } else if (option == L"--control-mask-format") {
            if (value == L"r8") settings.mask_format = NeuralSettings::MaskFormat::r8;
            else if (value == L"r16f") settings.mask_format = NeuralSettings::MaskFormat::r16f;
            else throw std::runtime_error("--control-mask-format must be r8 or r16f");
        } else if (option == L"--control-mask-value") {
            settings.control_mask = NeuralSettings::Mask::one;
            settings.control_mask_value = parse_float(value, -16.0f, 16.0f,
                                                       "--control-mask-value");
        } else if (option == L"--output-scale") {
            if (value != L"1" && value != L"2") {
                throw std::runtime_error("--output-scale must be 1 or 2");
            }
            settings.output_scale = value == L"2" ? 2u : 1u;
        } else if (option == L"--mv-scale-x") {
            settings.mv_scale_x = parse_float(value, -2.0f, 2.0f, "--mv-scale-x");
        } else if (option == L"--mv-scale-y") {
            settings.mv_scale_y = parse_float(value, -2.0f, 2.0f, "--mv-scale-y");
        } else if (option == L"--diagnostics") {
            if (value != L"0" && value != L"1") {
                throw std::runtime_error("--diagnostics must be 0 or 1");
            }
            settings.diagnostics = value == L"1";
        } else {
            throw std::runtime_error("Unknown neural control option");
        }
    }
    return settings;
}

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComApartment() {
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) check_hr(result, "CoInitializeEx");
    }
    ~ComApartment() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::cout << std::unitbuf;
        std::clog << std::unitbuf;
        std::wcout << std::unitbuf;
        const std::wstring_view mode = argc >= 2 ? argv[1] : L"";
        if (mode == L"--direct-probe" || mode == L"--core-probe") {
            if (argc > 3) throw std::runtime_error("Too many probe arguments");
            const fs::path dll_directory =
                argc == 3 ? fs::path(argv[2]) : default_runtime_directory();
            return mode == L"--direct-probe" ? direct_probe(dll_directory)
                                              : core_probe(dll_directory);
        }
        if (mode == L"--sequence") {
            if (argc < 6) throw std::runtime_error("--sequence expects input, output directory, frames, first|always");
            ComApartment com;
            const fs::path input = fs::absolute(argv[2]);
            const fs::path output_directory = fs::absolute(argv[3]);
            const unsigned frames = std::stoul(argv[4]);
            if (frames < 2 || frames > 64) throw std::runtime_error("Sequence frame count must be 2..64");
            const std::wstring_view reset_mode = argv[5];
            if (reset_mode != L"first" && reset_mode != L"always") {
                throw std::runtime_error("Sequence reset mode must be first or always");
            }
            return direct_probe(default_runtime_directory(), &input, &output_directory, frames,
                                reset_mode == L"always", true, parse_settings(argc, argv, 6));
        }
        if (argc >= 3 && !mode.starts_with(L"--")) {
            ComApartment com;
            const fs::path input = fs::absolute(argv[1]);
            const fs::path output = fs::absolute(argv[2]);
            return direct_probe(default_runtime_directory(), &input, &output, 1, false, false,
                                parse_settings(argc, argv, 3));
        }
        std::wcerr << L"Usage:\n"
                      L"  dlssnr-image.exe input.png output.png\n"
                      L"    [--preset 0..3] [--style default|natural|cinematic|0..6]\n"
                      L"    [--intensity 0..2] [--tone 0..2]\n"
                      L"    [--structure 0..2] [--skin -1..2] [--auto-mask 0|1]\n"
                      L"    [--enabled 0|1] [--ui-correction 0|1]\n"
                      L"    [--mv absent|zero|tiny] [--mv-scale-x N] [--mv-scale-y N]\n"
                      L"    [--depth absent|zero|one] [--depth-inverted 0|1]\n"
                      L"    [--transfer code|srgb-linear]\n"
                      L"    [--gpu-format fp16|rgba8]\n"
                      L"    [--control-mask absent|zero|one|half] [--control-mask-format r8|r16f]\n"
                       L"    [--control-mask-value -16..16]\n"
                       L"    [--diagnostics 0|1]  (0 saves only the requested output)\n"
                       L"    [--output-scale 1|2]  (2 is an experimental failure-boundary probe)\n"
                      L"  dlssnr-image.exe --sequence input.png output-dir frames first|always [controls]\n"
                      L"  dlssnr-image.exe --direct-probe|--core-probe "
                      L"[directory containing nvngx_dlssnr.dll]\n";
        return 64;
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << "\n";
        return 1;
    }
}

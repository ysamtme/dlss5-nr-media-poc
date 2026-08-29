#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "gui_processor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr int kFramesInFlight = 2;
constexpr int kBackBuffers = 2;
constexpr int kSrvHeapSize = 64;

void check_hr(HRESULT hr, std::string_view operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::format("{} failed: HRESULT 0x{:08X}", operation,
                                             static_cast<std::uint32_t>(hr)));
    }
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (!size) return "<path conversion failed>";
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring quote_arg(std::wstring_view value) {
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(ch);
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

struct FrameContext {
    ID3D12CommandAllocator* allocator = nullptr;
    UINT64 fence_value = 0;
};

struct DescriptorAllocator {
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    UINT increment = 0;
    std::vector<int> free_indices;

    void create(ID3D12Device* device, ID3D12DescriptorHeap* descriptor_heap) {
        heap = descriptor_heap;
        const auto desc = heap->GetDesc();
        cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
        gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
        increment = device->GetDescriptorHandleIncrementSize(desc.Type);
        free_indices.reserve(desc.NumDescriptors);
        for (int i = static_cast<int>(desc.NumDescriptors) - 1; i >= 0; --i) {
            free_indices.push_back(i);
        }
    }

    void alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        if (free_indices.empty()) throw std::runtime_error("D3D12 SRV descriptor heap exhausted");
        const int index = free_indices.back();
        free_indices.pop_back();
        cpu->ptr = cpu_start.ptr + static_cast<SIZE_T>(index) * increment;
        gpu->ptr = gpu_start.ptr + static_cast<UINT64>(index) * increment;
    }

    void free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
        const auto cpu_index = static_cast<int>((cpu.ptr - cpu_start.ptr) / increment);
        const auto gpu_index = static_cast<int>((gpu.ptr - gpu_start.ptr) / increment);
        if (cpu_index != gpu_index) throw std::runtime_error("Mismatched D3D12 descriptor handles");
        free_indices.push_back(cpu_index);
    }
};

FrameContext g_frames[kFramesInFlight]{};
UINT g_frame_index = 0;
ID3D12Device* g_device = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
ID3D12DescriptorHeap* g_srv_heap = nullptr;
DescriptorAllocator g_srv_allocator;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12GraphicsCommandList* g_command_list = nullptr;
ID3D12CommandAllocator* g_upload_allocator = nullptr;
ID3D12GraphicsCommandList* g_upload_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;
UINT64 g_last_fence = 0;
IDXGISwapChain3* g_swap_chain = nullptr;
bool g_swap_chain_occluded = false;
HANDLE g_swap_chain_waitable = nullptr;
ID3D12Resource* g_render_targets[kBackBuffers]{};
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv_handles[kBackBuffers]{};
std::vector<fs::path> g_pending_drops;

std::vector<fs::path> paths_from_hdrop(HDROP drop) {
    std::vector<fs::path> paths;
    if (!drop) return paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        path.resize(length);
        paths.emplace_back(path);
    }
    return paths;
}

void wait_for_gpu() {
    if (!g_queue || !g_fence) return;
    check_hr(g_queue->Signal(g_fence, ++g_last_fence), "Signal GUI fence");
    check_hr(g_fence->SetEventOnCompletion(g_last_fence, g_fence_event),
             "Set GUI fence event");
    WaitForSingleObject(g_fence_event, INFINITE);
}

void create_render_targets() {
    for (UINT i = 0; i < kBackBuffers; ++i) {
        check_hr(g_swap_chain->GetBuffer(i, IID_PPV_ARGS(&g_render_targets[i])),
                 "Get swap-chain buffer");
        g_device->CreateRenderTargetView(g_render_targets[i], nullptr, g_rtv_handles[i]);
    }
}

void cleanup_render_targets() {
    wait_for_gpu();
    for (auto*& target : g_render_targets) {
        if (target) target->Release();
        target = nullptr;
    }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.BufferCount = kBackBuffers;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.Scaling = DXGI_SCALING_STRETCH;

    ComPtr<IDXGIFactory6> factory;
    if (CreateDXGIFactory1(IID_PPV_ARGS(&factory)) != S_OK) return false;
    ComPtr<IDXGIAdapter4> adapter;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))); ++i) {
        DXGI_ADAPTER_DESC3 desc{};
        if (SUCCEEDED(adapter->GetDesc3(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) &&
            desc.VendorId == 0x10DE &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&g_device)))) break;
        adapter.Reset();
    }
    if (!g_device) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kBackBuffers;
    rtv_desc.NodeMask = 1;
    if (g_device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_rtv_heap)) != S_OK) return false;
    const SIZE_T rtv_increment =
        g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBackBuffers; ++i) {
        g_rtv_handles[i] = rtv;
        rtv.ptr += rtv_increment;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = kSrvHeapSize;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (g_device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_srv_heap)) != S_OK) return false;
    g_srv_allocator.create(g_device, g_srv_heap);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.NodeMask = 1;
    if (g_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&g_queue)) != S_OK) return false;
    for (auto& frame : g_frames) {
        if (g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&frame.allocator)) != S_OK) return false;
    }
    if (g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator,
                                    nullptr, IID_PPV_ARGS(&g_command_list)) != S_OK ||
        g_command_list->Close() != S_OK) return false;
    if (g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&g_upload_allocator)) != S_OK) return false;
    if (g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_upload_allocator,
                                    nullptr, IID_PPV_ARGS(&g_upload_list)) != S_OK ||
        g_upload_list->Close() != S_OK) return false;
    if (g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK) return false;
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event) return false;

    IDXGISwapChain1* swap_chain = nullptr;
    const HRESULT swap_hr = factory->CreateSwapChainForHwnd(g_queue, hwnd, &swap_desc, nullptr,
                                                            nullptr, &swap_chain);
    if (SUCCEEDED(swap_hr)) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    if (FAILED(swap_hr)) return false;
    if (swap_chain->QueryInterface(IID_PPV_ARGS(&g_swap_chain)) != S_OK) {
        swap_chain->Release();
        return false;
    }
    swap_chain->Release();
    g_swap_chain->SetMaximumFrameLatency(kBackBuffers);
    g_swap_chain_waitable = g_swap_chain->GetFrameLatencyWaitableObject();
    create_render_targets();
    return true;
}

void cleanup_device() {
    cleanup_render_targets();
    if (g_swap_chain) {
        g_swap_chain->SetFullscreenState(FALSE, nullptr);
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_swap_chain_waitable) CloseHandle(g_swap_chain_waitable);
    g_swap_chain_waitable = nullptr;
    for (auto& frame : g_frames) {
        if (frame.allocator) frame.allocator->Release();
        frame = {};
    }
    if (g_upload_list) g_upload_list->Release();
    if (g_upload_allocator) g_upload_allocator->Release();
    if (g_command_list) g_command_list->Release();
    if (g_queue) g_queue->Release();
    if (g_rtv_heap) g_rtv_heap->Release();
    if (g_srv_heap) g_srv_heap->Release();
    if (g_fence) g_fence->Release();
    if (g_fence_event) CloseHandle(g_fence_event);
    if (g_device) g_device->Release();
    g_upload_list = nullptr;
    g_upload_allocator = nullptr;
    g_command_list = nullptr;
    g_queue = nullptr;
    g_rtv_heap = nullptr;
    g_srv_heap = nullptr;
    g_fence = nullptr;
    g_fence_event = nullptr;
    g_device = nullptr;
}

FrameContext* wait_for_next_frame() {
    auto* frame = &g_frames[g_frame_index % kFramesInFlight];
    if (g_fence->GetCompletedValue() < frame->fence_value) {
        check_hr(g_fence->SetEventOnCompletion(frame->fence_value, g_fence_event),
                 "Wait for GUI frame fence");
        HANDLE handles[] = {g_swap_chain_waitable, g_fence_event};
        WaitForMultipleObjects(2, handles, TRUE, INFINITE);
    } else {
        WaitForSingleObject(g_swap_chain_waitable, INFINITE);
    }
    return frame;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        for (auto& path : paths_from_hdrop(drop)) g_pending_drops.push_back(std::move(path));
        DragFinish(drop);
        return 0;
    }
    case WM_SIZE:
        if (g_device && wparam != SIZE_MINIMIZED) {
            cleanup_render_targets();
            DXGI_SWAP_CHAIN_DESC1 desc{};
            g_swap_chain->GetDesc1(&desc);
            if (SUCCEEDED(g_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam),
                                                       desc.Format, desc.Flags))) {
                create_render_targets();
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0u) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

using ImageData = LiveImage;

ComPtr<IWICImagingFactory> g_wic;

ImageData decode_source(IWICBitmapSource* source) {
    ImageData image;
    check_hr(source->GetSize(&image.width, &image.height), "Read preview dimensions");
    if (!image.width || !image.height || image.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        image.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        throw std::runtime_error("Preview image dimensions are unsupported");
    }
    ComPtr<IWICBitmapSource> converted;
    check_hr(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, source, &converted),
             "Convert preview to RGBA8");
    const std::size_t row_size = static_cast<std::size_t>(image.width) * 4;
    if (row_size > std::numeric_limits<UINT>::max() ||
        image.height > std::numeric_limits<std::size_t>::max() / row_size) {
        throw std::runtime_error("Preview image is too large");
    }
    image.rgba.resize(row_size * image.height);
    check_hr(converted->CopyPixels(nullptr, static_cast<UINT>(row_size),
                                    static_cast<UINT>(image.rgba.size()), image.rgba.data()),
             "Copy preview pixels");
    return image;
}

ImageData decode_image(const fs::path& path) {
    ComPtr<IWICBitmapDecoder> decoder;
    check_hr(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnLoad, &decoder),
             "Open preview image");
    ComPtr<IWICBitmapFrameDecode> frame;
    check_hr(decoder->GetFrame(0, &frame), "Decode preview frame");
    return decode_source(frame.Get());
}

std::optional<std::vector<fs::path>> file_paths_from_clipboard(HWND owner) {
    if (!OpenClipboard(owner)) {
        throw std::runtime_error("Could not open the Windows clipboard");
    }
    try {
        if (!IsClipboardFormatAvailable(CF_HDROP)) {
            CloseClipboard();
            return std::nullopt;
        }
        const HDROP drop = reinterpret_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (!drop) throw std::runtime_error("Could not read file paths from the clipboard");
        auto paths = paths_from_hdrop(drop);
        CloseClipboard();
        return paths;
    } catch (...) {
        CloseClipboard();
        throw;
    }
}

ImageData image_from_clipboard(HWND owner) {
    if (!OpenClipboard(owner)) {
        throw std::runtime_error("Could not open the Windows clipboard");
    }
    try {
        const HBITMAP bitmap = reinterpret_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
        if (!bitmap) throw std::runtime_error("Clipboard does not contain an image");
        ComPtr<IWICBitmap> source;
        check_hr(g_wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source),
                 "Import clipboard bitmap");
        ImageData image = decode_source(source.Get());
        CloseClipboard();
        return image;
    } catch (...) {
        CloseClipboard();
        throw;
    }
}

void save_png(const fs::path& path, unsigned width, unsigned height,
              const std::vector<std::uint8_t>& rgba) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    ComPtr<IWICStream> stream;
    check_hr(g_wic->CreateStream(&stream), "Create PNG output stream");
    check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open PNG output");
    ComPtr<IWICBitmapEncoder> encoder;
    check_hr(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
             "Create PNG encoder");
    check_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
             "Initialize PNG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    check_hr(encoder->CreateNewFrame(&frame, &properties), "Create PNG frame");
    check_hr(frame->Initialize(properties.Get()), "Initialize PNG frame");
    check_hr(frame->SetSize(width, height), "Set PNG size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check_hr(frame->SetPixelFormat(&format), "Set PNG format");
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        throw std::runtime_error("WIC PNG encoder rejected BGRA8");
    }
    auto bgra = rgba;
    for (std::size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    check_hr(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data()),
             "Write PNG pixels");
    check_hr(frame->Commit(), "Commit PNG frame");
    check_hr(encoder->Commit(), "Commit PNG file");
}

void validate_comparison_inputs(const ImageData& input, const ImageData& output) {
    const std::size_t expected = static_cast<std::size_t>(input.width) * input.height * 4;
    if (!input.width || !input.height || input.width != output.width ||
        input.height != output.height || input.rgba.size() != expected ||
        output.rgba.size() != expected) {
        throw std::runtime_error("Comparison export requires equal non-empty input/output sizes");
    }
}

ImageData make_side_by_side(const ImageData& input, const ImageData& output) {
    validate_comparison_inputs(input, output);
    if (input.width > std::numeric_limits<unsigned>::max() / 2) {
        throw std::runtime_error("Side-by-side comparison width is too large");
    }
    ImageData comparison;
    comparison.width = input.width * 2;
    comparison.height = input.height;
    const std::size_t row = static_cast<std::size_t>(input.width) * 4;
    comparison.rgba.resize(row * 2 * input.height);
    for (unsigned y = 0; y < input.height; ++y) {
        const std::size_t source = static_cast<std::size_t>(y) * row;
        const std::size_t target = source * 2;
        std::copy_n(input.rgba.begin() + source, row, comparison.rgba.begin() + target);
        std::copy_n(output.rgba.begin() + source, row, comparison.rgba.begin() + target + row);
    }
    return comparison;
}

ImageData make_wipe(const ImageData& input, const ImageData& output, float position) {
    validate_comparison_inputs(input, output);
    ImageData comparison{input.width, input.height, input.rgba};
    const unsigned split = static_cast<unsigned>(
        std::lround(std::clamp(position, 0.0f, 1.0f) * input.width));
    const unsigned line_begin = split > 0 ? split - 1 : 0;
    const unsigned line_end = std::min(input.width, split + 1);
    for (unsigned y = 0; y < input.height; ++y) {
        for (unsigned x = 0; x < input.width; ++x) {
            const std::size_t pixel = (static_cast<std::size_t>(y) * input.width + x) * 4;
            if (x >= line_begin && x < line_end) {
                comparison.rgba[pixel + 0] = 255;
                comparison.rgba[pixel + 1] = 255;
                comparison.rgba[pixel + 2] = 255;
                comparison.rgba[pixel + 3] = 255;
            } else if (x < split) {
                std::copy_n(output.rgba.begin() + pixel, 4, comparison.rgba.begin() + pixel);
            }
        }
    }
    return comparison;
}

struct PreviewTexture {
    ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool encode_srgb = false;

    bool valid() const { return resource != nullptr; }
};

void clear_texture(PreviewTexture& texture) {
    if (texture.gpu.ptr) g_srv_allocator.free(texture.cpu, texture.gpu);
    texture = {};
}

void reset_fixed_texture(PreviewTexture& texture) {
    const auto cpu = texture.cpu;
    const auto gpu = texture.gpu;
    texture = {};
    texture.cpu = cpu;
    texture.gpu = gpu;
}

PreviewTexture upload_texture(const ImageData& image) {
    PreviewTexture texture;
    texture.width = image.width;
    texture.height = image.height;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = texture.width;
    desc.Height = texture.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    check_hr(g_device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&texture.resource)),
             "Create preview texture");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 upload_size = 0;
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &upload_size);
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    ComPtr<ID3D12Resource> upload;
    check_hr(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&upload)),
             "Create preview upload buffer");
    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check_hr(upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped)),
             "Map preview upload buffer");
    const std::size_t source_stride = static_cast<std::size_t>(texture.width) * 4;
    for (UINT row = 0; row < rows; ++row) {
        std::copy_n(image.rgba.data() + static_cast<std::size_t>(row) * source_stride,
                    source_stride,
                    mapped + footprint.Offset + static_cast<std::size_t>(row) *
                                                  footprint.Footprint.RowPitch);
    }
    upload->Unmap(0, nullptr);

    wait_for_gpu();
    check_hr(g_upload_allocator->Reset(), "Reset preview upload allocator");
    check_hr(g_upload_list->Reset(g_upload_allocator, nullptr), "Reset preview upload list");
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_upload_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    g_upload_list->ResourceBarrier(1, &barrier);
    check_hr(g_upload_list->Close(), "Close preview upload list");
    ID3D12CommandList* lists[] = {g_upload_list};
    g_queue->ExecuteCommandLists(1, lists);
    wait_for_gpu();

    g_srv_allocator.alloc(&texture.cpu, &texture.gpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(texture.resource.Get(), &srv, texture.cpu);
    return texture;
}

struct GpuPreviewCompute {
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT descriptor_size = 0;
    D3D12_RESOURCE_STATES converted_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES difference_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool conversion_dirty = false;
    bool difference_dirty = false;

    void initialize(ID3D12Device* device) {
        if (pipeline) return;
        static constexpr char shader[] = R"(
Texture2D<float4> SourceA : register(t0);
Texture2D<float4> SourceB : register(t1);
RWTexture2D<float4> Target : register(u0);
cbuffer Params : register(b0) { float Gain; uint Operation; uint EncodeB; };

float3 LinearToSrgb(float3 value) {
    value = max(value, 0.0);
    return lerp(value * 12.92, 1.055 * pow(value, 1.0 / 2.4) - 0.055,
                step(0.0031308, value));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    Target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float4 a = SourceA.Load(int3(id.xy, 0));
    if (Operation == 0) {
        Target[id.xy] = float4(saturate(LinearToSrgb(a.rgb)), saturate(a.a));
        return;
    }
    float4 b = SourceB.Load(int3(id.xy, 0));
    if (EncodeB != 0) b.rgb = LinearToSrgb(b.rgb);
    Target[id.xy] = float4(saturate(abs(a.rgb - b.rgb) * Gain), 1.0);
}
)";
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        const HRESULT compile = D3DCompile(shader, sizeof(shader) - 1, "gpu-preview", nullptr,
                                            nullptr, "main", "cs_5_0",
                                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                            &bytecode, &errors);
        if (FAILED(compile)) {
            const char* message = errors
                ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown shader error";
            throw std::runtime_error(std::format("Compile GPU preview shader: {}", message));
        }

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &ranges[0]};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &ranges[1]};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants = {0, 0, 3};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = static_cast<UINT>(std::size(params));
        root_desc.pParameters = params;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> serialize_errors;
        const HRESULT serialized_hr = D3D12SerializeRootSignature(
            &root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &serialize_errors);
        if (FAILED(serialized_hr)) {
            const char* message = serialize_errors
                ? static_cast<const char*>(serialize_errors->GetBufferPointer())
                : "unknown root-signature error";
            throw std::runtime_error(std::format("Serialize GPU preview root signature: {}",
                                                 message));
        }
        check_hr(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&root_signature)),
                 "Create GPU preview root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
        pipeline_desc.pRootSignature = root_signature.Get();
        pipeline_desc.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
        check_hr(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)),
                 "Create GPU preview pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 12;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors)),
                 "Create GPU preview descriptors");
        descriptor_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned index) const {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptor_size;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned index) const {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptor_size;
        return handle;
    }

    void create_target(ID3D12Device* device, PreviewTexture& target, unsigned width,
                       unsigned height, std::array<unsigned, 2> uav_indices,
                       D3D12_RESOURCE_STATES& state) {
        if (target.valid() && target.width == width && target.height == height) return;
        reset_fixed_texture(target);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(&target.resource)),
                 "Create GPU comparison texture");
        target.width = width;
        target.height = height;
        target.format = desc.Format;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(target.resource.Get(), &srv, target.cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = desc.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (const unsigned index : uav_indices) {
            device->CreateUnorderedAccessView(target.resource.Get(), nullptr, &uav, cpu(index));
        }
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    void set_sources(ID3D12Device* device, const PreviewTexture& input,
                     const std::array<PreviewTexture, 2>& outputs) {
        initialize(device);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        for (unsigned slot = 0; slot < outputs.size(); ++slot) {
            const unsigned base = slot * 6;
            srv.Format = outputs[slot].format;
            device->CreateShaderResourceView(outputs[slot].resource.Get(), &srv, cpu(base));
            device->CreateShaderResourceView(outputs[slot].resource.Get(), &srv, cpu(base + 1));
            srv.Format = input.format;
            device->CreateShaderResourceView(input.resource.Get(), &srv, cpu(base + 3));
            srv.Format = outputs[slot].format;
            device->CreateShaderResourceView(outputs[slot].resource.Get(), &srv, cpu(base + 4));
        }
        conversion_dirty = outputs[0].encode_srgb;
        difference_dirty = true;
    }

    void ensure_conversion(ID3D12Device* device, PreviewTexture& target, unsigned width,
                           unsigned height) {
        initialize(device);
        const bool changed = !target.valid() || target.width != width || target.height != height;
        create_target(device, target, width, height, {2, 8}, converted_state);
        if (changed) conversion_dirty = true;
    }

    void ensure_difference(ID3D12Device* device, PreviewTexture& target, unsigned width,
                           unsigned height) {
        initialize(device);
        const bool changed = !target.valid() || target.width != width || target.height != height;
        create_target(device, target, width, height, {5, 11}, difference_state);
        if (changed) difference_dirty = true;
    }

    static void transition_target(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }

    void dispatch(ID3D12GraphicsCommandList* list, unsigned descriptor_base,
                  PreviewTexture& target, D3D12_RESOURCE_STATES& state, float gain,
                  unsigned operation, unsigned encode_b) {
        if (state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            transition_target(list, target.resource.Get(), state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        list->SetComputeRootDescriptorTable(0, gpu(descriptor_base));
        list->SetComputeRootDescriptorTable(1, gpu(descriptor_base + 2));
        const struct { float gain; unsigned operation; unsigned encode_b; } constants{
            gain, operation, encode_b};
        list->SetComputeRoot32BitConstants(2, 3, &constants, 0);
        list->Dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);
        transition_target(list, target.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }

    void record(ID3D12GraphicsCommandList* list, PreviewTexture& converted,
                PreviewTexture& difference, bool need_conversion, bool need_difference,
                float gain, bool encode_output, unsigned output_slot) {
        if ((!need_conversion || !conversion_dirty) &&
            (!need_difference || !difference_dirty)) return;
        ID3D12DescriptorHeap* heaps[] = {descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_signature.Get());
        list->SetPipelineState(pipeline.Get());
        const unsigned slot_base = output_slot * 6;
        if (need_conversion && conversion_dirty) {
            dispatch(list, slot_base, converted, converted_state, 1.0f, 0, 0);
            conversion_dirty = false;
        }
        if (need_difference && difference_dirty) {
            dispatch(list, slot_base + 3, difference, difference_state, gain, 1,
                     encode_output ? 1u : 0u);
            difference_dirty = false;
        }
    }

    void reset(PreviewTexture& converted, PreviewTexture& difference) {
        reset_fixed_texture(converted);
        reset_fixed_texture(difference);
        converted_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        difference_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        conversion_dirty = false;
        difference_dirty = false;
    }
};

std::optional<fs::path> shell_item_path(IShellItem* item) {
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return std::nullopt;
    fs::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

std::vector<fs::path> open_image_dialog(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    check_hr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)),
             "Create file-open dialog");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    const COMDLG_FILTERSPEC filters[] = {
        {L"PNG and JPEG images", L"*.png;*.jpg;*.jpeg"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    if (dialog->Show(owner) == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    ComPtr<IShellItemArray> items;
    check_hr(dialog->GetResults(&items), "Get selected images");
    DWORD count = 0;
    items->GetCount(&count);
    std::vector<fs::path> paths;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(items->GetItemAt(i, &item))) {
            if (auto path = shell_item_path(item.Get())) paths.push_back(*path);
        }
    }
    return paths;
}

std::optional<fs::path> open_folder_dialog(HWND owner, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dialog;
    check_hr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)),
             "Create folder dialog");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    if (dialog->Show(owner) == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    ComPtr<IShellItem> item;
    check_hr(dialog->GetResult(&item), "Get selected folder");
    return shell_item_path(item.Get());
}

std::optional<fs::path> save_image_dialog(HWND owner, const std::wstring& filename) {
    ComPtr<IFileSaveDialog> dialog;
    check_hr(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)),
             "Create image-save dialog");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    const COMDLG_FILTERSPEC filter{L"PNG image", L"*.png"};
    dialog->SetFileTypes(1, &filter);
    dialog->SetDefaultExtension(L"png");
    dialog->SetFileName(filename.c_str());
    if (dialog->Show(owner) == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    ComPtr<IShellItem> item;
    check_hr(dialog->GetResult(&item), "Get image-save path");
    return shell_item_path(item.Get());
}

struct ChildProcess {
    HANDLE process = nullptr;
    HANDLE output = nullptr;
    std::string text;

    bool active() const { return process != nullptr; }

    void start(const fs::path& executable, std::wstring command_line,
               const fs::path& working_directory) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
            throw std::runtime_error("Create backend output pipe failed");
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write_pipe;
        startup.hStdError = write_pipe;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(
            executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, working_directory.c_str(), &startup, &info);
        CloseHandle(write_pipe);
        if (!created) {
            CloseHandle(read_pipe);
            throw std::runtime_error(std::format("Create backend process failed: Win32 {}",
                                                 GetLastError()));
        }
        CloseHandle(info.hThread);
        process = info.hProcess;
        output = read_pipe;
        text.clear();
    }

    void drain() {
        if (!output) return;
        DWORD available = 0;
        while (PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            if (!ReadFile(output, buffer.data(),
                          std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read,
                          nullptr) || !read) break;
            text.append(buffer.data(), read);
            if (text.size() > 1024 * 1024) text.erase(0, text.size() - 1024 * 1024);
        }
    }

    std::optional<DWORD> poll() {
        if (!process) return std::nullopt;
        drain();
        DWORD code = STILL_ACTIVE;
        if (!GetExitCodeProcess(process, &code) || code == STILL_ACTIVE) return std::nullopt;
        WaitForSingleObject(process, INFINITE);
        drain();
        CloseHandle(process);
        CloseHandle(output);
        process = nullptr;
        output = nullptr;
        return code;
    }

    void close_handles() {
        if (process) CloseHandle(process);
        if (output) CloseHandle(output);
        process = nullptr;
        output = nullptr;
    }
};

fs::path executable_directory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("GetModuleFileNameW failed");
    path.resize(length);
    return fs::path(path).parent_path();
}

bool supported_image(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".png" || extension == L".jpg" || extension == L".jpeg";
}

bool same_path(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()) == 0;
}

enum class JobStatus { ready, queued, running, done, skipped, error };

struct Job {
    fs::path input;
    fs::path output;
    fs::path side_by_side_output;
    fs::path wipe_output;
    std::shared_ptr<const ImageData> memory_input;
    bool selected = false;
    JobStatus status = JobStatus::ready;
    DWORD exit_code = 0;
    std::string log;
};

struct ExportPaths {
    fs::path output;
    fs::path side_by_side;
    fs::path wipe;
};

const char* status_text(JobStatus status) {
    switch (status) {
    case JobStatus::ready: return "Ready";
    case JobStatus::queued: return "Queued";
    case JobStatus::running: return "Running";
    case JobStatus::done: return "Done";
    case JobStatus::skipped: return "Skipped";
    case JobStatus::error: return "Error";
    }
    return "Unknown";
}

ImVec4 status_color(JobStatus status) {
    switch (status) {
    case JobStatus::done: return {0.35f, 0.85f, 0.45f, 1.0f};
    case JobStatus::running: return {0.35f, 0.70f, 1.0f, 1.0f};
    case JobStatus::error: return {1.0f, 0.35f, 0.35f, 1.0f};
    case JobStatus::skipped: return {0.95f, 0.75f, 0.30f, 1.0f};
    default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

using ModelSettings = LiveSettings;

struct App {
    HWND hwnd = nullptr;
    fs::path app_directory;
    fs::path backend;
    fs::path backend_working_directory;
    std::vector<Job> jobs;
    int selected = -1;
    int selection_anchor = -1;
    int current = -1;
    bool fast_export_mode = false;
    ChildProcess child;
    std::unique_ptr<LiveProcessor> live_processor;
    std::shared_ptr<const LiveImage> live_input;
    ModelSettings settings;
    ModelSettings export_settings;
    std::optional<fs::path> output_directory;
    std::array<char, 96> suffix{"_dlssnr"};
    int existing_policy = 0;
    bool export_comparison_images = false;
    bool export_side_by_side = true;
    bool export_wipe = false;
    float export_wipe_position = 0.5f;
    bool stop_after_current = false;
    bool preview_dirty = false;
    std::string app_error;
    PreviewTexture input_texture;
    std::array<PreviewTexture, 2> output_textures;
    PreviewTexture converted_texture;
    PreviewTexture difference_texture;
    GpuPreviewCompute gpu_preview;
    int output_slot = -1;
    std::uint64_t output_generation = 0;
    float difference_gain = 4.0f;
    int comparison_mode = 0;
    float wipe = 0.5f;
    bool fit = true;
    float zoom = 1.0f;
    ImVec2 pan{};
    bool live_dirty = false;
    std::uint64_t requested_revision = 0;
    std::uint64_t ready_revision = 0;
    double live_milliseconds = 0.0;
    std::string live_error;
    fs::path last_saved;
    unsigned clipboard_counter = 0;

    App(HWND window) : hwnd(window), app_directory(executable_directory()) {
        backend = app_directory / L"dlssnr-image.exe";
        const std::array<fs::path, 2> candidates = {app_directory, fs::current_path()};
        std::error_code error;
        for (const auto& candidate : candidates) {
            if (fs::is_regular_file(candidate / L"nvngx_dlssnr.dll", error)) {
                backend_working_directory = candidate;
                break;
            }
            error.clear();
        }
        if (backend_working_directory.empty()) backend_working_directory = app_directory;
        for (auto& output : output_textures) {
            g_srv_allocator.alloc(&output.cpu, &output.gpu);
        }
        g_srv_allocator.alloc(&converted_texture.cpu, &converted_texture.gpu);
        g_srv_allocator.alloc(&difference_texture.cpu, &difference_texture.gpu);
        live_processor = std::make_unique<LiveProcessor>(backend_working_directory,
                                                         app_directory, g_device, g_queue);
    }

    ~App() {
        child.close_handles();
        live_processor.reset();
        clear_previews();
        for (auto& output : output_textures) {
            g_srv_allocator.free(output.cpu, output.gpu);
            output.cpu = {};
            output.gpu = {};
        }
        g_srv_allocator.free(converted_texture.cpu, converted_texture.gpu);
        g_srv_allocator.free(difference_texture.cpu, difference_texture.gpu);
        converted_texture.cpu = {};
        converted_texture.gpu = {};
        difference_texture.cpu = {};
        difference_texture.gpu = {};
    }

    void clear_previews() {
        if (input_texture.valid() || output_textures[0].valid() || output_textures[1].valid() ||
            converted_texture.valid() || difference_texture.valid()) {
            wait_for_gpu();
        }
        clear_texture(input_texture);
        for (auto& output : output_textures) reset_fixed_texture(output);
        gpu_preview.reset(converted_texture, difference_texture);
        output_slot = -1;
        output_generation = 0;
        live_input.reset();
    }

    void add_file(const fs::path& path) {
        if (!fs::is_regular_file(path) || !supported_image(path)) return;
        const fs::path absolute = fs::absolute(path);
        if (std::ranges::any_of(jobs, [&](const Job& job) { return same_path(job.input, absolute); })) {
            return;
        }
        jobs.push_back(Job{absolute});
    }

    void add_clipboard_image() {
        Job job;
        job.input = std::format(L"clipboard_{:03}.png", ++clipboard_counter);
        job.memory_input = std::make_shared<ImageData>(image_from_clipboard(hwnd));
        jobs.push_back(std::move(job));
        select_only(static_cast<int>(jobs.size()) - 1);
    }

    void paste_clipboard() {
        if (const auto paths = file_paths_from_clipboard(hwnd)) {
            const std::size_t first = jobs.size();
            for (const auto& path : *paths) add_path(path);
            if (jobs.size() == first) {
                throw std::runtime_error("Clipboard file list contains no supported images");
            }
            select_added(first);
            return;
        }
        add_clipboard_image();
    }

    void add_path(const fs::path& path) {
        try {
            if (fs::is_directory(path)) {
                std::vector<fs::path> files;
                for (const auto& entry : fs::directory_iterator(
                         path, fs::directory_options::skip_permission_denied)) {
                    if (entry.is_regular_file() && supported_image(entry.path())) {
                        files.push_back(entry.path());
                    }
                }
                std::ranges::sort(files);
                for (const auto& file : files) add_file(file);
            } else {
                add_file(path);
            }
        } catch (const std::exception& error) {
            app_error = error.what();
        }
    }

    void activate(int index) {
        if (index < 0 || index >= static_cast<int>(jobs.size())) return;
        selected = index;
        last_saved.clear();
        preview_dirty = true;
    }

    void select_only(int index) {
        if (index < 0 || index >= static_cast<int>(jobs.size())) return;
        for (auto& job : jobs) job.selected = false;
        jobs[static_cast<std::size_t>(index)].selected = true;
        selection_anchor = index;
        activate(index);
    }

    void select_added(std::size_t first) {
        if (first >= jobs.size()) return;
        for (auto& job : jobs) job.selected = false;
        for (std::size_t i = first; i < jobs.size(); ++i) jobs[i].selected = true;
        selection_anchor = static_cast<int>(first);
        activate(static_cast<int>(jobs.size()) - 1);
    }

    void select_clicked(int index, bool ctrl, bool shift) {
        if (index < 0 || index >= static_cast<int>(jobs.size())) return;
        if (shift && selection_anchor >= 0 && selection_anchor < static_cast<int>(jobs.size())) {
            if (!ctrl) {
                for (auto& job : jobs) job.selected = false;
            }
            const auto [first, last] = std::minmax(selection_anchor, index);
            for (int i = first; i <= last; ++i) jobs[static_cast<std::size_t>(i)].selected = true;
        } else if (ctrl) {
            jobs[static_cast<std::size_t>(index)].selected =
                !jobs[static_cast<std::size_t>(index)].selected;
            selection_anchor = index;
        } else {
            for (auto& job : jobs) job.selected = false;
            jobs[static_cast<std::size_t>(index)].selected = true;
            selection_anchor = index;
        }
        activate(index);
    }

    void select_all() {
        for (auto& job : jobs) job.selected = true;
        if (selected < 0 && !jobs.empty()) activate(0);
        if (selection_anchor < 0) selection_anchor = selected;
    }

    std::size_t selected_count() const {
        return static_cast<std::size_t>(std::ranges::count_if(
            jobs, [](const Job& job) { return job.selected; }));
    }

    void request_live_preview() {
        if (live_input) live_dirty = true;
    }

    std::wstring output_filename(const Job& job, std::wstring_view artifact = {},
                                 unsigned serial = 0) const {
        const int size = MultiByteToWideChar(CP_UTF8, 0, suffix.data(), -1, nullptr, 0);
        std::wstring wide_suffix(static_cast<std::size_t>(std::max(1, size)), L'\0');
        if (size > 1) {
            MultiByteToWideChar(CP_UTF8, 0, suffix.data(), -1, wide_suffix.data(), size);
            wide_suffix.resize(static_cast<std::size_t>(size - 1));
        } else {
            wide_suffix.clear();
        }
        std::wstring filename = job.input.stem().wstring() + wide_suffix;
        if (serial > 1) filename += std::format(L"_{}", serial);
        filename += artifact;
        return filename + L".png";
    }

    fs::path output_for(const Job& job, std::wstring_view artifact = {},
                        unsigned serial = 0) const {
        if (job.memory_input && !output_directory) return {};
        const fs::path directory = output_directory ? *output_directory : job.input.parent_path();
        return directory / output_filename(job, artifact, serial);
    }

    ExportPaths export_paths_for(const Job& job, unsigned serial = 0) const {
        ExportPaths paths;
        paths.output = output_for(job, {}, serial);
        if (export_comparison_images && export_side_by_side) {
            paths.side_by_side = output_for(job, L"_side-by-side", serial);
        }
        if (export_comparison_images && export_wipe) {
            paths.wipe = output_for(job, L"_wipe", serial);
        }
        return paths;
    }

    static bool any_exists(const ExportPaths& paths) {
        return fs::exists(paths.output) || (!paths.side_by_side.empty() && fs::exists(paths.side_by_side)) ||
               (!paths.wipe.empty() && fs::exists(paths.wipe));
    }

    ExportPaths resolved_export_paths(const Job& job) const {
        ExportPaths paths = export_paths_for(job);
        if (existing_policy != 2 || paths.output.empty() || !any_exists(paths)) return paths;
        for (unsigned serial = 2;; ++serial) {
            paths = export_paths_for(job, serial);
            if (!any_exists(paths)) return paths;
        }
    }

    bool suffix_valid() const {
        if (!suffix[0]) return false;
        return std::string_view(suffix.data()).find_first_of("<>:\"/\\|?*") ==
               std::string_view::npos;
    }

    bool queue_active() const {
        return child.active() || std::ranges::any_of(jobs, [](const Job& job) {
            return job.status == JobStatus::queued || job.status == JobStatus::running;
        });
    }

    void queue_all() {
        if (queue_active() || live_processor->busy() || live_dirty || jobs.empty() ||
            !suffix_valid()) return;
        fast_export_mode = !settings.diagnostics;
        export_settings = settings;
        export_wipe_position = wipe;
        bool any = false;
        for (auto& job : jobs) {
            if (fast_export_mode || !job.memory_input) {
                job.status = JobStatus::queued;
                any = true;
            } else {
                job.status = JobStatus::skipped;
                job.log = "Diagnostic CLI export is unavailable for clipboard images.";
            }
        }
        if (!any) return;
        stop_after_current = false;
        start_next();
    }

    void queue_selected() {
        if (queue_active() || live_processor->busy() || live_dirty || selected_count() == 0 ||
            !suffix_valid()) return;
        fast_export_mode = !settings.diagnostics;
        export_settings = settings;
        export_wipe_position = wipe;
        bool any = false;
        for (auto& job : jobs) {
            if (!job.selected) continue;
            if (fast_export_mode || !job.memory_input) {
                job.status = JobStatus::queued;
                any = true;
            } else {
                job.status = JobStatus::skipped;
                job.log = "Diagnostic CLI export is unavailable for clipboard images.";
            }
        }
        if (!any) return;
        stop_after_current = false;
        start_next();
    }

    std::wstring backend_command(const Job& job) const {
        auto add = [](std::wstring& command, std::wstring_view option, std::wstring value) {
            command += L" ";
            command += option;
            command += L" ";
            command += quote_arg(value);
        };
        std::wstring command = quote_arg(backend.wstring()) + L" " + quote_arg(job.input.wstring()) +
                               L" " + quote_arg(job.output.wstring());
        add(command, L"--style", std::to_wstring(export_settings.style));
        add(command, L"--preset", std::to_wstring(export_settings.preset));
        add(command, L"--intensity", std::format(L"{:.3f}", export_settings.intensity));
        add(command, L"--tone", std::format(L"{:.3f}", export_settings.tone));
        add(command, L"--structure", std::format(L"{:.3f}", export_settings.structure));
        add(command, L"--skin", std::format(L"{:.3f}", export_settings.skin));
        add(command, L"--auto-mask", export_settings.auto_mask ? L"1" : L"0");
        add(command, L"--enabled", export_settings.enabled ? L"1" : L"0");
        add(command, L"--ui-correction", export_settings.ui_correction ? L"1" : L"0");
        static constexpr const wchar_t* motions[] = {L"absent", L"zero", L"tiny"};
        add(command, L"--mv", motions[export_settings.motion]);
        add(command, L"--mv-scale-x", std::format(L"{:.3f}", export_settings.mv_scale_x));
        add(command, L"--mv-scale-y", std::format(L"{:.3f}", export_settings.mv_scale_y));
        static constexpr const wchar_t* depths[] = {L"absent", L"zero", L"one"};
        add(command, L"--depth", depths[export_settings.depth]);
        add(command, L"--depth-inverted", export_settings.depth_inverted ? L"1" : L"0");
        static constexpr const wchar_t* masks[] = {L"absent", L"zero", L"one", L"half"};
        add(command, L"--control-mask", masks[export_settings.control_mask]);
        if (export_settings.control_mask == 2) {
            add(command, L"--control-mask-value",
                std::format(L"{:.3f}", export_settings.control_mask_value));
        }
        add(command, L"--control-mask-format",
            export_settings.control_mask_format == 0 ? L"r8" : L"r16f");
        add(command, L"--transfer", export_settings.transfer == 0 ? L"code" : L"srgb-linear");
        add(command, L"--gpu-format", export_settings.gpu_format == 0 ? L"fp16" : L"rgba8");
        add(command, L"--diagnostics", export_settings.diagnostics ? L"1" : L"0");
        add(command, L"--output-scale", std::to_wstring(export_settings.output_scale));
        return command;
    }

    void start_next() {
        if (child.active()) return;
        while (true) {
            const auto iterator = std::ranges::find_if(
                jobs, [&](const Job& job) {
                    return job.status == JobStatus::queued &&
                           (fast_export_mode || !job.memory_input);
                });
            if (iterator == jobs.end()) {
                current = -1;
                if (fast_export_mode) {
                    fast_export_mode = false;
                    preview_dirty = selected >= 0;
                }
                return;
            }
            current = static_cast<int>(std::distance(jobs.begin(), iterator));
            Job& job = *iterator;
            const ExportPaths paths = resolved_export_paths(job);
            job.output = paths.output;
            job.side_by_side_output = paths.side_by_side;
            job.wipe_output = paths.wipe;
            job.log.clear();
            if (job.output.empty()) {
                job.status = JobStatus::error;
                job.log = "Clipboard images require a selected output folder for export.";
                app_error = job.log;
                continue;
            }
            if (existing_policy == 1 && any_exists(paths)) {
                job.status = JobStatus::skipped;
                job.log = std::format(
                    "Output group already exists; skipped:\n{}\nChange the existing-files "
                    "policy, suffix, or output folder.\n",
                    utf8(job.output.wstring()));
                continue;
            }
            try {
                if (fast_export_mode) {
                    std::shared_ptr<const ImageData> image = job.memory_input
                        ? job.memory_input
                        : std::make_shared<ImageData>(decode_image(job.input));
                    job.status = JobStatus::running;
                    live_processor->request(++requested_revision, image, export_settings);
                    return;
                }
                if (!fs::is_regular_file(backend)) {
                    throw std::runtime_error("dlssnr-image.exe is missing beside dlssnr-gui.exe");
                }
                job.status = JobStatus::running;
                child.start(backend, backend_command(job), backend_working_directory);
            } catch (const std::exception& error) {
                job.status = JobStatus::error;
                job.log = error.what();
                app_error = error.what();
                continue;
            }
            return;
        }
    }

    void advance_export_queue() {
        current = -1;
        if (stop_after_current) {
            for (auto& job : jobs) {
                if (job.status == JobStatus::queued) job.status = JobStatus::ready;
            }
            stop_after_current = false;
            if (fast_export_mode) {
                fast_export_mode = false;
                preview_dirty = selected >= 0;
            }
        } else {
            start_next();
        }
    }

    ImageData job_input(const Job& job) const {
        return job.memory_input ? *job.memory_input : decode_image(job.input);
    }

    void save_job_comparisons(const Job& job, const ImageData& output) const {
        if (job.side_by_side_output.empty() && job.wipe_output.empty()) return;
        const ImageData input = job_input(job);
        if (!job.side_by_side_output.empty()) {
            const ImageData comparison = make_side_by_side(input, output);
            save_png(job.side_by_side_output, comparison.width, comparison.height,
                     comparison.rgba);
        }
        if (!job.wipe_output.empty()) {
            const ImageData comparison = make_wipe(input, output, export_wipe_position);
            save_png(job.wipe_output, comparison.width, comparison.height, comparison.rgba);
        }
    }

    void poll_backend() {
        if (!child.active()) return;
        child.drain();
        if (current >= 0) jobs[static_cast<std::size_t>(current)].log = child.text;
        const auto exit_code = child.poll();
        if (!exit_code) return;
        if (current >= 0) {
            Job& job = jobs[static_cast<std::size_t>(current)];
            job.exit_code = *exit_code;
            job.log = child.text;
            if (*exit_code == 0 && fs::is_regular_file(job.output)) {
                try {
                    save_job_comparisons(job, decode_image(job.output));
                    job.status = JobStatus::done;
                } catch (const std::exception& error) {
                    job.status = JobStatus::error;
                    job.log += std::format("\nComparison export failed: {}", error.what());
                    app_error = error.what();
                }
            } else {
                job.status = JobStatus::error;
            }
        }
        advance_export_queue();
    }

    PreviewTexture* raw_output() {
        return output_slot >= 0 ? &output_textures[static_cast<std::size_t>(output_slot)] : nullptr;
    }

    const PreviewTexture* raw_output() const {
        return output_slot >= 0 ? &output_textures[static_cast<std::size_t>(output_slot)] : nullptr;
    }

    PreviewTexture* displayed_output() {
        auto* raw = raw_output();
        if (!raw) return nullptr;
        return raw->encode_srgb ? &converted_texture : raw;
    }

    const PreviewTexture* displayed_output() const {
        const auto* raw = raw_output();
        if (!raw) return nullptr;
        return raw->encode_srgb ? &converted_texture : raw;
    }

    void ensure_gpu_comparisons() {
        auto* raw = raw_output();
        if (!raw || !raw->valid()) return;
        if (raw->encode_srgb) {
            gpu_preview.ensure_conversion(g_device, converted_texture, raw->width, raw->height);
        }
        if (comparison_mode == 4 && input_texture.valid() &&
            input_texture.width == raw->width && input_texture.height == raw->height) {
            gpu_preview.ensure_difference(g_device, difference_texture, raw->width, raw->height);
        }
    }

    void refresh_preview() {
        preview_dirty = false;
        clear_previews();
        app_error.clear();
        live_error.clear();
        if (selected < 0 || selected >= static_cast<int>(jobs.size())) return;
        try {
            Job& job = jobs[static_cast<std::size_t>(selected)];
            std::shared_ptr<const ImageData> image = job.memory_input
                ? job.memory_input : std::make_shared<ImageData>(decode_image(job.input));
            input_texture = upload_texture(*image);
            live_input = image;
            fit = true;
            zoom = 1.0f;
            pan = {};
            request_live_preview();
        } catch (const std::exception& error) {
            app_error = std::format("Preview: {}", error.what());
        }
    }

    void poll_live_preview() {
        if (live_dirty && !queue_active()) {
            live_dirty = false;
            live_error.clear();
            live_processor->request(++requested_revision, live_input, settings);
        }
        const auto result = live_processor->take_result();
        if (!result) return;
        if (fast_export_mode && current >= 0) {
            Job& job = jobs[static_cast<std::size_t>(current)];
            try {
                if (!result->error.empty()) throw std::runtime_error(result->error);
                const ImageData image = readback_live_image(g_queue, *result);
                save_png(job.output, image.width, image.height, image.rgba);
                save_job_comparisons(job, image);
                job.status = JobStatus::done;
                job.log = std::format(
                    "Fast in-process export:\n{}\nEvaluate {:.1f} ms; one explicit save readback; "
                    "no diagnostic artifacts.{}", utf8(job.output.wstring()), result->milliseconds,
                    job.side_by_side_output.empty() && job.wipe_output.empty()
                        ? "" : " Comparison files saved.");
            } catch (const std::exception& error) {
                job.status = JobStatus::error;
                job.log = error.what();
                app_error = error.what();
            }
            advance_export_queue();
            return;
        }
        live_milliseconds = result->milliseconds;
        if (!result->error.empty()) {
            live_error = result->error;
            return;
        }
        try {
            if (result->generation != output_generation) {
                wait_for_gpu();
                for (auto& output : output_textures) reset_fixed_texture(output);
                gpu_preview.reset(converted_texture, difference_texture);
                for (unsigned slot = 0; slot < output_textures.size(); ++slot) {
                    auto& output = output_textures[slot];
                    output.resource = result->resources[slot];
                    output.width = result->width;
                    output.height = result->height;
                    output.format = result->format;
                    output.encode_srgb = result->encode_srgb;
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Format = output.format;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Texture2D.MipLevels = 1;
                    g_device->CreateShaderResourceView(output.resource.Get(), &srv, output.cpu);
                }
                output_generation = result->generation;
                gpu_preview.set_sources(g_device, input_texture, output_textures);
            }
            output_slot = static_cast<int>(result->slot);
            gpu_preview.conversion_dirty = result->encode_srgb;
            gpu_preview.difference_dirty = true;
            ready_revision = result->revision;
            live_error.clear();
        } catch (const std::exception& error) {
            live_error = error.what();
        }
    }

    bool live_result_ready() const {
        return raw_output() && raw_output()->valid() && ready_revision == requested_revision &&
               !live_dirty && !live_processor->busy() && live_error.empty();
    }

    ImageData readback_current_output() const {
        const auto* output = raw_output();
        LiveResult gpu_result;
        gpu_result.resources[static_cast<std::size_t>(output_slot)] = output->resource;
        gpu_result.width = output->width;
        gpu_result.height = output->height;
        gpu_result.format = output->format;
        gpu_result.encode_srgb = output->encode_srgb;
        gpu_result.slot = static_cast<unsigned>(output_slot);
        return readback_live_image(g_queue, gpu_result);
    }

    void save_current_as() {
        if (!live_result_ready() || selected < 0) return;
        Job& job = jobs[static_cast<std::size_t>(selected)];
        const auto chosen = save_image_dialog(hwnd, output_filename(job));
        if (!chosen) return;
        const ImageData image = readback_current_output();
        const fs::path path = *chosen;
        save_png(path, image.width, image.height, image.rgba);
        job.status = JobStatus::done;
        job.log = std::format("Saved active GPU preview as:\n{}\nExplicit GPU readback; no CLI "
                              "diagnostics were run.", utf8(path.wstring()));
        last_saved = path;
    }

    void save_current_comparison() {
        if (!live_result_ready() || selected < 0 || (comparison_mode != 0 && comparison_mode != 1)) {
            return;
        }
        Job& job = jobs[static_cast<std::size_t>(selected)];
        const std::wstring_view artifact = comparison_mode == 0 ? L"_wipe" : L"_side-by-side";
        const auto chosen = save_image_dialog(hwnd, output_filename(job, artifact));
        if (!chosen) return;
        const ImageData output = readback_current_output();
        const ImageData comparison = comparison_mode == 0
            ? make_wipe(*live_input, output, wipe)
            : make_side_by_side(*live_input, output);
        save_png(*chosen, comparison.width, comparison.height, comparison.rgba);
        job.status = JobStatus::done;
        job.log = std::format("Saved active {} comparison as:\n{}",
                              comparison_mode == 0 ? "wipe" : "side-by-side",
                              utf8(chosen->wstring()));
        last_saved = *chosen;
    }

    void record_gpu_work(ID3D12GraphicsCommandList* list) {
        auto* raw = raw_output();
        if (!raw || !raw->valid()) return;
        ensure_gpu_comparisons();
        const bool need_conversion = raw->encode_srgb && converted_texture.valid();
        const bool need_difference = comparison_mode == 4 && difference_texture.valid();
        gpu_preview.record(list, converted_texture, difference_texture, need_conversion,
                           need_difference, difference_gain, raw->encode_srgb,
                           static_cast<unsigned>(output_slot));
    }

    void tick() {
        const std::size_t first_drop = jobs.size();
        for (const auto& path : g_pending_drops) add_path(path);
        if (jobs.size() > first_drop) select_added(first_drop);
        g_pending_drops.clear();
        poll_backend();
        if (!child.active() && current < 0 && !stop_after_current) start_next();
        if (preview_dirty) refresh_preview();
        poll_live_preview();
    }

    void draw_toolbar() {
        const bool busy = queue_active();
        const bool paste_shortcut = !busy && !ImGui::GetIO().WantTextInput &&
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_RouteGlobal);
        const bool save_shortcut = !busy && !ImGui::GetIO().WantTextInput &&
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal);
        if (save_shortcut) {
            try {
                save_current_as();
            } catch (const std::exception& error) {
                app_error = error.what();
            }
        }
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Add images...")) {
            try {
                const std::size_t first = jobs.size();
                for (const auto& path : open_image_dialog(hwnd)) add_file(path);
                select_added(first);
            } catch (const std::exception& error) {
                app_error = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Paste image") || paste_shortcut) {
            try {
                paste_clipboard();
            } catch (const std::exception& error) {
                app_error = error.what();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Paste an image or Explorer file selection (Ctrl+V)");
        }
        ImGui::SameLine();
        if (ImGui::Button("Add folder...")) {
            try {
                if (const auto folder = open_folder_dialog(hwnd, L"Add images from folder")) {
                    const std::size_t first = jobs.size();
                    add_path(*folder);
                    select_added(first);
                }
            } catch (const std::exception& error) {
                app_error = error.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Drop files/folders or Ctrl+V");
    }

    void draw_output_options() {
        ImGui::SeparatorText("Output");
        const bool busy = queue_active();
        const Job* active_job = selected >= 0 && selected < static_cast<int>(jobs.size())
            ? &jobs[static_cast<std::size_t>(selected)] : nullptr;

        ImGui::BeginDisabled(busy);
        int destination = output_directory ? 1 : 0;
        static constexpr const char* destinations[] = {"Next to source", "Selected folder"};
        if (ImGui::Combo("Destination", &destination, destinations,
                         static_cast<int>(std::size(destinations)))) {
            try {
                if (destination == 0) {
                    output_directory.reset();
                } else if (const auto folder = open_folder_dialog(hwnd, L"Choose output folder")) {
                    output_directory = *folder;
                }
                last_saved.clear();
            } catch (const std::exception& error) {
                app_error = error.what();
            }
        }
        if (output_directory) {
            if (ImGui::Button("Browse output folder...")) {
                try {
                    if (const auto folder = open_folder_dialog(hwnd, L"Choose output folder")) {
                        output_directory = *folder;
                        last_saved.clear();
                    }
                } catch (const std::exception& error) {
                    app_error = error.what();
                }
            }
            ImGui::TextWrapped("%s", utf8(output_directory->wstring()).c_str());
        }

        if (ImGui::InputText("Filename suffix", suffix.data(), suffix.size())) {
            last_saved.clear();
        }
        ImGui::EndDisabled();

        if (!suffix_valid()) {
            ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f},
                               "Enter a valid non-empty suffix");
        }
        if (active_job) {
            const fs::path path = output_for(*active_job);
            if (path.empty()) {
                ImGui::TextColored({1.0f, 0.65f, 0.25f, 1.0f},
                                   "Clipboard export needs a selected folder.");
            } else {
                ImGui::TextWrapped("Active output: %s", utf8(path.wstring()).c_str());
                if (ImGui::SmallButton("Open output folder")) {
                    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                        hwnd, L"open", path.parent_path().c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL));
                    if (result <= 32) app_error = "Could not open the output folder";
                }
            }
        }

        if (ImGui::TreeNodeEx("More output options")) {
            ImGui::BeginDisabled(busy);
            static constexpr const char* existing[] = {"Replace", "Skip", "Add number"};
            ImGui::Combo("Existing files", &existing_policy, existing,
                         static_cast<int>(std::size(existing)));
            ImGui::Checkbox("Diagnostic export (CLI)", &settings.diagnostics);
            if (ImGui::Checkbox("Export comparison images", &export_comparison_images) &&
                export_comparison_images && !export_side_by_side && !export_wipe) {
                export_side_by_side = true;
            }
            if (export_comparison_images) {
                ImGui::Indent();
                ImGui::Checkbox("Side-by-side", &export_side_by_side);
                ImGui::Checkbox("Wipe", &export_wipe);
                if (!export_side_by_side && !export_wipe) {
                    ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f},
                                       "Choose at least one comparison type");
                }
                if (export_wipe) {
                    ImGui::TextDisabled("Uses the current preview Wipe position (%.2f).", wipe);
                }
                ImGui::Unindent();
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }

        const std::size_t count = selected_count();
        const bool comparisons_valid = !export_comparison_images || export_side_by_side || export_wipe;
        const bool can_start = !busy && !live_processor->busy() && !live_dirty && suffix_valid() &&
                               comparisons_valid;
        const bool selected_needs_folder = !output_directory && std::ranges::any_of(
            jobs, [](const Job& job) { return job.selected && job.memory_input; });
        const bool all_need_folder = !output_directory && std::ranges::any_of(
            jobs, [](const Job& job) { return job.memory_input != nullptr; });
        const std::string selected_label = std::format("Export selected ({})", count);
        ImGui::BeginDisabled(!can_start || count == 0 || selected_needs_folder);
        if (ImGui::Button(selected_label.c_str())) queue_selected();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_start || jobs.empty() || all_need_folder);
        if (ImGui::Button("Export all")) queue_all();
        ImGui::EndDisabled();

        if (busy) {
            if (!stop_after_current && ImGui::Button("Stop after current")) {
                stop_after_current = true;
            }
            if (stop_after_current) ImGui::TextDisabled("Stopping after the active image...");
        }
        const auto finished = std::ranges::count_if(jobs, [](const Job& job) {
            return job.status == JobStatus::done || job.status == JobStatus::skipped ||
                   job.status == JobStatus::error;
        });
        ImGui::TextDisabled("%zu selected | %zu images | %zu finished", count, jobs.size(),
                            static_cast<std::size_t>(finished));
    }

    void draw_queue() {
        ImGui::SeparatorText("Queue");
        const bool busy = queue_active();
        if (!ImGui::GetIO().WantTextInput &&
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_RouteGlobal)) {
            select_all();
        }
        ImGui::BeginDisabled(busy || selected_count() == 0);
        if (ImGui::Button("Remove selected")) {
            const int old_active = selected;
            std::erase_if(jobs, [](const Job& job) { return job.selected; });
            if (jobs.empty()) {
                selected = -1;
                selection_anchor = -1;
                preview_dirty = true;
            } else {
                select_only(std::min(old_active, static_cast<int>(jobs.size()) - 1));
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || jobs.empty());
        if (ImGui::Button("Clear")) {
            jobs.clear();
            selected = -1;
            selection_anchor = -1;
            preview_dirty = true;
        }
        ImGui::EndDisabled();

        const float log_height = selected >= 0 ? 190.0f : 0.0f;
        if (ImGui::BeginChild("queue-list", ImVec2(0, -log_height), ImGuiChildFlags_Borders)) {
            if (jobs.empty()) {
                ImGui::TextWrapped("Add PNG/JPEG files, a folder, drag them here, or press Ctrl+V.");
            } else if (ImGui::BeginTable("jobs", 2,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                             ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 62.0f);
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(jobs.size()));
                while (clipper.Step()) {
                    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                        Job& job = jobs[static_cast<std::size_t>(index)];
                        ImGui::PushID(index);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const std::string name = std::format("{}{}", selected == index ? "> " : "",
                                                             utf8(job.input.filename().wstring()));
                        if (ImGui::Selectable(name.c_str(), job.selected,
                                              ImGuiSelectableFlags_SpanAllColumns)) {
                            const ImGuiIO& io = ImGui::GetIO();
                            select_clicked(index, io.KeyCtrl, io.KeyShift);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", utf8(job.input.wstring()).c_str());
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextColored(status_color(job.status), "%s", status_text(job.status));
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        if (selected >= 0 && selected < static_cast<int>(jobs.size())) {
            const Job& job = jobs[static_cast<std::size_t>(selected)];
            ImGui::SeparatorText("Backend log");
            ImGui::TextColored(status_color(job.status), "%s", status_text(job.status));
            if (job.exit_code) {
                ImGui::SameLine();
                ImGui::TextDisabled("exit %lu", job.exit_code);
            }
            ImGui::BeginChild("job-log", ImVec2(0, 0), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(job.log.empty() ? "No log yet." : job.log.c_str());
            ImGui::EndChild();
        }
    }

    struct ImageRect {
        ImVec2 min;
        ImVec2 max;
    };

    ImageRect image_rect(const PreviewTexture& texture, ImVec2 box_min, ImVec2 box_max) const {
        const ImVec2 available{std::max(1.0f, box_max.x - box_min.x),
                               std::max(1.0f, box_max.y - box_min.y)};
        float scale = zoom;
        if (fit) {
            scale *= std::min(available.x / texture.width, available.y / texture.height);
        }
        const ImVec2 size{texture.width * scale, texture.height * scale};
        const ImVec2 center{box_min.x + available.x * 0.5f + pan.x,
                            box_min.y + available.y * 0.5f + pan.y};
        return {{center.x - size.x * 0.5f, center.y - size.y * 0.5f},
                {center.x + size.x * 0.5f, center.y + size.y * 0.5f}};
    }

    static ImTextureRef texture_ref(const PreviewTexture& texture) {
        return ImTextureRef(static_cast<ImTextureID>(texture.gpu.ptr));
    }

    ImageRect draw_image(ImDrawList* list, const PreviewTexture& texture, ImVec2 box_min,
                         ImVec2 box_max) const {
        const ImageRect rect = image_rect(texture, box_min, box_max);
        list->PushClipRect(box_min, box_max, true);
        list->AddImage(texture_ref(texture), rect.min, rect.max);
        list->PopClipRect();
        return rect;
    }

    void draw_preview_canvas() {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.x < 8.0f || available.y < 8.0f) return;
        const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("preview-canvas", available,
                               ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
        const ImVec2 canvas_max{canvas_min.x + available.x, canvas_min.y + available.y};
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->AddRectFilled(canvas_min, canvas_max, IM_COL32(18, 20, 24, 255));

        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
                pan.x += io.MouseDelta.x;
                pan.y += io.MouseDelta.y;
            }
            if (io.MouseWheel != 0.0f) {
                const float old_zoom = zoom;
                zoom = std::clamp(zoom * std::pow(1.18f, io.MouseWheel), 0.1f, 16.0f);
                const float ratio = zoom / old_zoom;
                const ImVec2 box_center{canvas_min.x + available.x * 0.5f,
                                        canvas_min.y + available.y * 0.5f};
                pan.x = io.MousePos.x - box_center.x -
                        ratio * (io.MousePos.x - box_center.x - pan.x);
                pan.y = io.MousePos.y - box_center.y -
                        ratio * (io.MousePos.y - box_center.y - pan.y);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
                fit = true;
                zoom = 1.0f;
                pan = {};
            }
        }

        if (!input_texture.valid()) {
            list->AddText({canvas_min.x + 14.0f, canvas_min.y + 14.0f},
                          IM_COL32(155, 160, 170, 255), "Select an image to preview it.");
            return;
        }
        const PreviewTexture* output = displayed_output();

        if (comparison_mode == 1 && output && output->valid()) {
            const float middle = canvas_min.x + available.x * 0.5f;
            draw_image(list, input_texture, canvas_min, {middle - 3.0f, canvas_max.y});
            draw_image(list, *output, {middle + 3.0f, canvas_min.y}, canvas_max);
            list->AddText({canvas_min.x + 8.0f, canvas_min.y + 8.0f}, IM_COL32_WHITE, "Input");
            list->AddText({middle + 8.0f, canvas_min.y + 8.0f}, IM_COL32_WHITE, "Output");
            return;
        }

        const PreviewTexture* shown = &input_texture;
        if (comparison_mode == 3 && output && output->valid()) shown = output;
        if (comparison_mode == 4 && difference_texture.valid()) shown = &difference_texture;
        const ImageRect rect = draw_image(list, *shown, canvas_min, canvas_max);

        if (comparison_mode == 0 && output && output->valid()) {
            if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                rect.max.x > rect.min.x) {
                wipe = std::clamp((ImGui::GetIO().MousePos.x - rect.min.x) /
                                      (rect.max.x - rect.min.x),
                                  0.0f, 1.0f);
            }
            const float split = rect.min.x + (rect.max.x - rect.min.x) * wipe;
            const ImageRect visible{
                {std::max(rect.min.x, canvas_min.x), std::max(rect.min.y, canvas_min.y)},
                {std::min(rect.max.x, canvas_max.x), std::min(rect.max.y, canvas_max.y)}};
            if (visible.min.x < visible.max.x && visible.min.y < visible.max.y) {
                const float visible_split = std::clamp(split, visible.min.x, visible.max.x);
                if (visible_split > visible.min.x) {
                    list->PushClipRect(visible.min, {visible_split, visible.max.y}, true);
                    list->AddImage(texture_ref(*output), rect.min, rect.max);
                    list->PopClipRect();
                }
                if (split >= visible.min.x && split <= visible.max.x) {
                    list->AddLine({split, visible.min.y}, {split, visible.max.y},
                                  IM_COL32_WHITE, 2.0f);
                }
                list->AddText({visible.min.x + 8.0f, visible.min.y + 8.0f}, IM_COL32_WHITE,
                              "Output");
                const ImVec2 label_size = ImGui::CalcTextSize("Input");
                list->AddText({visible.max.x - label_size.x - 8.0f, visible.min.y + 8.0f},
                              IM_COL32_WHITE, "Input");
            }
        }
    }

    void draw_preview() {
        ImGui::SeparatorText("Comparison");
        static constexpr const char* modes[] = {"Wipe", "Side by side", "Input", "Output",
                                                "Difference"};
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::Combo("Mode", &comparison_mode, modes,
                         static_cast<int>(std::size(modes)))) {
            if (comparison_mode == 4) gpu_preview.difference_dirty = true;
        }
        ensure_gpu_comparisons();
        const PreviewTexture* output = displayed_output();
        ImGui::SameLine();
        if (ImGui::Button("Fit")) {
            fit = true;
            zoom = 1.0f;
            pan = {};
        }
        ImGui::SameLine();
        if (ImGui::Button("100%")) {
            fit = false;
            zoom = 1.0f;
            pan = {};
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(115.0f);
        ImGui::SliderFloat("Zoom", &zoom, 0.1f, 16.0f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic);
        if (comparison_mode == 0 && output && output->valid()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Wipe", &wipe, 0.0f, 1.0f, "%.2f");
        }
        if (comparison_mode == 4 && difference_texture.valid()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::SliderFloat("Gain", &difference_gain, 1.0f, 32.0f, "%.1fx",
                                   ImGuiSliderFlags_Logarithmic)) {
                gpu_preview.difference_dirty = true;
            }
        }

        if (input_texture.valid()) {
            ImGui::TextDisabled("Input %ux%u", input_texture.width, input_texture.height);
            if (output && output->valid()) {
                ImGui::SameLine();
                ImGui::TextDisabled(" | Output %ux%u", output->width, output->height);
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled(" | No output yet");
            }
        }
        ImGui::BeginDisabled(!live_result_ready() || selected < 0);
        if (ImGui::Button("Save As... (Ctrl+S)")) {
            try {
                save_current_as();
            } catch (const std::exception& error) {
                app_error = error.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!live_result_ready() || selected < 0 ||
                             (comparison_mode != 0 && comparison_mode != 1));
        if (ImGui::Button("Save comparison...")) {
            try {
                save_current_comparison();
            } catch (const std::exception& error) {
                app_error = error.what();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Available for Wipe and Side-by-side preview modes");
        }
        if (!last_saved.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("Saved: %s", utf8(last_saved.wstring()).c_str());
        }
        ImGui::BeginChild("preview-status", ImVec2(0, ImGui::GetTextLineHeight()),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (fast_export_mode && current >= 0) {
            ImGui::TextColored({0.35f, 0.85f, 0.45f, 1.0f},
                               "Fast export: processing queued image...");
        } else if (live_processor->busy() || live_dirty) {
            ImGui::TextColored({0.35f, 0.70f, 1.0f, 1.0f},
                               "Live preview: rendering latest settings...");
        } else if (!live_error.empty()) {
            ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "Live preview: %s",
                               live_error.c_str());
        } else if (live_result_ready()) {
            ImGui::TextDisabled("GPU-only preview: %.1f ms | no readback until save/export | wheel zoom, "
                                "right/middle drag pan", live_milliseconds);
        }
        ImGui::EndChild();
        ImGui::BeginChild("preview", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        draw_preview_canvas();
        ImGui::EndChild();
    }

    void draw_settings() {
        const ModelSettings before = settings;
        ImGui::SeparatorText("Neural Rendering");
        ImGui::PushItemWidth(-130.0f);
        static constexpr const char* styles[] = {
            "Default (0)", "Natural (1)", "Cinematic (2)", "Diagnostic 3",
            "Diagnostic 4", "Diagnostic 5", "Diagnostic 6"};
        ImGui::Combo("RN Style", &settings.style, styles, static_cast<int>(std::size(styles)));
        ImGui::SliderInt("Preset", &settings.preset, 0, 3);
        ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Tone", &settings.tone, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Structure", &settings.structure, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Skin", &settings.skin, -1.0f, 2.0f, "%.2f");
        ImGui::Checkbox("Auto mask", &settings.auto_mask);
        ImGui::SameLine();
        ImGui::Checkbox("Enabled", &settings.enabled);
        ImGui::Checkbox("UI correction", &settings.ui_correction);

        static constexpr const char* scales[] = {"1x (supported)", "2x (experimental)"};
        int scale_index = settings.output_scale - 1;
        if (ImGui::Combo("Output scale", &scale_index, scales,
                         static_cast<int>(std::size(scales)))) {
            settings.output_scale = scale_index + 1;
        }
        if (settings.output_scale == 2) {
            ImGui::TextColored({1.0f, 0.65f, 0.25f, 1.0f},
                               "Current DLL: Evaluate returns InvalidParameter.");
        }

        if (ImGui::TreeNodeEx("Advanced inputs", ImGuiTreeNodeFlags_DefaultOpen)) {
            static constexpr const char* motions[] = {"Absent", "Zero", "Tiny"};
            ImGui::Combo("Motion vectors", &settings.motion, motions,
                         static_cast<int>(std::size(motions)));
            ImGui::SliderFloat("MV scale X", &settings.mv_scale_x, -2.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("MV scale Y", &settings.mv_scale_y, -2.0f, 2.0f, "%.2f");
            static constexpr const char* depths[] = {"Absent", "Zero", "One"};
            ImGui::Combo("Depth", &settings.depth, depths,
                         static_cast<int>(std::size(depths)));
            ImGui::Checkbox("Depth inverted", &settings.depth_inverted);
            static constexpr const char* masks[] = {"Absent", "Zero", "One/value",
                                                    "Half-image"};
            ImGui::Combo("Control mask", &settings.control_mask, masks,
                         static_cast<int>(std::size(masks)));
            if (settings.control_mask == 2) {
                ImGui::SliderFloat("Mask value", &settings.control_mask_value, -16.0f, 16.0f,
                                   "%.2f");
            }
            static constexpr const char* mask_formats[] = {"R8", "R16F"};
            ImGui::Combo("Mask format", &settings.control_mask_format, mask_formats,
                         static_cast<int>(std::size(mask_formats)));
            static constexpr const char* transfers[] = {"Code values", "sRGB to linear"};
            ImGui::Combo("Transfer", &settings.transfer, transfers,
                         static_cast<int>(std::size(transfers)));
            static constexpr const char* gpu_formats[] = {"RGBA16F", "RGBA8"};
            ImGui::Combo("GPU format", &settings.gpu_format, gpu_formats,
                         static_cast<int>(std::size(gpu_formats)));
            ImGui::TreePop();
        }

        if (ImGui::Button("Reset defaults")) settings = {};
        ImGui::TextWrapped("Defaults are based on the RenoDX addon example: Default style, "
                           "preset 0, Intensity/Tone/Structure 1, Skin -1, optional inputs absent.");

        const bool backend_missing = !fs::is_regular_file(backend);
        const bool runtime_missing =
            !fs::is_regular_file(backend_working_directory / L"nvngx_dlssnr.dll");
        if (!app_error.empty() || backend_missing || runtime_missing) {
            ImGui::SeparatorText("Error");
            if (!app_error.empty()) {
                ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "%s", app_error.c_str());
            }
            if (backend_missing) {
                ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f},
                                   "dlssnr-image.exe is missing beside the GUI.");
            }
            if (runtime_missing) {
                ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f},
                                   "nvngx_dlssnr.dll was not found beside the GUI.");
            }
        }
        ImGui::PopItemWidth();
        ModelSettings comparable_before = before;
        comparable_before.diagnostics = settings.diagnostics;
        if (!(settings == comparable_before)) request_live_preview();
    }

    void draw() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("dlss5-nr-media-poc", nullptr, flags);
        draw_toolbar();
        ImGui::Separator();
        if (ImGui::BeginTable("main-layout", 3,
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthFixed, 300.0f);
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Settings", ImGuiTableColumnFlags_WidthFixed, 315.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("queue-panel", ImVec2(0, 0));
            draw_queue();
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("preview-panel", ImVec2(0, 0));
            draw_preview();
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(2);
            ImGui::BeginChild("settings-panel", ImVec2(0, 0));
            ImGui::BeginDisabled(queue_active());
            draw_settings();
            ImGui::EndDisabled();
            draw_output_options();
            ImGui::EndChild();
            ImGui::EndTable();
        }
        ImGui::End();
    }
};

int run_gui(HINSTANCE instance, int show_command) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check_hr(com_result, "Initialize COM");
    check_hr(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&g_wic)),
             "Create WIC imaging factory");

    ImGui_ImplWin32_EnableDpiAwareness();
    const WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW), CS_CLASSDC, wnd_proc, 0, 0, instance, nullptr, nullptr, nullptr,
        nullptr, L"dlss5-nr-media-poc-gui", nullptr};
    if (!RegisterClassExW(&window_class)) throw std::runtime_error("Register window class failed");
    RECT rect{0, 0, 1500, 900};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(window_class.lpszClassName, L"dlss5-nr-media-poc",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                              instance, nullptr);
    if (!hwnd) throw std::runtime_error("Create window failed");
    DragAcceptFiles(hwnd, TRUE);
    if (!create_device(hwnd)) throw std::runtime_error("Create D3D12 GUI device failed");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale;
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr,
                                 io.Fonts->GetGlyphRangesCyrillic());

    if (!ImGui_ImplWin32_Init(hwnd)) throw std::runtime_error("Initialize ImGui Win32 failed");
    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = g_device;
    init_info.CommandQueue = g_queue;
    init_info.NumFramesInFlight = kFramesInFlight;
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = g_srv_heap;
    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        g_srv_allocator.alloc(cpu, gpu);
    };
    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
        g_srv_allocator.free(cpu, gpu);
    };
    if (!ImGui_ImplDX12_Init(&init_info)) throw std::runtime_error("Initialize ImGui D3D12 failed");

    (void)show_command;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    bool done = false;
    {
        App app(hwnd);
        while (!done) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT) done = true;
            }
            if (done) break;
            if ((g_swap_chain_occluded &&
                 g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) ||
                IsIconic(hwnd)) {
                Sleep(10);
                continue;
            }
            g_swap_chain_occluded = false;

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            try {
                app.tick();
                app.draw();
            } catch (const std::exception& error) {
                app.app_error = error.what();
                app.draw();
            }
            ImGui::Render();

            FrameContext* frame = wait_for_next_frame();
            const UINT back_buffer = g_swap_chain->GetCurrentBackBufferIndex();
            check_hr(frame->allocator->Reset(), "Reset GUI command allocator");
            check_hr(g_command_list->Reset(frame->allocator, nullptr), "Reset GUI command list");
            app.record_gpu_work(g_command_list);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_render_targets[back_buffer];
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_command_list->ResourceBarrier(1, &barrier);
            constexpr float clear_color[4] = {0.055f, 0.060f, 0.075f, 1.0f};
            g_command_list->ClearRenderTargetView(g_rtv_handles[back_buffer], clear_color, 0,
                                                   nullptr);
            g_command_list->OMSetRenderTargets(1, &g_rtv_handles[back_buffer], FALSE, nullptr);
            g_command_list->SetDescriptorHeaps(1, &g_srv_heap);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_command_list);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            g_command_list->ResourceBarrier(1, &barrier);
            check_hr(g_command_list->Close(), "Close GUI command list");
            ID3D12CommandList* lists[] = {g_command_list};
            g_queue->ExecuteCommandLists(1, lists);
            check_hr(g_queue->Signal(g_fence, ++g_last_fence), "Signal GUI frame");
            frame->fence_value = g_last_fence;

            const HRESULT present = g_swap_chain->Present(1, 0);
            g_swap_chain_occluded = present == DXGI_STATUS_OCCLUDED;
            if (FAILED(present) && !g_swap_chain_occluded) check_hr(present, "Present GUI frame");
            ++g_frame_index;
        }
    }

    wait_for_gpu();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_wic.Reset();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(window_class.lpszClassName, instance);
    CoUninitialize();
    return 0;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show_command) {
    try {
        return run_gui(instance, show_command);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "dlss5-nr-media-poc - fatal error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}

#include "gui_processor.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void check_hr(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::format("{} failed: HRESULT 0x{:08X}", operation,
                                             static_cast<std::uint32_t>(result)));
    }
}

fs::path executable_directory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("GetModuleFileNameW failed");
    path.resize(length);
    return fs::path(path).parent_path();
}

struct D3D12Context {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;

    D3D12Context() {
        ComPtr<IDXGIFactory6> factory;
        check_hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
        ComPtr<IDXGIAdapter4> adapter;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))); ++i) {
            DXGI_ADAPTER_DESC3 desc{};
            if (SUCCEEDED(adapter->GetDesc3(&desc)) &&
                !(desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) && desc.VendorId == 0x10DE &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                            IID_PPV_ARGS(&device)))) {
                break;
            }
            adapter.Reset();
        }
        if (!device) throw std::runtime_error("No NVIDIA D3D12 FL12_0 adapter found");
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check_hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)),
                 "Create smoke command queue");
    }
};

LiveImage load_image(IWICImagingFactory* factory, const fs::path& path) {
    ComPtr<IWICBitmapDecoder> decoder;
    check_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnLoad, &decoder),
             "Open smoke input");
    ComPtr<IWICBitmapFrameDecode> frame;
    check_hr(decoder->GetFrame(0, &frame), "Decode smoke input");
    LiveImage image;
    check_hr(frame->GetSize(&image.width, &image.height), "Read smoke input size");
    ComPtr<IWICBitmapSource> converted;
    check_hr(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &converted),
             "Convert smoke input");
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    check_hr(converted->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(image.rgba.size()),
                                   image.rgba.data()),
             "Copy smoke input");
    return image;
}

void save_image(IWICImagingFactory* factory, const fs::path& path, const LiveImage& image) {
    fs::create_directories(path.parent_path());
    ComPtr<IWICStream> stream;
    check_hr(factory->CreateStream(&stream), "Create smoke output stream");
    check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open smoke output");
    ComPtr<IWICBitmapEncoder> encoder;
    check_hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
             "Create smoke PNG encoder");
    check_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
             "Initialize smoke PNG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    check_hr(encoder->CreateNewFrame(&frame, &properties), "Create smoke PNG frame");
    check_hr(frame->Initialize(properties.Get()), "Initialize smoke PNG frame");
    check_hr(frame->SetSize(image.width, image.height), "Set smoke PNG size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check_hr(frame->SetPixelFormat(&format), "Set smoke PNG format");
    auto bgra = image.rgba;
    for (std::size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    check_hr(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(bgra.size()),
                                bgra.data()),
             "Write smoke PNG");
    check_hr(frame->Commit(), "Commit smoke PNG frame");
    check_hr(encoder->Commit(), "Commit smoke PNG");
}

int wmain(int argc, wchar_t** argv) try {
    if (argc < 3) {
        std::cerr << "usage: dlssnr-gui-smoke input output [style] [intensity] "
                     "[second-output second-style second-intensity]\n";
        return 2;
    }
    check_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
    ComPtr<IWICImagingFactory> wic;
    check_hr(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wic)),
             "Create WIC factory");
    auto input = std::make_shared<LiveImage>(load_image(wic.Get(), argv[1]));
    LiveSettings settings;
    if (argc > 3) settings.style = std::stoi(argv[3]);
    if (argc > 4) settings.intensity = std::stof(argv[4]);
    D3D12Context d3d;
    LiveProcessor processor(fs::current_path(), executable_directory(), d3d.device.Get(),
                            d3d.queue.Get());
    processor.request(1, input, settings);
    std::optional<LiveResult> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (std::chrono::steady_clock::now() < deadline && !(result = processor.take_result())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!result) throw std::runtime_error("Live processor timed out");
    if (!result->error.empty()) throw std::runtime_error(result->error);
    LiveImage output = readback_live_image(d3d.queue.Get(), *result);
    save_image(wic.Get(), argv[2], output);

    std::uint64_t absolute_sum = 0;
    unsigned max_difference = 0;
    std::size_t samples = 0;
    if (input->width == output.width && input->height == output.height) {
        for (std::size_t i = 0; i < input->rgba.size(); i += 4) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const unsigned difference = static_cast<unsigned>(std::abs(
                    static_cast<int>(input->rgba[i + channel]) -
                    static_cast<int>(output.rgba[i + channel])));
                absolute_sum += difference;
                max_difference = std::max(max_difference, difference);
                ++samples;
            }
        }
    }
    std::cout << std::format("OK {}x{} -> {}x{}, {:.1f} ms, MAE {:.6f}, max {}/255\n",
                             input->width, input->height, output.width, output.height,
                             result->milliseconds,
                             samples ? static_cast<double>(absolute_sum) / samples / 255.0 : 0.0,
                             max_difference);
    if (argc > 6) {
        settings.style = std::stoi(argv[6]);
        if (argc > 7) settings.intensity = std::stof(argv[7]);
        processor.request(2, input, settings);
        result.reset();
        const auto second_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (std::chrono::steady_clock::now() < second_deadline &&
               !(result = processor.take_result())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!result) throw std::runtime_error("Second live evaluation timed out");
        if (!result->error.empty()) throw std::runtime_error(result->error);
        output = readback_live_image(d3d.queue.Get(), *result);
        save_image(wic.Get(), argv[5], output);
        std::cout << std::format("REUSED feature/resources: style {}, {:.1f} ms -> {}\n",
                                 settings.style, result->milliseconds,
                                 fs::path(argv[5]).string());
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 1;
}

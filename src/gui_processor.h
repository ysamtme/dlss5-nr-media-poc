#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct LiveImage {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> rgba;
};

struct LiveSettings {
    int style = 0;
    int preset = 0;
    float intensity = 1.0f;
    float tone = 1.0f;
    float structure = 1.0f;
    float skin = -1.0f;
    bool auto_mask = false;
    bool enabled = true;
    bool ui_correction = false;
    int motion = 0;
    float mv_scale_x = 1.0f;
    float mv_scale_y = 1.0f;
    int depth = 0;
    bool depth_inverted = true;
    int control_mask = 0;
    int control_mask_format = 1;
    float control_mask_value = 1.0f;
    int transfer = 0;
    int gpu_format = 0;
    bool diagnostics = false;
    int output_scale = 1;

    bool operator==(const LiveSettings&) const = default;
};

struct LiveResult {
    std::uint64_t revision = 0;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> resources;
    unsigned width = 0;
    unsigned height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool encode_srgb = false;
    unsigned slot = 0;
    std::uint64_t generation = 0;
    double milliseconds = 0.0;
    std::string error;
};

class LiveProcessor {
public:
    LiveProcessor(std::filesystem::path dll_directory,
                  std::filesystem::path executable_directory,
                  ID3D12Device* device, ID3D12CommandQueue* queue);
    ~LiveProcessor();
    LiveProcessor(const LiveProcessor&) = delete;
    LiveProcessor& operator=(const LiveProcessor&) = delete;

    void request(std::uint64_t revision, std::shared_ptr<const LiveImage> image,
                 LiveSettings settings);
    std::optional<LiveResult> take_result();
    bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

LiveImage readback_live_image(ID3D12CommandQueue* queue, const LiveResult& result);

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace model1 {

// Owns the host window, renderer and screen texture. A presentation layer:
// it receives emulator memory as plain byte views and never touches the bus
// or CPU.
//
// Frame pipeline (once per emulated frame, during VBlank):
//   update_framebuffer(vram)  VRAM -> internal 32-bit pixel buffer
//   present_frame()           pixel buffer -> streaming texture -> window
//
// Debug VRAM layout (temporary, until the real tile/polygon layers exist):
//   496x384 pixels, row-major, 2 bytes per pixel, little-endian, starting
//   at VRAM offset 0 (380,928 bytes). Each pixel uses the Model 1 colour
//   format, xBGR 5:5:5 with an intensity bit:
//     bits  0-4  red      bits 10-14  blue
//     bits  5-9  green    bit  15     intensity (0 = half brightness)
//   While VRAM is entirely zero, a test pattern is shown instead.
class VideoManager {
public:
    // Native Sega Model 1 output resolution.
    static constexpr int k_native_width = 496;
    static constexpr int k_native_height = 384;
    static constexpr std::size_t k_pixel_count =
        static_cast<std::size_t>(k_native_width) * static_cast<std::size_t>(k_native_height);
    static constexpr std::size_t k_debug_vram_bytes = k_pixel_count * 2;

    VideoManager() = default;
    ~VideoManager();

    VideoManager(const VideoManager&) = delete;
    VideoManager& operator=(const VideoManager&) = delete;
    VideoManager(VideoManager&&) = delete;
    VideoManager& operator=(VideoManager&&) = delete;

    // Creates the window, renderer and streaming texture.
    // Requires SDL_INIT_VIDEO to be active.
    [[nodiscard]] bool initialize(const char* title);
    void shutdown();

    // Rebuilds the pixel buffer from VRAM, or from the test pattern while
    // VRAM is entirely zero.
    void update_framebuffer(std::span<const uint8_t> vram);

    // Uploads the pixel buffer to the texture and presents it.
    void present_frame();

    // The current frame, one 0xAARRGGBB value per pixel, row-major.
    [[nodiscard]] std::span<const uint32_t> pixels() const { return m_pixels; }

    // Converts one Model 1 15-bit colour (+ intensity bit) to 0xAARRGGBB.
    [[nodiscard]] static uint32_t decode_color(uint16_t color);

private:
    enum class Source { None, TestPattern, Vram };

    void build_test_pattern();
    void mirror_vram(std::span<const uint8_t> vram);
    void draw_test_pattern();

    SDL_Window*   m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture = nullptr;

    // Host-side buffers (not emulated memory), so heap storage is fine.
    // Pixel format matches SDL_PIXELFORMAT_ARGB8888: 0xAARRGGBB per uint32_t.
    std::vector<uint32_t> m_pixels = std::vector<uint32_t>(k_pixel_count, 0);
    std::vector<uint32_t> m_test_pattern = std::vector<uint32_t>(k_pixel_count, 0);

    Source   m_source = Source::None;
    uint64_t m_frame_count = 0;
};

} // namespace model1

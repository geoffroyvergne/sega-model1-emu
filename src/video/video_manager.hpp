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
// it receives finished frames from the emulator core and never interprets
// emulated video memory itself.
//
// Frame pipeline (once per emulated frame, during VBlank):
//   update_framebuffer(frame)  core frame -> internal pixel buffer
//   present_frame()            pixel buffer -> streaming texture -> window
class VideoManager {
public:
    // Native Sega Model 1 output resolution.
    static constexpr int k_native_width = 496;
    static constexpr int k_native_height = 384;
    static constexpr std::size_t k_pixel_count =
        static_cast<std::size_t>(k_native_width) * static_cast<std::size_t>(k_native_height);

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

    // Copies a finished frame (0xAARRGGBB per pixel, row-major, 496x384).
    // A frame of the wrong size is rejected and logged.
    void update_framebuffer(std::span<const uint32_t> frame);

    // Uploads the pixel buffer to the texture and presents it.
    void present_frame();

    // The frame that will be presented next.
    [[nodiscard]] std::span<const uint32_t> pixels() const { return m_pixels; }

private:
    SDL_Window*   m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture = nullptr;

    // Host-side buffer (not emulated memory), so heap storage is fine.
    // Pixel format matches SDL_PIXELFORMAT_ARGB8888: 0xAARRGGBB per uint32_t.
    std::vector<uint32_t> m_pixels = std::vector<uint32_t>(k_pixel_count, 0xFF000000u);
    bool m_size_error_logged = false;
};

} // namespace model1

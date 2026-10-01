#include "video/video_manager.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace model1 {

VideoManager::~VideoManager()
{
    shutdown();
}

bool VideoManager::initialize(const char* title)
{
    m_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        k_native_width, k_native_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (m_window == nullptr) {
        std::cerr << "[Video] SDL_CreateWindow failed: " << SDL_GetError() << '\n';
        return false;
    }

    // No PRESENTVSYNC: frame pacing is owned by the main loop, not the display.
    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
    if (m_renderer == nullptr) {
        std::cerr << "[Video] SDL_CreateRenderer failed: " << SDL_GetError() << '\n';
        shutdown();
        return false;
    }

    // Keep the 496x384 aspect ratio when the window is resized.
    SDL_RenderSetLogicalSize(m_renderer, k_native_width, k_native_height);

    m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, k_native_width, k_native_height);
    if (m_texture == nullptr) {
        std::cerr << "[Video] SDL_CreateTexture failed: " << SDL_GetError() << '\n';
        shutdown();
        return false;
    }
    // Sharp pixels when the window is scaled up.
    SDL_SetTextureScaleMode(m_texture, SDL_ScaleModeNearest);

    std::cerr << "[Video] Window created (" << k_native_width << "x" << k_native_height << ")\n";
    return true;
}

void VideoManager::shutdown()
{
    if (m_texture != nullptr) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    if (m_renderer != nullptr) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

void VideoManager::update_framebuffer(std::span<const uint32_t> frame)
{
    if (frame.size() != k_pixel_count) {
        if (!m_size_error_logged) {
            std::cerr << "[Video] ERROR: frame has " << frame.size() << " pixels, expected "
                      << k_pixel_count << "; keeping the previous frame\n";
            m_size_error_logged = true;
        }
        return;
    }
    std::copy(frame.begin(), frame.end(), m_pixels.begin());
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

void VideoManager::present_frame()
{
    if (m_renderer == nullptr || m_texture == nullptr) {
        return;
    }

    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);

    void* texture_pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(m_texture, nullptr, &texture_pixels, &pitch) != 0) {
        std::cerr << "[Video] SDL_LockTexture failed: " << SDL_GetError() << '\n';
        return;
    }

    // The texture's row pitch may include padding, so copy row by row unless
    // it matches the packed buffer exactly.
    constexpr std::size_t row_bytes = static_cast<std::size_t>(k_native_width) * sizeof(uint32_t);
    auto* dst = static_cast<uint8_t*>(texture_pixels);
    if (static_cast<std::size_t>(pitch) == row_bytes) {
        std::memcpy(dst, m_pixels.data(), row_bytes * static_cast<std::size_t>(k_native_height));
    } else {
        for (int y = 0; y < k_native_height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(pitch),
                &m_pixels[static_cast<std::size_t>(y * k_native_width)], row_bytes);
        }
    }
    SDL_UnlockTexture(m_texture);

    SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);
}

} // namespace model1

#include "video/video_manager.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

namespace model1 {

namespace {

constexpr uint32_t k_opaque = 0xFF000000u;

constexpr uint32_t argb(uint32_t r, uint32_t g, uint32_t b)
{
    return k_opaque | (r << 16) | (g << 8) | b;
}

// Expands a 5-bit channel to 8 bits so 0x1F maps to 0xFF exactly.
constexpr uint32_t expand5(uint32_t channel)
{
    return (channel << 3) | (channel >> 2);
}

} // namespace

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

    build_test_pattern();

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

uint32_t VideoManager::decode_color(uint16_t color)
{
    uint32_t r = expand5(color & 0x1Fu);
    uint32_t g = expand5((color >> 5) & 0x1Fu);
    uint32_t b = expand5((color >> 10) & 0x1Fu);
    if ((color & 0x8000u) == 0) {
        // Intensity bit clear: half brightness.
        r >>= 1;
        g >>= 1;
        b >>= 1;
    }
    return argb(r, g, b);
}

void VideoManager::update_framebuffer(std::span<const uint8_t> vram)
{
    ++m_frame_count;

    const bool vram_empty = std::all_of(vram.begin(), vram.end(),
        [](uint8_t byte) { return byte == 0; });
    const bool vram_usable = vram.size() >= k_debug_vram_bytes;
    const Source source = (vram_empty || !vram_usable) ? Source::TestPattern : Source::Vram;

    if (source != m_source) {
        if (source == Source::Vram) {
            std::cerr << "[Video] VRAM has data: mirroring VRAM to screen\n";
        } else if (!vram_usable) {
            std::cerr << "[Video] VRAM is " << vram.size() << " bytes, need " << k_debug_vram_bytes
                      << " for the debug framebuffer: showing test pattern\n";
        } else {
            std::cerr << "[Video] VRAM is empty: showing test pattern\n";
        }
        m_source = source;
    }

    if (source == Source::Vram) {
        mirror_vram(vram);
    } else {
        draw_test_pattern();
    }
}

void VideoManager::mirror_vram(std::span<const uint8_t> vram)
{
    // Caller guarantees vram.size() >= k_debug_vram_bytes.
    const uint8_t* src = vram.data();
    for (std::size_t i = 0; i < k_pixel_count; ++i) {
        // Little-endian 16-bit pixel, matching the V60's byte order.
        const auto color = static_cast<uint16_t>(src[2 * i] | (src[2 * i + 1] << 8));
        m_pixels[i] = decode_color(color);
    }
}

// Built once: colour bars (top), a 32-step grey ramp (middle, one step per
// 5-bit level), red/green/blue ramps (bottom), a 16-pixel grid over the ramps,
// and a 1-pixel white border to show that no edge of the 496x384 image is
// cropped.
void VideoManager::build_test_pattern()
{
    static constexpr std::array<uint32_t, 8> k_bars = {
        argb(0xFF, 0xFF, 0xFF), argb(0xFF, 0xFF, 0x00), argb(0x00, 0xFF, 0xFF),
        argb(0x00, 0xFF, 0x00), argb(0xFF, 0x00, 0xFF), argb(0xFF, 0x00, 0x00),
        argb(0x00, 0x00, 0xFF), argb(0x00, 0x00, 0x00),
    };
    constexpr int bars_end = k_native_height / 2;          // rows   0-191
    constexpr int grey_end = bars_end + k_native_height / 4; // rows 192-287
    constexpr int ramp_height = (k_native_height - grey_end) / 3; // 32 rows each

    for (int y = 0; y < k_native_height; ++y) {
        for (int x = 0; x < k_native_width; ++x) {
            const auto level = static_cast<uint32_t>((x * 32) / k_native_width); // 0-31
            uint32_t color = 0;
            if (y < bars_end) {
                color = k_bars[static_cast<std::size_t>((x * 8) / k_native_width)];
            } else if (y < grey_end) {
                const uint32_t v = expand5(level);
                color = argb(v, v, v);
            } else {
                const int band = (y - grey_end) / ramp_height;
                const uint32_t v = expand5(level);
                color = band == 0 ? argb(v, 0, 0) : band == 1 ? argb(0, v, 0) : argb(0, 0, v);
            }

            const bool grid = y >= bars_end && (x % 16 == 0 || y % 16 == 0);
            const bool border = x == 0 || y == 0 || x == k_native_width - 1 || y == k_native_height - 1;
            if (border) {
                color = argb(0xFF, 0xFF, 0xFF);
            } else if (grid) {
                color = argb(0x40, 0x40, 0x40);
            }
            m_test_pattern[static_cast<std::size_t>(y * k_native_width + x)] = color;
        }
    }
}

// Copies the cached pattern and overlays a marker that sweeps across the
// colour bars, so a frozen image is easy to tell apart from a live one.
void VideoManager::draw_test_pattern()
{
    std::copy(m_test_pattern.begin(), m_test_pattern.end(), m_pixels.begin());

    const auto marker_x = static_cast<int>((m_frame_count * 2) % static_cast<uint64_t>(k_native_width - 8));
    for (int y = 1; y < k_native_height / 2; ++y) {
        uint32_t* row = &m_pixels[static_cast<std::size_t>(y * k_native_width + marker_x)];
        std::fill(row, row + 8, argb(0x80, 0x80, 0x80));
    }
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

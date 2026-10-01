#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace model1 {

// Sega System 24 tilemap chip, as used on Model 1 for 2D layers (HUD, text,
// backgrounds), plus the board's colour output. Renders one 496x384 frame
// from the contents of tile RAM, character RAM and palette RAM.
//
// Formats follow MAME's segaic24 tilemap device and model1 video driver.
//
// Character RAM (512 KB at 0x780000): 16384 tiles of 8x8 pixels, 4 bits per
// pixel, 32 bytes per tile. Each 16-bit word holds 4 pixels, leftmost pixel in
// bits 15-12; each row is 2 words. Pixel value 0 is transparent.
//
// Tile RAM (64 KB at 0x700000), in 16-bit words:
//   0x0000-0x3FFF  tilemaps 0-3, 64x64 entries each (512x512 pixels):
//                    bits 13-0  tile number (Model 1 tile mask 0x3FFF)
//                    bits 14-7  palette (16 colours each; overlaps the tile
//                               number bits, as in MAME's implementation)
//                    bit  15    priority: 1 = drawn above the polygons
//   0x4000-0x47FF  per-line horizontal scroll tables, 0x200 words per tilemap
//   0x5000-0x5003  horizontal scroll, tilemaps 0-3 (bit 15 = per-line mode)
//   0x5004-0x5007  vertical scroll, tilemaps 0-3 (bit 15 = layer disabled);
//                  bits 14-13 of words 0x5004 / 0x5006 select special
//                  split modes for pairs 0/1 and 2/3 (not emulated)
//   0x6000-0x67FF  window mask for pair 0/1, 0x6800-0x6FFF for pair 2/3:
//                  4 words per screen line, one bit per 8-pixel column
//                  (bit 15 of word 0 = columns 0-7). Even tilemaps draw where
//                  the bit is 0, odd tilemaps where it is 1.
//
// Palette RAM (16 KB at 0x900000): 8192 16-bit colours, xBGR 5:5:5 with an
// intensity bit (bit 15 clear = half brightness). Pen = palette * 16 + pixel.
//
// Layer order (Model 1 screen update, back to front):
//   render_background(): tilemaps 3, 2 low priority, opaque
//                        tilemaps 1, 0 low priority, transparent
//   [3D polygons, drawn into the same frame by the PolygonRenderer]
//   render_foreground(): tilemaps 3, 2, 1, 0 high priority, transparent (HUD)
class TilemapRenderer {
public:
    static constexpr int k_width = 496;
    static constexpr int k_height = 384;
    static constexpr std::size_t k_pixel_count =
        static_cast<std::size_t>(k_width) * static_cast<std::size_t>(k_height);

    static constexpr std::size_t k_tile_ram_bytes = 0x10000;
    static constexpr std::size_t k_char_ram_bytes = 0x80000;
    static constexpr std::size_t k_palette_ram_bytes = 0x4000;

    TilemapRenderer();

    // Draws the low-priority layers over pen 0, replacing every pixel of
    // `frame` (k_pixel_count pixels, 0xAARRGGBB, row-major).
    void render_background(std::span<const uint8_t> tile_ram, std::span<const uint8_t> char_ram,
                           std::span<const uint8_t> palette_ram, std::span<uint32_t> frame);

    // Draws the high-priority layers over the existing contents of `frame`.
    void render_foreground(std::span<const uint8_t> tile_ram, std::span<const uint8_t> char_ram,
                           std::span<const uint8_t> palette_ram, std::span<uint32_t> frame);

    // Converts one Model 1 colour (xBGR 5:5:5 + intensity bit) to 0xAARRGGBB.
    [[nodiscard]] static uint32_t decode_color(uint16_t color);

private:
    enum class DrawMode { Opaque, Transparent };

    // Validates the inputs and prepares the palette for a pass.
    bool begin_pass(std::span<const uint8_t> tile_ram, std::span<const uint8_t> char_ram,
                    std::span<const uint8_t> palette_ram, std::span<uint32_t> frame);

    // layer = tilemap * 2 + priority (0 = low, 1 = high), as in MAME.
    void draw_layer(int layer, DrawMode mode);

    [[nodiscard]] uint16_t tile_word(std::size_t index) const;
    [[nodiscard]] uint16_t char_word(std::size_t index) const;

    // Inputs of the frame being rendered.
    std::span<const uint8_t> m_tile_ram;
    std::span<const uint8_t> m_char_ram;

    // Pen (palette index) per pixel for the current pass, resolved to colours
    // at the end of the pass. k_no_pen marks pixels the pass did not draw.
    static constexpr uint16_t k_no_pen = 0xFFFF;
    std::vector<uint16_t> m_pens = std::vector<uint16_t>(k_pixel_count, 0);

    // Palette RAM decoded to colours for the current frame (pens 0-4095).
    std::vector<uint32_t> m_palette = std::vector<uint32_t>(4096, 0xFF000000u);

    // Last special-mode control value seen per tilemap pair (for logging).
    std::array<uint16_t, 2> m_logged_special_mode{};
};

} // namespace model1

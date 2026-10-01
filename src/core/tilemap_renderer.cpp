#include "core/tilemap_renderer.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <iostream>

namespace model1 {

namespace {

// Tile RAM word offsets.
constexpr std::size_t k_tilemap_words      = 0x1000; // 64 x 64 entries
constexpr std::size_t k_line_scroll_base   = 0x4000;
constexpr std::size_t k_line_scroll_stride = 0x200;
constexpr std::size_t k_hscroll_base       = 0x5000;
constexpr std::size_t k_vscroll_base       = 0x5004;
constexpr std::size_t k_mask_base_pair01   = 0x6000;
constexpr std::size_t k_mask_base_pair23   = 0x6800;

constexpr uint16_t k_tile_mask      = 0x3FFF; // 16384 tiles in 512 KB
constexpr uint16_t k_layer_disabled = 0x8000; // vertical scroll bit 15
constexpr uint16_t k_line_scroll_on = 0x8000; // horizontal scroll bit 15
constexpr uint16_t k_special_modes  = 0x6000;

constexpr int k_map_size_mask = 511; // tilemaps are 512x512 pixels and wrap

constexpr uint32_t expand5(uint32_t channel)
{
    return (channel << 3) | (channel >> 2);
}

uint16_t read_le16(std::span<const uint8_t> bytes, std::size_t word_index)
{
    const std::size_t i = word_index * 2;
    return static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
}

} // namespace

TilemapRenderer::TilemapRenderer() = default;

uint32_t TilemapRenderer::decode_color(uint16_t color)
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
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint16_t TilemapRenderer::tile_word(std::size_t index) const
{
    return read_le16(m_tile_ram, index);
}

uint16_t TilemapRenderer::char_word(std::size_t index) const
{
    return read_le16(m_char_ram, index);
}

bool TilemapRenderer::begin_pass(std::span<const uint8_t> tile_ram, std::span<const uint8_t> char_ram,
                                 std::span<const uint8_t> palette_ram, std::span<uint32_t> frame)
{
    if (tile_ram.size() < k_tile_ram_bytes || char_ram.size() < k_char_ram_bytes
        || palette_ram.size() < k_palette_ram_bytes || frame.size() < k_pixel_count) {
        std::cerr << "[Tilemap] ERROR: video memory or frame views too small, layers not rendered\n";
        return false;
    }
    m_tile_ram = tile_ram;
    m_char_ram = char_ram;
    // Pens reach at most 255 * 16 + 15: the first 4096 palette entries.
    for (std::size_t pen = 0; pen < m_palette.size(); ++pen) {
        m_palette[pen] = decode_color(read_le16(palette_ram, pen));
    }
    return true;
}

void TilemapRenderer::render_background(std::span<const uint8_t> tile_ram, std::span<const uint8_t> char_ram,
                                        std::span<const uint8_t> palette_ram, std::span<uint32_t> frame)
{
    if (!begin_pass(tile_ram, char_ram, palette_ram, frame)) {
        std::fill(frame.begin(), frame.end(), 0xFF000000u);
        return;
    }
    // Background: pen 0. Model 1 layer order (MAME screen_update_model1).
    std::fill(m_pens.begin(), m_pens.end(), uint16_t{0});
    draw_layer(6, DrawMode::Opaque);      // tilemap 3, low priority
    draw_layer(4, DrawMode::Opaque);      // tilemap 2, low priority
    draw_layer(2, DrawMode::Transparent); // tilemap 1, low priority
    draw_layer(0, DrawMode::Transparent); // tilemap 0, low priority

    for (std::size_t i = 0; i < k_pixel_count; ++i) {
        frame[i] = m_palette[m_pens[i]];
    }
    m_tile_ram = {};
    m_char_ram = {};
}

void TilemapRenderer::render_foreground(std::span<const uint8_t> tile_ram, std::span<const uint8_t> char_ram,
                                        std::span<const uint8_t> palette_ram, std::span<uint32_t> frame)
{
    if (!begin_pass(tile_ram, char_ram, palette_ram, frame)) {
        return;
    }
    std::fill(m_pens.begin(), m_pens.end(), k_no_pen);
    draw_layer(7, DrawMode::Transparent); // tilemap 3, high priority
    draw_layer(5, DrawMode::Transparent); // tilemap 2, high priority
    draw_layer(3, DrawMode::Transparent); // tilemap 1, high priority
    draw_layer(1, DrawMode::Transparent); // tilemap 0, high priority

    for (std::size_t i = 0; i < k_pixel_count; ++i) {
        if (m_pens[i] != k_no_pen) {
            frame[i] = m_palette[m_pens[i]];
        }
    }
    m_tile_ram = {};
    m_char_ram = {};
}

void TilemapRenderer::draw_layer(int layer, DrawMode mode)
{
    const auto tilemap = static_cast<std::size_t>(layer >> 1);
    const uint16_t priority = static_cast<uint16_t>(layer & 1);
    const uint16_t hscroll = tile_word(k_hscroll_base + tilemap);
    const uint16_t vscroll = tile_word(k_vscroll_base + tilemap);
    const uint16_t control = tile_word(k_vscroll_base + (tilemap & 2));

    if ((vscroll & k_layer_disabled) != 0) {
        return;
    }

    if ((control & k_special_modes) != 0) {
        // Split-screen / window special modes are not emulated; the layer is
        // drawn in the normal mode instead. Logged once per change.
        uint16_t& logged = m_logged_special_mode[tilemap >> 1];
        if (logged != (control & k_special_modes)) {
            logged = control & k_special_modes;
            std::cerr << "[Tilemap] WARNING: special mode " << ((control & k_special_modes) >> 13)
                      << " for tilemaps " << (tilemap & 2) << "/" << ((tilemap & 2) + 1)
                      << " not emulated, drawing in normal mode\n";
        }
    }

    const bool line_scroll = (hscroll & k_line_scroll_on) != 0;
    const std::size_t map_base = tilemap * k_tilemap_words;
    const std::size_t mask_base = tilemap >= 2 ? k_mask_base_pair23 : k_mask_base_pair01;
    const bool odd_tilemap = (tilemap & 1) != 0;
    const bool transparent = mode == DrawMode::Transparent;

    for (int y = 0; y < k_height; ++y) {
        // Scroll values are negated for X, as on the chip.
        const uint16_t line_h = line_scroll
            ? tile_word(k_line_scroll_base + tilemap * k_line_scroll_stride + static_cast<std::size_t>(y))
            : hscroll;
        const int scroll_x = (-static_cast<int>(line_h)) & k_map_size_mask;
        const int map_y = (static_cast<int>(vscroll) + y) & k_map_size_mask;
        const std::size_t map_row = map_base + static_cast<std::size_t>(map_y >> 3) * 64;
        const std::size_t char_row = static_cast<std::size_t>(map_y & 7) * 2;

        // Window mask for this line: 4 words, one bit per 8-pixel column.
        const std::size_t mask_row = mask_base + static_cast<std::size_t>(y) * 4;
        const std::array<uint16_t, 4> mask = {tile_word(mask_row), tile_word(mask_row + 1),
                                              tile_word(mask_row + 2), tile_word(mask_row + 3)};

        uint16_t* pen_row = &m_pens[static_cast<std::size_t>(y) * k_width];

        // Walk the line one tile row (up to 8 pixels) at a time.
        int x = 0;
        while (x < k_width) {
            const int map_x = (x + scroll_x) & k_map_size_mask;
            const int first_pixel = map_x & 7;
            const int run = std::min(8 - first_pixel, k_width - x);

            const uint16_t entry = tile_word(map_row + static_cast<std::size_t>(map_x >> 3));
            if (transparent && (entry >> 15) != priority) {
                x += run;
                continue;
            }
            const std::size_t char_base = static_cast<std::size_t>(entry & k_tile_mask) * 16 + char_row;
            // 8 pixels of this tile row, leftmost pixel in bits 31-28.
            const uint32_t row_pixels = (static_cast<uint32_t>(char_word(char_base)) << 16) | char_word(char_base + 1);
            const auto pen_base = static_cast<uint16_t>(((entry >> 7) & 0xFF) * 16);

            for (int i = 0; i < run; ++i, ++x) {
                // Window mask: which tilemap of the pair owns this 8-pixel column.
                const bool mask_bit = ((mask[static_cast<std::size_t>(x >> 7)] << ((x & 127) >> 3)) & 0x8000) != 0;
                if (mask_bit != odd_tilemap) {
                    continue;
                }
                const auto pixel = static_cast<uint16_t>((row_pixels >> (28 - 4 * (first_pixel + i))) & 0xF);
                if (transparent && pixel == 0) {
                    continue;
                }
                pen_row[x] = static_cast<uint16_t>(pen_base + pixel);
            }
        }
    }
}

} // namespace model1

// 2D layer tests: the System 24 tilemap renderer (tile RAM, character RAM,
// palette RAM -> 32-bit frame), memory-safety with worst-case inputs, and the
// HUD text written by the tile demo.
//
// Video memory is filled through the Bus, the way game code would.

#include "test_framework.hpp"

#include "core/bus.hpp"
#include "core/motherboard.hpp"
#include "core/tilemap_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using model1::Bus;
using model1::Motherboard;
using model1::TilemapRenderer;

constexpr uint32_t k_black = 0xFF000000;
constexpr uint32_t k_red = 0xFFFF0000;
constexpr uint32_t k_green = 0xFF00FF00;
constexpr uint32_t k_blue = 0xFF0000FF;
constexpr uint32_t k_white = 0xFFFFFFFF;

// Model 1 colours: xBGR 5:5:5, bit 15 = full intensity.
constexpr uint16_t k_c_red = 0x801F;
constexpr uint16_t k_c_green = 0x83E0;
constexpr uint16_t k_c_blue = 0xFC00;
constexpr uint16_t k_c_white = 0xFFFF;

struct TileRig {
    std::unique_ptr<Bus> bus = std::make_unique<Bus>();
    std::unique_ptr<TilemapRenderer> renderer = std::make_unique<TilemapRenderer>();
    std::vector<uint32_t> frame = std::vector<uint32_t>(TilemapRenderer::k_pixel_count, 0);

    void palette(uint32_t pen, uint16_t color) { bus->write_word(Bus::k_palette_base + pen * 2, color); }

    // One tile row: 8 pens, leftmost in bits 31-28.
    void tile_row(uint32_t tile, uint32_t row, uint32_t pens)
    {
        const uint32_t address = Bus::k_char_ram_base + tile * 32 + row * 4;
        bus->write_word(address, static_cast<uint16_t>(pens >> 16));
        bus->write_word(address + 2, static_cast<uint16_t>(pens));
    }
    void solid_tile(uint32_t tile, uint32_t pen)
    {
        for (uint32_t row = 0; row < 8; ++row) {
            tile_row(tile, row, pen * 0x11111111u);
        }
    }
    void entry(uint32_t tilemap, uint32_t col, uint32_t row, uint16_t value)
    {
        bus->write_word(Bus::k_tile_ram_base + (tilemap * 0x1000 + row * 64 + col) * 2, value);
    }
    void fill(uint32_t tilemap, uint16_t value)
    {
        for (uint32_t i = 0; i < 0x1000; ++i) {
            bus->write_word(Bus::k_tile_ram_base + (tilemap * 0x1000 + i) * 2, value);
        }
    }
    void reg(uint32_t word, uint16_t value) { bus->write_word(Bus::k_tile_ram_base + word * 2, value); }

    void render()
    {
        renderer->render_background(bus->tile_ram(), bus->char_ram(), bus->palette_ram(), frame);
        renderer->render_foreground(bus->tile_ram(), bus->char_ram(), bus->palette_ram(), frame);
    }
    uint32_t px(int x, int y) const { return frame[static_cast<std::size_t>(y * TilemapRenderer::k_width + x)]; }
};

} // namespace

// ---------------------------------------------------------------------------
// Colours and tile decoding
// ---------------------------------------------------------------------------

TEST_CASE(tilemap_color_decoding)
{
    CHECK_EQ(TilemapRenderer::decode_color(0x801F), k_red);
    CHECK_EQ(TilemapRenderer::decode_color(0xFFFF), k_white);
    CHECK_EQ(TilemapRenderer::decode_color(0x8000), k_black);
    CHECK_EQ(TilemapRenderer::decode_color(0x001F), 0xFF7F0000u); // intensity bit clear: half brightness
    CHECK_EQ(TilemapRenderer::decode_color(0x8010), 0xFF840000u); // 5-bit 16 -> 0x84
}

TEST_CASE(tilemap_empty_video_memory_is_pen_zero)
{
    TileRig rig;
    rig.render();
    CHECK_EQ(rig.px(0, 0), k_black);
    CHECK_EQ(rig.px(495, 383), k_black);
    rig.palette(0, k_c_blue); // pen 0 is the backdrop colour
    rig.render();
    CHECK_EQ(rig.px(200, 100), k_blue);
}

TEST_CASE(tilemap_pixel_order_inside_a_tile)
{
    // Leftmost pixel in bits 15-12 of the first word; rows are 2 words.
    TileRig rig;
    rig.palette(1, k_c_red);
    rig.palette(2, k_c_green);
    rig.palette(3, k_c_blue);
    rig.palette(4, k_c_white);
    rig.tile_row(5, 0, 0x12340000);
    rig.tile_row(5, 1, 0x00000004);
    rig.entry(2, 0, 0, 5); // tilemap 2 (opaque background), map cell (0, 0)
    rig.render();
    CHECK_EQ(rig.px(0, 0), k_red);
    CHECK_EQ(rig.px(1, 0), k_green);
    CHECK_EQ(rig.px(2, 0), k_blue);
    CHECK_EQ(rig.px(3, 0), k_white);
    CHECK_EQ(rig.px(4, 0), k_black);
    CHECK_EQ(rig.px(7, 1), k_white);
    CHECK_EQ(rig.px(8, 0), k_black);
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

TEST_CASE(tilemap_scroll_and_wraparound)
{
    TileRig rig;
    rig.palette(1, k_c_red);
    rig.solid_tile(1, 1);
    rig.entry(2, 10, 5, 1); // map pixels x 80-87, y 40-47
    rig.render();
    CHECK_EQ(rig.px(80, 40), k_red);
    CHECK_EQ(rig.px(79, 40), k_black);

    // 9-bit scroll values: horizontal v moves the layer right by v, vertical
    // v moves it up by v.
    rig.reg(0x5002, static_cast<uint16_t>((-30) & 0x1FF));
    rig.reg(0x5006, 16);
    rig.render();
    CHECK_EQ(rig.px(50, 24), k_red);
    CHECK_EQ(rig.px(57, 31), k_red);
    CHECK_EQ(rig.px(58, 24), k_black);

    // Tilemaps are 512x512 and wrap: the last cell appears at the origin.
    rig.entry(2, 10, 5, 0);
    rig.entry(2, 63, 63, 1);
    rig.reg(0x5002, 8);
    rig.reg(0x5006, 504);
    rig.render();
    CHECK_EQ(rig.px(0, 0), k_red);
    CHECK_EQ(rig.px(7, 7), k_red);
    CHECK_EQ(rig.px(8, 0), k_black);
}

TEST_CASE(tilemap_per_line_scroll_and_layer_disable)
{
    TileRig rig;
    rig.palette(1, k_c_red);
    rig.solid_tile(1, 1);
    rig.entry(2, 0, 0, 1);
    rig.reg(0x5006, 0x8000); // vertical scroll bit 15: tilemap 2 disabled
    rig.render();
    CHECK_EQ(rig.px(0, 0), k_black);

    rig.reg(0x5006, 0);
    rig.reg(0x5002, 0x8000);             // per-line horizontal scroll
    rig.reg(0x4000 + 2 * 0x200 + 3, 100); // line 3 moves right by 100
    rig.render();
    CHECK_EQ(rig.px(0, 0), k_red);
    CHECK_EQ(rig.px(100, 3), k_red);
    CHECK_EQ(rig.px(0, 3), k_black);
}

// ---------------------------------------------------------------------------
// Layering: transparency, priority, opaque background, window masks
// ---------------------------------------------------------------------------

TEST_CASE(tilemap_transparency_and_priority_order)
{
    TileRig rig;
    rig.palette(1, k_c_red);
    rig.palette(16 + 1, k_c_green);
    rig.palette(32 + 1, k_c_blue);
    rig.solid_tile(1, 1);             // palette 0 tile block
    rig.tile_row(129, 0, 0x01000000); // palette 1 block: one pixel at x = 1, row 0
    rig.solid_tile(257, 1);           // palette 2 block
    rig.fill(2, 1);                   // opaque red background
    rig.entry(0, 0, 0, 129);          // tilemap 0, low priority
    rig.render();
    CHECK_EQ(rig.px(0, 0), k_red);    // pixel value 0 is transparent
    CHECK_EQ(rig.px(1, 0), k_green);

    rig.entry(2, 0, 0, static_cast<uint16_t>(0x8000 | 257)); // tilemap 2, HIGH priority
    rig.render();
    CHECK_EQ(rig.px(1, 0), k_blue); // high priority beats low priority of any tilemap

    rig.entry(0, 0, 0, static_cast<uint16_t>(0x8000 | 129)); // tilemap 0, high priority: drawn last
    rig.render();
    CHECK_EQ(rig.px(1, 0), k_green);
    CHECK_EQ(rig.px(0, 0), k_blue);
}

TEST_CASE(tilemap_opaque_background_draws_pen_zero)
{
    TileRig rig;
    rig.palette(16, k_c_green); // palette 1, pen 0
    rig.fill(2, 128);           // palette 1, tile 128: all pixels 0
    rig.render();
    CHECK_EQ(rig.px(100, 100), k_green);
}

TEST_CASE(tilemap_window_mask_selects_tilemap_per_column)
{
    TileRig rig;
    rig.palette(1, k_c_red);
    rig.palette(2, k_c_blue);
    rig.solid_tile(1, 1);
    rig.solid_tile(2, 2);
    rig.fill(2, 1);
    rig.fill(3, 2);
    rig.reg(0x6800 + 10 * 4 + 0, 0x8000); // line 10, columns 0-7 -> tilemap 3
    rig.reg(0x6800 + 11 * 4 + 1, 0x4000); // line 11, columns 136-143 -> tilemap 3
    rig.render();
    CHECK_EQ(rig.px(0, 10), k_blue);
    CHECK_EQ(rig.px(7, 10), k_blue);
    CHECK_EQ(rig.px(8, 10), k_red);
    CHECK_EQ(rig.px(0, 9), k_red);
    CHECK_EQ(rig.px(136, 11), k_blue);
    CHECK_EQ(rig.px(143, 11), k_blue);
    CHECK_EQ(rig.px(135, 11), k_red);
    CHECK_EQ(rig.px(144, 11), k_red);
}

// ---------------------------------------------------------------------------
// Memory safety: worst-case values never index outside the buffers
// ---------------------------------------------------------------------------

TEST_CASE(tilemap_maximum_tile_index_and_palette_stay_in_bounds)
{
    // Entry 0xFFFF: tile 0x3FFF (the last 32 bytes of character RAM), palette
    // 0xFF (pens 4080-4095, the last decoded palette entries), high priority.
    TileRig rig;
    rig.fill(0, 0xFFFF);
    rig.tile_row(0x3FFF, 7, 0x0000000F); // last word of character RAM: 0x7FFFE
    rig.palette(0xFF * 16 + 15, k_c_green);
    rig.render();
    // Row 7, pixel 7 of every cell is pen 4095; everything else is transparent.
    CHECK_EQ(rig.px(7, 7), k_green);
    CHECK_EQ(rig.px(495, 383), k_green);
    CHECK_EQ(rig.px(6, 7), k_black);
}

TEST_CASE(tilemap_worst_case_registers_render_safely)
{
    // Every tile entry, scroll register, per-line scroll table entry and
    // window mask set to extreme values; special modes enabled. The render
    // must stay in bounds (run with -DMODEL1_SANITIZE=ON to have
    // AddressSanitizer prove it) and produce a full frame.
    TileRig rig;
    for (uint32_t word = 0; word < 0x8000; ++word) {
        rig.reg(word, 0xFFFF);
    }
    for (uint32_t tilemap = 0; tilemap < 4; ++tilemap) {
        rig.reg(0x5004 + tilemap, 0x7FFF); // enabled (bit 15 clear), special modes, max scroll
    }
    for (uint32_t byte = 0; byte < Bus::k_char_ram_size; byte += 2) {
        rig.bus->write_word(Bus::k_char_ram_base + byte, 0xFFFF);
    }
    for (uint32_t pen = 0; pen < 0x2000; ++pen) {
        rig.palette(pen, k_c_white);
    }
    rig.render();
    CHECK_EQ(rig.frame.size(), TilemapRenderer::k_pixel_count);
    CHECK_EQ(rig.px(0, 0), k_white);
    CHECK_EQ(rig.px(495, 383), k_white);
    CHECK(model1_test::captured_log().find("Special mode 3") != std::string::npos);
}

TEST_CASE(tilemap_undersized_buffers_are_refused)
{
    auto renderer = std::make_unique<TilemapRenderer>();
    std::vector<uint8_t> small(16);
    std::vector<uint32_t> frame(TilemapRenderer::k_pixel_count, k_red);
    renderer->render_background(small, small, small, frame);
    CHECK_EQ(frame[0], k_black); // cleared, nothing read from the small views
    std::fill(frame.begin(), frame.end(), k_red);
    renderer->render_foreground(small, small, small, frame);
    CHECK_EQ(frame[0], k_red);   // untouched

    TileRig rig;
    std::vector<uint32_t> short_frame(100, k_red);
    rig.renderer->render_background(rig.bus->tile_ram(), rig.bus->char_ram(), rig.bus->palette_ram(), short_frame);
    CHECK_EQ(short_frame.size(), 100u);
    CHECK(model1_test::captured_log().find("too small") != std::string::npos);
}

// ---------------------------------------------------------------------------
// HUD text from the tile demo
// ---------------------------------------------------------------------------

TEST_CASE(tilemap_demo_hud_text_renders)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    board->load_tile_demo();
    board->run_frame();
    const auto frame = board->frame();
    auto px = [&frame](uint32_t x, uint32_t y) { return frame[y * TilemapRenderer::k_width + x]; };

    // "SCORE" starts at map cell (1, 1): screen (8, 8). 'S' row 0 = 0x3C:
    // pixels 2-5 lit (white); row 1 pixel 3 is the drop shadow of row 0
    // pixel 2 (black).
    const uint32_t x0 = Motherboard::k_demo_score_col * 8;
    const uint32_t y0 = Motherboard::k_demo_score_row * 8;
    CHECK_EQ(px(x0 + 2, y0), k_white);
    CHECK_EQ(px(x0 + 5, y0), k_white);
    CHECK_EQ(px(x0 + 3, y0 + 1), k_black);
    // Glyph pixel 0 is transparent: the background (the colour bar in the
    // same column, below the text) shows through.
    CHECK_EQ(px(x0 + 0, y0), px(x0 + 0, 48));

    // "INSERT COIN" in yellow: 'I' row 0 = 0x3C.
    const uint32_t x1 = Motherboard::k_demo_coin_col * 8;
    const uint32_t y1 = Motherboard::k_demo_coin_row * 8;
    CHECK_EQ(px(x1 + 2, y1), 0xFFFFE700u); // rgb(31, 28, 0)
}

// ---------------------------------------------------------------------------
// Special split modes (tilemaps 2 / 3, as Virtua Racing's sky / landscape)
// ---------------------------------------------------------------------------

namespace {

// Tilemap 2 solid red, tilemap 3 solid green; window masks all set (in the
// normal mode they would hide tilemap 2 entirely).
TileRig split_rig()
{
    TileRig rig;
    rig.palette(1, k_c_red);
    rig.palette(2, k_c_green);
    rig.solid_tile(1, 1);
    rig.solid_tile(2, 2);
    rig.fill(2, 1);
    rig.fill(3, 2);
    for (uint32_t word = 0x6800; word < 0x7000; ++word) {
        rig.reg(word, 0xFFFF);
    }
    return rig;
}

} // namespace

TEST_CASE(tilemap_special_mode_1_splits_at_a_line)
{
    // Mode 1 (bits 14-13 of 0x5006 = 01): split at line (-vscroll) & 0x1FF.
    // vscroll 0x219C: -0x219C = 0xDE64 -> line 100, bit 9 set: tilemap 2
    // above, tilemap 3 below. Masks ignored.
    TileRig rig = split_rig();
    rig.reg(0x5006, 0x219C);
    rig.render();
    CHECK_EQ(rig.px(10, 50), k_red);
    CHECK_EQ(rig.px(400, 99), k_red);
    CHECK_EQ(rig.px(10, 100), k_green);
    CHECK_EQ(rig.px(400, 300), k_green);

    // 0x239C: -0x239C = 0xDC64 -> line 100 again, bit 9 clear: swapped.
    rig.reg(0x5006, 0x239C);
    rig.render();
    CHECK_EQ(rig.px(10, 50), k_green);
    CHECK_EQ(rig.px(10, 150), k_red);
}

TEST_CASE(tilemap_special_mode_2_splits_at_a_column)
{
    // Mode 2: split at column hscroll & 0x1FF (200), bit 9 set: tilemap 2
    // on the left.
    TileRig rig = split_rig();
    rig.reg(0x5006, 0x4000);
    rig.reg(0x5002, 0x200 | 200);
    rig.render();
    CHECK_EQ(rig.px(10, 10), k_red);
    CHECK_EQ(rig.px(199, 300), k_red);
    CHECK_EQ(rig.px(200, 10), k_green);
    CHECK_EQ(rig.px(495, 300), k_green);

    // Per-line horizontal scroll: each line has its own split column.
    rig.reg(0x5002, 0x8000);
    for (uint32_t y = 0; y < 384; ++y) {
        rig.reg(0x4000 + 2 * 0x200 + y, static_cast<uint16_t>(0x200 | (y < 192 ? 50 : 300)));
    }
    rig.render();
    CHECK_EQ(rig.px(60, 10), k_green);
    CHECK_EQ(rig.px(60, 300), k_red);
    CHECK_EQ(rig.px(310, 300), k_green);
}

#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace model1 {

// Model 1 3D rendering hardware - display-list interpreter and flat-shaded
// quad rasterizer.
//
// The V60 (usually with TGP help) builds a display list in display list RAM;
// this class reads it once per frame and draws into the frame, between the
// low- and high-priority tilemap layers. Formats and behaviour follow MAME's
// model1 video driver (model1_v.cpp), which is itself a high-level
// simulation of the board.
//
// Display list RAM (0x600000): two 64 KB lists of 16-bit words; a "long" is
// two words, low word first. Which list is read is chosen by the list
// control register at 0x680000 (two 16-bit words):
//   word 0 bit 2  automatic double buffering: lists swap every second frame
//          bit 3  list to use when automatic mode is off (1 = list 1)
//          bit 6  list in use (read back; reads also return bits 4-5 set)
//   word 1        rendering runs only while bits 4-0 are all set (0x1F)
//
// Commands (first long of each; offsets below are in words):
//   0x00  no-op                                            2 words
//   0x01  3D object: +2 colour table address, +4 model address (bit 23:
//         polygon RAM, else polygon ROM), +6 polygon count (0 = until the
//         end marker); see push_object                          8 words
//   0x41  3D object above the HUD (drawn in the same pass here)   8 words
//   0x02  direct polygons, see below                       variable
//   0x03  viewport: +4 centre x, +6 centre y, +8 left, +10 bottom, +12 right,
//         +14 top (16-bit; y values are stored as 422 - screen y)   16 words
//   0x04  colour table write: +2 address (>= 0x40000), +4 count - 1,
//         then one 16-bit entry every 2 words                    6 + 2n words
//   0x05  polygon data RAM upload: +2 address (>= 0x800000), +4 count n,
//         then one 32-bit word every 2 words (stored, see poly_ram_word)
//                                                                6 + 2n words
//   0x06  lighting parameters: +2 first slot (0-255), +4 count n, then one
//         32-bit word each: power | specular | ambient | diffuse (bytes,
//         high to low; stored, see light_param)                  6 + 2n words
//   0x07  mode word: bit 0 enables specular lighting; 0x08 select mode
//         (ignored)                                                4 words
//   0x09  zoom x, y (x4); 0x0A light direction x, y, z; 0x0B object matrix
//         (3x3 + translation, 12 floats); 0x0C view translation x, y
//                                                          6/8/26/6 words
//
// 3D objects (as MAME's model1_v.cpp): model points are transformed by the
// object matrix, projected (screen = centre + p/z * zoom + view
// translation), back-face culled, lit (ambient + diffuse + optional
// specular from the 0x06 lighting slot, through colour translation RAM)
// and clipped to the viewport's frustum (bottom, top, left, right planes),
// then depth-sorted with the direct polygons of the same viewport.
//   0x0F  end of list; 0xFFFFFFFF (erased / never-written list) also ends it
// A command whose length runs past the end of the 64 KB list ends the list.
//
// Direct polygons (0x02) describe a strip of quads in screen space:
//   +2 colour table address, +6/+8/+10 first point (x, y, z floats),
//   +14/+16/+18 second point; then records starting 18 words in, each:
//     +2 flags: bits 1-0 record type (0 = end of strip, 2 = one new point,
//               1/3 = two new points), bits 9-8 link mode, bit 12 advance
//               colour address, bit 13 stipple ("moire") fill
//     +4 luminance (bits 31-24)
//     one point:  +8/+10/+12 x, y, z               (record = 12 words)
//     two points: +8/+10/+12 first, +14 sort z, +16/+18/+20 second (20 words)
//   Each record with a non-zero link mode draws the quad
//   (previous second point, previous first point, new first, new second).
//   x and y are already projected: screen = (centre x + x, centre y - y).
//   z is only used to sort quads far-to-near (painter's algorithm).
//
// Colour: palette RAM entry 0x1000 + (colour table entry & 0x3FF) gives a
// 5:5:5 colour; each channel is then mapped through colour translation RAM
// (0x910000, red/green/blue tables of 0x2000 words) indexed by
// (channel << 8) | luminance level (0-63), output in bits 7-3.
//
// Quads are queued and drawn sorted far-to-near when a viewport command
// arrives and at the end of the list, clipped to the current viewport.
// A quad with only two distinct corners is drawn as a line (wireframe), one
// with a single distinct corner as a point.
class PolygonRenderer {
public:
    static constexpr int k_width = 496;
    static constexpr int k_height = 384;
    static constexpr std::size_t k_pixel_count =
        static_cast<std::size_t>(k_width) * static_cast<std::size_t>(k_height);

    static constexpr std::size_t k_list_words = 0x8000;              // 64 KB per list
    static constexpr std::size_t k_display_list_bytes = 2 * k_list_words * 2;
    static constexpr std::size_t k_palette_ram_bytes = 0x4000;
    static constexpr std::size_t k_color_xlat_bytes = 0xC000;
    static constexpr uint32_t k_color_table_base = 0x40000;
    static constexpr std::size_t k_color_table_words = 0xC0000;
    // Polygon data RAM written by command 0x05 (object data addresses
    // 0x800000 and up, next to the polygon ROM; MAME's m_poly_ram).
    static constexpr uint32_t k_poly_ram_base = 0x800000;
    static constexpr std::size_t k_poly_ram_words = 0x400000;
    // Lighting parameter slots written by command 0x06.
    static constexpr std::size_t k_light_param_count = 256;
    // Polygon (model) ROM: 16 MB of 32-bit words, read by command 0x01.
    static constexpr std::size_t k_poly_rom_words = 0x400000;

    // Command 0x06 entry: one 32-bit word = power << 24 | specular << 16 |
    // ambient << 8 | diffuse, the three factors as fractions of 255.
    struct LightParam {
        float   diffuse = 0;
        float   ambient = 0;
        float   specular = 0;
        uint8_t power = 0;
    };

    PolygonRenderer();

    void reset();

    // List control register handlers (0x680000, registered by the Motherboard).
    uint16_t read_list_control(uint32_t offset) const;
    void     write_list_control(uint32_t offset, uint16_t value);

    // Interprets the current display list and draws its polygons into
    // `frame` (k_pixel_count pixels, 0xAARRGGBB). `display_lists` is the
    // whole 128 KB display list RAM.
    void render(std::span<const uint8_t> display_lists, std::span<const uint8_t> palette_ram,
                std::span<const uint8_t> color_xlat, std::span<uint32_t> frame);

    // Called once per frame at VBlank: swaps lists in automatic mode.
    void end_frame();

    // Quads drawn by the last render() call (for diagnostics and tests).
    [[nodiscard]] std::size_t quads_drawn() const { return m_quads_drawn; }
    // 3D objects (command 0x01 / 0x41) processed by the last render() call.
    [[nodiscard]] std::size_t objects_drawn() const { return m_objects_drawn; }

    // Copies polygon ROM bytes (little-endian 32-bit words) to byte offset
    // `offset`; false if they do not fit.
    bool load_poly_rom(std::span<const uint8_t> bytes, std::size_t offset);

    // Data uploaded by commands 0x05 / 0x06, for the (not yet emulated)
    // 3D object renderer, tests and debugging. `index` is relative to
    // k_poly_ram_base / slot 0; out-of-range indexes read 0 / defaults.
    [[nodiscard]] uint32_t poly_ram_word(std::size_t index) const
    {
        return index < k_poly_ram_words ? m_poly_ram[index] : 0;
    }
    [[nodiscard]] LightParam light_param(std::size_t index) const
    {
        return index < k_light_param_count ? m_light_params[index] : LightParam{};
    }

private:
    struct Point {
        float x = 0, y = 0, z = 0; // as stored in the display list
        int   sx = 0, sy = 0;      // screen position
    };

    struct Quad {
        std::array<Point, 4> p;
        float    z = 0;            // sort key, larger = farther
        uint32_t color = 0;        // 0xAARRGGBB
        bool     stipple = false;
        std::size_t order = 0;     // submission order, for a stable sort
    };

    struct Viewport {
        float xc = 0, yc = 0;      // projection centre
        int   x1 = 0, x2 = 0;      // clip rectangle, inclusive
        int   y1 = 0, y2 = 0;
    };

    // Camera state for 3D objects, set by display-list commands and kept
    // between frames (as on the board); the object matrix is reset to
    // identity at the start of each list.
    struct ObjectView {
        float zoom_x = 0, zoom_y = 0;   // 0x09 (stored x4, as MAME)
        float view_x = 0, view_y = 0;   // 0x0C
        std::array<float, 12> matrix{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; // 0x0B: 3x3 + translation
        std::array<float, 3> light{0, 0, 0}; // 0x0A, normalized
        bool specular = false;               // 0x07 bit 0
        // Frustum slopes (x / z, y / z), from the viewport, zoom and view
        // translation: points beyond them are clipped.
        float a_left = 0, a_right = 0, a_bottom = 0, a_top = 0;
    };

    // Display list access (word offsets wrap inside the 64 KB list).
    [[nodiscard]] uint16_t word(std::size_t offset) const;
    [[nodiscard]] uint32_t long_at(std::size_t offset) const;
    [[nodiscard]] float    float_at(std::size_t offset) const;

    [[nodiscard]] bool use_list1();
    std::size_t parse_direct(std::size_t offset);
    // True if a command's `words` (header + payload) fit before the end of
    // the list; otherwise logs and the list ends (a garbage length would
    // otherwise run for billions of iterations).
    [[nodiscard]] bool fits_in_list(std::size_t offset, uint64_t words, uint32_t command) const;
    void project(Point& p) const;
    // 3D objects (MAME's push_object and its frustum clipper).
    void recompute_frustum();
    void transform_point(Point& p) const;
    void project_perspective(Point& p) const;
    void push_object(uint32_t color_address, uint32_t poly_address, uint32_t size);
    void clip_and_queue(int plane, const Quad& quad);
    [[nodiscard]] bool outside(int plane, const Point& p) const;
    [[nodiscard]] Point intersect(int plane, const Point& p1, const Point& p2) const;
    [[nodiscard]] uint32_t object_color(uint32_t color_address, float light_level, bool stipple);
    [[nodiscard]] uint32_t translate_color(uint16_t palette_color, uint32_t level) const;
    [[nodiscard]] uint32_t quad_color(uint32_t color_address, uint32_t luminance, bool stipple);
    void flush_quads();

    void draw_quad(const Quad& quad);
    void fill_triangle(const Point& a, const Point& b, const Point& c, uint32_t color, bool stipple);
    void draw_line(int x0, int y0, int x1, int y1, uint32_t color, bool stipple);
    void plot(int x, int y, uint32_t color, bool stipple);

    void log_once(unsigned command, const char* message);

    // Inputs of the frame being rendered.
    std::span<const uint8_t> m_list;
    std::span<const uint8_t> m_palette_ram;
    std::span<const uint8_t> m_color_xlat;
    std::span<uint32_t>      m_frame;

    std::array<uint16_t, 2> m_list_control{};
    uint64_t m_frame_number = 0;
    Viewport m_view;
    ObjectView m_object_view;
    float m_old_z = 0;            // z of the previous object polygon (z mode 0 reuses it)
    std::size_t m_objects_drawn = 0;
    // Polygon ROM (models) read by command 0x01 (addresses below 0x800000).
    std::vector<uint32_t> m_poly_rom = std::vector<uint32_t>(k_poly_rom_words, 0);

    // Colour table written by command 0x04 (addresses 0x40000 and up).
    std::vector<uint16_t> m_color_table = std::vector<uint16_t>(k_color_table_words, 0);
    // Polygon data RAM (command 0x05) and lighting parameters (command 0x06).
    std::vector<uint32_t> m_poly_ram = std::vector<uint32_t>(k_poly_ram_words, 0);
    std::array<LightParam, k_light_param_count> m_light_params{};

    std::vector<Quad> m_quads;
    std::size_t m_quads_drawn = 0;
    std::bitset<0x100> m_logged_commands;
};

} // namespace model1

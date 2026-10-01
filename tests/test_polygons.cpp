// 3D polygon tests: the display-list interpreter and flat quad rasterizer
// (PolygonRenderer), and the TGP-driven cube demo (culling, backdrop, spin).

#include "test_framework.hpp"

#include "core/motherboard.hpp"
#include "core/polygon_renderer.hpp"

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using model1::Motherboard;
using model1::PolygonRenderer;

constexpr uint32_t k_backdrop = 0xFF101010; // what the frame holds before polygons
constexpr uint32_t k_red = 0xFFFF0000;
constexpr uint32_t k_green = 0xFF00FF00;
constexpr uint32_t k_blue = 0xFF0000FF;

// A renderer with its own display list RAM, palette and colour translation,
// and helpers that write display-list commands.
struct PolyRig {
    std::vector<uint8_t> lists = std::vector<uint8_t>(PolygonRenderer::k_display_list_bytes, 0);
    std::vector<uint8_t> palette = std::vector<uint8_t>(PolygonRenderer::k_palette_ram_bytes, 0);
    std::vector<uint8_t> xlat = std::vector<uint8_t>(PolygonRenderer::k_color_xlat_bytes, 0);
    std::vector<uint32_t> frame = std::vector<uint32_t>(PolygonRenderer::k_pixel_count, k_backdrop);
    std::unique_ptr<PolygonRenderer> renderer = std::make_unique<PolygonRenderer>();
    std::size_t w = 0;         // next free word in the list being built
    std::size_t list_base = 0; // word offset of that list (0 or 0x8000)

    PolyRig()
    {
        renderer->write_list_control(2, 0x1F); // rendering enabled
        // Colour translation: channel * level / 63 for all three channels.
        for (uint32_t table : {0x0000u, 0x2000u, 0x4000u}) {
            for (uint32_t c = 0; c < 32; ++c) {
                for (uint32_t l = 0; l < 64; ++l) {
                    put(xlat, table | (c << 8) | l, static_cast<uint16_t>(((c * l) / 63) << 3));
                }
            }
        }
        put(palette, 0x1001, 0x801F); // red
        put(palette, 0x1002, 0x83E0); // green
        put(palette, 0x1003, 0xFC00); // blue
    }

    static void put(std::vector<uint8_t>& v, std::size_t word, uint16_t value)
    {
        v[word * 2] = static_cast<uint8_t>(value);
        v[word * 2 + 1] = static_cast<uint8_t>(value >> 8);
    }
    void p16(std::size_t i, uint16_t v) { put(lists, list_base + i, v); }
    void p32(std::size_t i, uint32_t v)
    {
        p16(i, static_cast<uint16_t>(v));
        p16(i + 1, static_cast<uint16_t>(v >> 16));
    }
    void pf(std::size_t i, float v) { p32(i, std::bit_cast<uint32_t>(v)); }

    void viewport(int xc, int yc, int x1, int y1, int x2, int y2)
    {
        p32(w, 3);
        p32(w + 2, 0);
        p16(w + 4, static_cast<uint16_t>(xc));
        p16(w + 6, static_cast<uint16_t>(422 - yc));
        p16(w + 8, static_cast<uint16_t>(x1));
        p16(w + 10, static_cast<uint16_t>(422 - y2));
        p16(w + 12, static_cast<uint16_t>(x2));
        p16(w + 14, static_cast<uint16_t>(422 - y1));
        w += 16;
    }
    // Colour table entries 0x40000-0x40002 -> palette 1-3 (red, green, blue).
    void colors()
    {
        p32(w, 4);
        p32(w + 2, 0x40000);
        p32(w + 4, 2);
        for (uint32_t i = 0; i < 3; ++i) {
            p32(w + 6 + 2 * i, 1 + i);
        }
        w += 12;
    }
    // Quad (A, B, C, D) in centre-relative coordinates (y up).
    void quad(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, uint32_t color,
              float z, uint32_t extra_flags = 0, uint32_t luminance = 126u << 24)
    {
        p32(w, 2);
        p32(w + 2, 0x40000 + color);
        pf(w + 6, bx);
        pf(w + 8, by);
        pf(w + 10, z);
        pf(w + 14, ax);
        pf(w + 16, ay);
        pf(w + 18, z);
        p32(w + 20, 0x0201 | extra_flags);
        p32(w + 22, luminance);
        pf(w + 26, cx);
        pf(w + 28, cy);
        pf(w + 30, z);
        pf(w + 32, z);
        pf(w + 34, dx);
        pf(w + 36, dy);
        pf(w + 38, z);
        p32(w + 40, 0);
        w += 42;
    }
    void end() { p32(w, 0x0F); w += 2; }
    void full_screen() { viewport(248, 192, 0, 0, 495, 383); colors(); }
    void render() { renderer->render(lists, palette, xlat, frame); }
    uint32_t px(int x, int y) const { return frame[static_cast<std::size_t>(y * PolygonRenderer::k_width + x)]; }
};

} // namespace

// ---------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------

TEST_CASE(polygon_rendering_requires_enable_mask)
{
    PolyRig rig;
    rig.full_screen();
    rig.quad(-10, 10, 10, 10, 10, -10, -10, -10, 0, 5);
    rig.end();
    rig.renderer->write_list_control(2, 0x0F);
    rig.render();
    CHECK_EQ(rig.renderer->quads_drawn(), 0u);
    CHECK_EQ(rig.px(248, 192), k_backdrop);
    rig.renderer->write_list_control(2, 0x1F);
    rig.render();
    CHECK_EQ(rig.renderer->quads_drawn(), 1u);
    CHECK_EQ(rig.px(248, 192), k_red);
}

TEST_CASE(polygon_fill_covers_exact_pixels)
{
    // Corners (-10, 10) .. (10, -10) around centre (248, 192), y up:
    // screen x 238-258, y 182-202, edges included.
    PolyRig rig;
    rig.full_screen();
    rig.quad(-10, 10, 10, 10, 10, -10, -10, -10, 0, 5);
    rig.end();
    rig.render();
    CHECK_EQ(rig.px(238, 182), k_red);
    CHECK_EQ(rig.px(258, 202), k_red);
    CHECK_EQ(rig.px(237, 192), k_backdrop);
    CHECK_EQ(rig.px(259, 192), k_backdrop);
    CHECK_EQ(rig.px(248, 181), k_backdrop);
    CHECK_EQ(rig.px(248, 203), k_backdrop);
}

TEST_CASE(polygon_painters_algorithm_far_to_near)
{
    PolyRig rig;
    rig.full_screen();
    rig.quad(-5, 5, 5, 5, 5, -5, -5, -5, 1, 2.0f);          // green, near, listed first
    rig.quad(-20, 20, 20, 20, 20, -20, -20, -20, 0, 9.0f);  // red, far
    rig.quad(30, 5, 40, 5, 40, -5, 30, -5, 0, 3.0f);        // red ...
    rig.quad(30, 5, 40, 5, 40, -5, 30, -5, 2, 3.0f);        // ... blue at the same depth, listed later
    rig.end();
    rig.render();
    CHECK_EQ(rig.px(248, 192), k_green);      // nearer quad drawn last
    CHECK_EQ(rig.px(248 + 15, 192), k_red);
    CHECK_EQ(rig.px(248 + 35, 192), k_blue);  // equal depth: list order
}

TEST_CASE(polygon_viewport_clipping)
{
    PolyRig rig;
    rig.viewport(248, 192, 240, 180, 250, 200);
    rig.colors();
    rig.quad(-10, -10, 10, -10, 10, 10, -10, 10, 0, 5); // either winding is filled
    rig.viewport(100, 100, 0, 0, 495, 383);           // later quads: new centre and clip
    rig.quad(-2, 2, 2, 2, 2, -2, -2, -2, 2, 5);
    rig.end();
    rig.render();
    CHECK_EQ(rig.px(245, 192), k_red);
    CHECK_EQ(rig.px(250, 200), k_red);
    CHECK_EQ(rig.px(251, 192), k_backdrop);
    CHECK_EQ(rig.px(239, 192), k_backdrop);
    CHECK_EQ(rig.px(100, 100), k_blue);
}

TEST_CASE(polygon_stipple_and_wireframe)
{
    PolyRig rig;
    rig.full_screen();
    rig.quad(-10, 10, 10, 10, 10, -10, -10, -10, 1, 5, 0x2000); // stippled ("moire")
    rig.quad(-60, 50, -60, 50, -20, 50, -20, 50, 2, 5);           // two distinct corners: a line
    rig.end();
    rig.render();
    CHECK_EQ(rig.px(248, 192), k_green); // (x ^ y) even: drawn
    CHECK_EQ(rig.px(249, 192), k_backdrop);
    CHECK_EQ(rig.px(249, 193), k_green);
    CHECK_EQ(rig.px(188, 142), k_blue);  // line from x 188 to 228 at y 142
    CHECK_EQ(rig.px(228, 142), k_blue);
    CHECK_EQ(rig.px(210, 143), k_backdrop);
}

TEST_CASE(polygon_color_luminance_and_table_advance)
{
    PolyRig rig;
    rig.full_screen();
    rig.quad(-5, 5, 5, 5, 5, -5, -5, -5, 0, 5, 0, 62u << 24); // level 31: red 31 * 31 / 63 = 15
    rig.quad(20, 5, 30, 5, 30, -5, 20, -5, 0, 5, 0x1000);     // advance flag: colour entry 1 (green)
    rig.end();
    rig.render();
    CHECK_EQ(rig.px(248, 192), 0xFF000000u | (((15u << 3) | (15u >> 2)) << 16));
    CHECK_EQ(rig.px(248 + 25, 192), k_green);
}

TEST_CASE(polygon_list_selection_and_double_buffering)
{
    PolyRig rig;
    rig.list_base = PolygonRenderer::k_list_words; // build the scene in list 1
    rig.full_screen();
    rig.quad(-5, 5, 5, 5, 5, -5, -5, -5, 2, 5);
    rig.end();
    rig.render(); // list 0 (empty) is current
    CHECK_EQ(rig.px(248, 192), k_backdrop);
    rig.renderer->write_list_control(0, 0x8); // manual mode, list 1
    CHECK_EQ(rig.renderer->read_list_control(0), 0x38u);
    rig.render();
    CHECK_EQ(rig.px(248, 192), k_blue);

    rig.renderer->write_list_control(0, 0x4); // automatic mode: swaps every second frame
    std::vector<bool> list1;
    for (int i = 0; i < 4; ++i) {
        list1.push_back((rig.renderer->read_list_control(0) & 0x40) != 0);
        rig.renderer->end_frame();
    }
    CHECK(list1[0] == list1[1] || list1[1] == list1[2]);
    CHECK(list1[0] != list1[2] || list1[1] != list1[3]);
}

TEST_CASE(polygon_malformed_lists_are_bounded)
{
    {
        PolyRig rig; // unknown command: logged, list ends
        rig.full_screen();
        rig.p32(rig.w, 0x77);
        rig.w += 2;
        rig.quad(-5, 5, 5, 5, 5, -5, -5, -5, 0, 5);
        rig.end();
        rig.render();
        CHECK_EQ(rig.renderer->quads_drawn(), 0u);
        CHECK(model1_test::captured_log().find("unknown display list command 0x00000077") != std::string::npos);
    }
    {
        PolyRig rig; // no end marker anywhere: stops after one pass over the list
        rig.render();
        CHECK(model1_test::captured_log().find("no end marker") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// TGP cube demo: culling, backdrop, spinning
// ---------------------------------------------------------------------------

TEST_CASE(polygon_demo_culls_back_faces)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    board->load_polygon_demo();
    // A cube seen in perspective shows 1 to 3 faces, never more. Check many
    // orientations: the faces drawn must match the faces sent (all of them
    // front-facing) and never exceed 3.
    for (int step = 0; step < 40; ++step) {
        const float angle = static_cast<float>(step) * 0.16f;
        const uint32_t sent = board->update_polygon_demo(angle, angle * 0.7f);
        board->run_frame();
        CHECK(sent >= 1 && sent <= 3);
        CHECK_EQ(board->polygons().quads_drawn(), static_cast<std::size_t>(sent));
    }
    // Looking straight at one face: exactly one face is visible.
    CHECK_EQ(board->update_polygon_demo(0.0f, 0.0f), 1u);
}

TEST_CASE(polygon_demo_draws_cube_over_blue_backdrop)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    board->load_polygon_demo();
    board->run_frame();
    const auto frame = board->frame();
    CHECK_EQ(frame[0], 0xFF0000FFu);                         // backdrop: palette entry 0, arcade blue
    CHECK_EQ(frame[383 * 496 + 495], 0xFF0000FFu);
    CHECK(frame[192 * 496 + 248] != 0xFF0000FFu);            // the cube covers the centre
    std::set<uint32_t> colours(frame.begin(), frame.end());
    CHECK(colours.size() >= 3);                              // backdrop + at least two shaded faces
}

TEST_CASE(polygon_demo_spins_between_frames)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    board->load_polygon_demo();
    board->update_polygon_demo(0.3f, 0.2f);
    board->run_frame();
    const std::vector<uint32_t> first(board->frame().begin(), board->frame().end());
    board->update_polygon_demo(0.9f, 0.6f);
    board->run_frame();
    const std::vector<uint32_t> second(board->frame().begin(), board->frame().end());
    CHECK(first != second);
}

// ---------------------------------------------------------------------------
// Commands 0x05 (polygon data RAM upload) and 0x06 (lighting parameters)
// ---------------------------------------------------------------------------

namespace {

bool poly_log_contains(const char* text)
{
    return model1_test::captured_log().find(text) != std::string::npos;
}

// Command header + payload: `cmd`, `address`, `count`, then `values`.
void upload(PolyRig& rig, uint32_t cmd, uint32_t address, uint32_t count, std::initializer_list<uint32_t> values)
{
    rig.p32(rig.w, cmd);
    rig.p32(rig.w + 2, address);
    rig.p32(rig.w + 4, count);
    std::size_t i = 0;
    for (uint32_t v : values) {
        rig.p32(rig.w + 6 + 2 * i, v);
        ++i;
    }
    rig.w += 6 + 2 * count;
}

} // namespace

TEST_CASE(polygon_command_05_uploads_polygon_ram_and_06_lighting)
{
    PolyRig rig;
    upload(rig, 0x05, PolygonRenderer::k_poly_ram_base + 0x10, 3, {0x11111111, 0x22222222, 0x33333333});
    upload(rig, 0x06, 5, 2, {0x10FF8040, 0x00000000});
    rig.colors(); // still parsed after the uploads: offsets advanced correctly
    rig.end();
    rig.render();
    const PolygonRenderer& r = *rig.renderer;
    CHECK_EQ(r.poly_ram_word(0x10), 0x11111111u);
    CHECK_EQ(r.poly_ram_word(0x12), 0x33333333u);
    CHECK_EQ(r.poly_ram_word(0x13), 0u);
    const PolygonRenderer::LightParam lp = r.light_param(5);
    CHECK_EQ(lp.diffuse, 0x40 / 255.0f);
    CHECK_EQ(lp.ambient, 0x80 / 255.0f);
    CHECK_EQ(lp.specular, 1.0f);
    CHECK_EQ(lp.power, 0x10u);
    CHECK(!poly_log_contains("WARNING"));

    rig.renderer->reset();
    CHECK_EQ(rig.renderer->poly_ram_word(0x10), 0u);
    CHECK_EQ(rig.renderer->light_param(5).power, 0u);
}

TEST_CASE(polygon_uploads_reject_out_of_range_targets)
{
    PolyRig rig;
    const uint32_t last = PolygonRenderer::k_poly_ram_base + static_cast<uint32_t>(PolygonRenderer::k_poly_ram_words) - 1;
    upload(rig, 0x05, last, 3, {0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC}); // only the first fits
    upload(rig, 0x05, PolygonRenderer::k_poly_ram_base - 1, 1, {0xDDDDDDDD}); // below the RAM
    upload(rig, 0x06, 255, 2, {0x01010101, 0x02020202});                // slot 256 does not exist
    rig.end();
    rig.render();
    CHECK_EQ(rig.renderer->poly_ram_word(PolygonRenderer::k_poly_ram_words - 1), 0xAAAAAAAAu);
    CHECK_EQ(rig.renderer->poly_ram_word(0), 0u);
    CHECK_EQ(rig.renderer->light_param(255).power, 1u);
    CHECK(poly_log_contains("polygon data write outside polygon RAM at 0x00C00000"));
    CHECK(poly_log_contains("polygon data write outside polygon RAM at 0x007FFFFF"));
    CHECK(poly_log_contains("lighting parameter slot 256 out of range"));
}

TEST_CASE(polygon_upload_with_garbage_length_ends_list_without_overflow)
{
    // A count of 0xFFFFFFFF (or anything past the end of the 64 KB list)
    // must neither loop for billions of iterations nor write anything.
    PolyRig rig;
    rig.p32(0, 0x05);
    rig.p32(2, PolygonRenderer::k_poly_ram_base);
    rig.p32(4, 0xFFFFFFFF);
    rig.p32(6, 0x12345678);
    rig.render();
    CHECK_EQ(rig.renderer->poly_ram_word(0), 0u);
    CHECK(poly_log_contains("command 0x05 at word 0x00000000 claims"));

    // Just too long: header at the last words of the list.
    PolyRig tail;
    const std::size_t at = PolygonRenderer::k_list_words - 8;
    tail.p32(0, 0x00); // no-ops up to `at`
    for (std::size_t i = 0; i < at; i += 2) {
        tail.p32(i, 0x00);
    }
    tail.p32(at, 0x06);
    tail.p32(at + 2, 0);
    tail.p32(at + 4, 2); // 6 + 4 words: 2 past the end
    tail.render();
    CHECK_EQ(tail.renderer->light_param(0).power, 0u);
    CHECK(poly_log_contains("command 0x06 at word 0x00007FF8 claims 10 words"));
}

TEST_CASE(polygon_erased_list_ends_quietly)
{
    PolyRig rig;
    rig.p32(0, 0xFFFFFFFF); // never-written list, as VR's boot leaves it
    rig.render();
    CHECK_EQ(rig.renderer->quads_drawn(), 0u);
    CHECK(!poly_log_contains("unknown display list command"));
}

// ---------------------------------------------------------------------------
// 3D objects (command 0x01): projection, backface culling, clipping, lighting
// ---------------------------------------------------------------------------

namespace {

struct ObjectScene {
    float light_x = 0, light_y = 0, light_z = -1;
    uint32_t light_param = 0x0000FF00; // ambient 1.0
    bool reversed = false;             // winding seen from the camera: back-facing
    uint32_t extra_flags = 0;          // e.g. 0x4000 double-sided
    int viewport_x1 = 0;
    bool from_rom = false;
};

// A 2x2 square at z = 10 (object space x, y = +/-1), zoom 100: it covers
// screen x 238..258, y 182..202 around the centre (248, 192).
void build_object_scene(PolyRig& rig, const ObjectScene& scene)
{
    rig.colors(); // colour table 0x40000 -> palette 0x1001 (red)
    upload(rig, 0x06, 0, 1, {scene.light_param});

    const float a = scene.reversed ? 1.0f : -1.0f;
    // Start edge A, B; one record with C, D: quad (B, A, C, D).
    const std::vector<float> points = {a, 1, 0, a, -1, 0};
    const uint32_t flags = 0x1 | 0x100 | 0x400 | scene.extra_flags; // two points, link 1, nearest z
    const std::vector<float> record = {0, 0, -1, -a, 1, 0, -a, -1, 0}; // normal, C, D
    std::vector<uint32_t> words;
    for (float v : points) words.push_back(std::bit_cast<uint32_t>(v));
    words.push_back(flags);
    for (float v : record) words.push_back(std::bit_cast<uint32_t>(v));
    words.push_back(0); // end
    if (scene.from_rom) {
        std::vector<uint8_t> bytes(words.size() * 4);
        for (std::size_t i = 0; i < words.size(); ++i) {
            for (int b = 0; b < 4; ++b) bytes[i * 4 + static_cast<std::size_t>(b)] = static_cast<uint8_t>(words[i] >> (8 * b));
        }
        rig.renderer->load_poly_rom(bytes, 0x100 * 4);
    } else {
        rig.p32(rig.w, 0x05);
        rig.p32(rig.w + 2, PolygonRenderer::k_poly_ram_base);
        rig.p32(rig.w + 4, static_cast<uint32_t>(words.size()));
        for (std::size_t i = 0; i < words.size(); ++i) rig.p32(rig.w + 6 + 2 * i, words[i]);
        rig.w += 6 + 2 * words.size();
    }

    rig.viewport(248, 192, scene.viewport_x1, 0, 495, 383);
    rig.p32(rig.w, 0x09); rig.pf(rig.w + 2, 25.0f); rig.pf(rig.w + 4, 25.0f); rig.w += 6; // zoom x4 = 100
    rig.p32(rig.w, 0x0A); rig.pf(rig.w + 2, scene.light_x); rig.pf(rig.w + 4, scene.light_y); rig.pf(rig.w + 6, scene.light_z); rig.w += 8;
    rig.p32(rig.w, 0x0B);
    const float matrix[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10}; // translate to z = 10
    for (int i = 0; i < 12; ++i) rig.pf(rig.w + 2 + 2 * static_cast<std::size_t>(i), matrix[i]);
    rig.w += 26;
    rig.p32(rig.w, 0x01);
    rig.p32(rig.w + 2, 0x40000);
    rig.p32(rig.w + 4, scene.from_rom ? 0x100 : PolygonRenderer::k_poly_ram_base);
    rig.p32(rig.w + 6, 0);
    rig.w += 8;
    rig.end();
    rig.render();
}

uint32_t pixel(const PolyRig& rig, int x, int y) { return rig.frame[static_cast<std::size_t>(y) * 496 + static_cast<std::size_t>(x)]; }

} // namespace

TEST_CASE(polygon_object_projects_and_fills)
{
    PolyRig rig;
    build_object_scene(rig, ObjectScene{});
    CHECK_EQ(rig.renderer->objects_drawn(), 1u);
    CHECK_EQ(rig.renderer->quads_drawn(), 1u);
    CHECK_EQ(pixel(rig, 248, 192), k_red);   // centre
    CHECK_EQ(pixel(rig, 240, 184), k_red);   // inside the corner
    CHECK_EQ(pixel(rig, 230, 192), k_backdrop); // left of x = 238
    CHECK_EQ(pixel(rig, 248, 175), k_backdrop); // above y = 182

    PolyRig rom;
    ObjectScene scene;
    scene.from_rom = true; // same object read from polygon ROM
    build_object_scene(rom, scene);
    CHECK_EQ(pixel(rom, 248, 192), k_red);
}

TEST_CASE(polygon_object_backface_culling)
{
    PolyRig back;
    ObjectScene scene;
    scene.reversed = true;
    build_object_scene(back, scene);
    CHECK_EQ(back.renderer->quads_drawn(), 0u);
    CHECK_EQ(pixel(back, 248, 192), k_backdrop);

    PolyRig both;
    scene.extra_flags = 0x4000; // double-sided
    build_object_scene(both, scene);
    CHECK_EQ(pixel(both, 248, 192), k_red);
}

TEST_CASE(polygon_object_clipped_to_viewport)
{
    PolyRig rig;
    ObjectScene scene;
    scene.viewport_x1 = 245; // left edge cuts the square (x 238..258)
    build_object_scene(rig, scene);
    CHECK_EQ(pixel(rig, 244, 192), k_backdrop);
    CHECK_EQ(pixel(rig, 247, 192), k_red);
    CHECK_EQ(pixel(rig, 256, 192), k_red);
}

TEST_CASE(polygon_object_diffuse_lighting)
{
    // Diffuse only (ambient 0): lit when the light points along the
    // normal, black when it is perpendicular.
    PolyRig lit;
    ObjectScene scene;
    scene.light_param = 0x000000FF; // diffuse 1.0
    build_object_scene(lit, scene);
    CHECK_EQ(pixel(lit, 248, 192), k_red);

    PolyRig dark;
    scene.light_x = 1;
    scene.light_z = 0;
    build_object_scene(dark, scene);
    CHECK_EQ(pixel(dark, 248, 192), 0xFF000000u);
}

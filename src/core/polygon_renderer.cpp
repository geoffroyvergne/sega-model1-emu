#include "core/polygon_renderer.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace model1 {

namespace {

// Command lengths in words, for commands with a fixed size.
constexpr std::size_t k_nop_words      = 2;
constexpr std::size_t k_object_words   = 8;
constexpr std::size_t k_viewport_words = 16;
constexpr std::size_t k_mode_words     = 4;
constexpr std::size_t k_zoom_words     = 6;
constexpr std::size_t k_light_words    = 8;
constexpr std::size_t k_matrix_words   = 26;
constexpr std::size_t k_trans_words    = 6;

// Viewport y values are stored as 383 - (y - 39) = 422 - y.
constexpr int k_viewport_y_flip = 422;

constexpr uint32_t k_flag_advance_color = 0x1000;
// Bounds against runaway data: polygons per object without an end marker,
// and quads queued per viewport.
constexpr uint64_t k_max_object_polygons = 0x100000;
constexpr std::size_t k_max_queued_quads = 1'000'000;
constexpr uint32_t k_flag_stipple       = 0x2000;

constexpr uint32_t expand5(uint32_t channel)
{
    return (channel << 3) | (channel >> 2);
}

uint16_t read_le16(std::span<const uint8_t> bytes, std::size_t word_index)
{
    const std::size_t i = word_index * 2;
    return static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
}

// Float -> screen coordinate, saturated so absurd values cannot overflow int.
int to_screen(float value)
{
    if (!(value > -32768.0f)) {
        return -32768;
    }
    if (!(value < 32767.0f)) {
        return 32767;
    }
    return static_cast<int>(value); // truncates toward zero, as MAME does
}

} // namespace

PolygonRenderer::PolygonRenderer()
{
    m_quads.reserve(4096);
}

void PolygonRenderer::reset()
{
    m_list_control = {};
    m_frame_number = 0;
    m_view = Viewport{};
    std::fill(m_color_table.begin(), m_color_table.end(), uint16_t{0});
    std::fill(m_poly_ram.begin(), m_poly_ram.end(), uint32_t{0});
    m_light_params.fill(LightParam{});
    m_object_view = ObjectView{};
    m_old_z = 0;
    m_objects_drawn = 0;
    m_quads.clear();
    m_quads_drawn = 0;
}

bool PolygonRenderer::load_poly_rom(std::span<const uint8_t> bytes, std::size_t offset)
{
    if ((offset & 3) != 0 || (bytes.size() & 3) != 0 || offset + bytes.size() > k_poly_rom_words * 4) {
        return false;
    }
    for (std::size_t i = 0; i < bytes.size() / 4; ++i) {
        m_poly_rom[offset / 4 + i] = static_cast<uint32_t>(bytes[i * 4]) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8)
                                   | (static_cast<uint32_t>(bytes[i * 4 + 2]) << 16)
                                   | (static_cast<uint32_t>(bytes[i * 4 + 3]) << 24);
    }
    return true;
}

// ---------------------------------------------------------------------------
// List control register and double buffering
// ---------------------------------------------------------------------------

uint16_t PolygonRenderer::read_list_control(uint32_t offset) const
{
    if (offset == 0) {
        return static_cast<uint16_t>(m_list_control[0] | 0x30);
    }
    return m_list_control[1];
}

void PolygonRenderer::write_list_control(uint32_t offset, uint16_t value)
{
    m_list_control[offset == 0 ? 0 : 1] = value;
}

bool PolygonRenderer::use_list1()
{
    // In manual mode (bit 2 clear), bit 3 selects the list and is mirrored
    // into bit 6.
    if ((m_list_control[0] & 0x4) == 0) {
        m_list_control[0] = static_cast<uint16_t>((m_list_control[0] & ~0x40) | ((m_list_control[0] & 0x8) != 0 ? 0x40 : 0));
    }
    return (m_list_control[0] & 0x40) != 0;
}

void PolygonRenderer::end_frame()
{
    if ((m_list_control[0] & 0x4) != 0 && (m_frame_number & 1) != 0) {
        m_list_control[0] ^= 0x40;
    }
    ++m_frame_number;
}

// ---------------------------------------------------------------------------
// Display list access
// ---------------------------------------------------------------------------

uint16_t PolygonRenderer::word(std::size_t offset) const
{
    return read_le16(m_list, offset & (k_list_words - 1));
}

uint32_t PolygonRenderer::long_at(std::size_t offset) const
{
    return word(offset) | (static_cast<uint32_t>(word(offset + 1)) << 16);
}

float PolygonRenderer::float_at(std::size_t offset) const
{
    return std::bit_cast<float>(long_at(offset));
}

bool PolygonRenderer::fits_in_list(std::size_t offset, uint64_t words, uint32_t command) const
{
    if (offset + words <= k_list_words) {
        return true;
    }
    std::cerr << "[Polygon] WARNING: command " << Hex{command, 2} << " at word " << Hex{static_cast<uint32_t>(offset)}
              << " claims " << words << " words, past the end of the list; list ended\n";
    return false;
}

void PolygonRenderer::log_once(unsigned command, const char* message)
{
    if (!m_logged_commands.test(command & 0xFF)) {
        m_logged_commands.set(command & 0xFF);
        std::cerr << "[Polygon] " << message << '\n';
    }
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

void PolygonRenderer::render(std::span<const uint8_t> display_lists, std::span<const uint8_t> palette_ram,
                             std::span<const uint8_t> color_xlat, std::span<uint32_t> frame)
{
    m_quads_drawn = 0;
    m_objects_drawn = 0;
    if ((m_list_control[1] & 0x1F) != 0x1F) {
        return; // rendering disabled
    }
    if (display_lists.size() < k_display_list_bytes || palette_ram.size() < k_palette_ram_bytes
        || color_xlat.size() < k_color_xlat_bytes || frame.size() < k_pixel_count) {
        std::cerr << "[Polygon] ERROR: memory or frame views too small, polygons not rendered\n";
        return;
    }

    const std::size_t list_bytes = k_list_words * 2;
    m_list = display_lists.subspan(use_list1() ? list_bytes : 0, list_bytes);
    m_palette_ram = palette_ram;
    m_color_xlat = color_xlat;
    m_frame = frame;
    m_quads.clear();
    // Each list starts with an identity object matrix (as in MAME); zoom,
    // light and view translation carry over from previous frames.
    m_object_view.matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    m_old_z = 0;

    std::size_t offset = 0;
    bool done = false;
    while (!done) {
        // A list without an end marker would loop forever (offsets wrap).
        if (offset >= k_list_words) {
            log_once(0xFE, "WARNING: display list has no end marker within 64 KB, stopping");
            break;
        }
        const uint32_t command = long_at(offset);
        switch (command) {
        case 0x00:
            offset += k_nop_words;
            break;
        case 0x01:
        case 0x41: // 0x41 is drawn above the HUD on the board; here in the same pass as 0x01
            push_object(long_at(offset + 2), long_at(offset + 4), long_at(offset + 6));
            offset += k_object_words;
            break;
        case 0x02:
            offset = parse_direct(offset);
            break;
        case 0x03: {
            // Pending quads are drawn with the viewport they were queued under.
            flush_quads();
            m_view.xc = static_cast<int16_t>(word(offset + 4));
            m_view.yc = static_cast<float>(k_viewport_y_flip - static_cast<int16_t>(word(offset + 6)));
            m_view.x1 = static_cast<int16_t>(word(offset + 8));
            m_view.y2 = k_viewport_y_flip - static_cast<int16_t>(word(offset + 10));
            m_view.x2 = static_cast<int16_t>(word(offset + 12));
            m_view.y1 = k_viewport_y_flip - static_cast<int16_t>(word(offset + 14));
            recompute_frustum();
            offset += k_viewport_words;
            break;
        }
        case 0x04: {
            const uint32_t address = long_at(offset + 2);
            const uint64_t count = uint64_t{long_at(offset + 4)} + 1;
            if (!fits_in_list(offset, 6 + 2 * count, command)) {
                done = true;
                break;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t target = address + i;
                if (target < k_color_table_base || target - k_color_table_base >= k_color_table_words) {
                    std::cerr << "[Polygon] WARNING: colour table write outside table at " << Hex{target} << ", ignored\n";
                    break;
                }
                m_color_table[target - k_color_table_base] = word(offset + 6 + 2 * static_cast<std::size_t>(i));
            }
            offset += 6 + 2 * static_cast<std::size_t>(count);
            break;
        }
        case 0x05: {
            // Upload to polygon data RAM: address, count, then `count` longs.
            const uint32_t address = long_at(offset + 2);
            const uint32_t count = long_at(offset + 4);
            if (!fits_in_list(offset, 6 + 2 * uint64_t{count}, command)) {
                done = true;
                break;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const uint64_t index = uint64_t{address} + i - k_poly_ram_base;
                if (address < k_poly_ram_base || index >= k_poly_ram_words) {
                    std::cerr << "[Polygon] WARNING: polygon data write outside polygon RAM at "
                              << Hex{static_cast<uint32_t>(address + i)} << ", rest of the upload ignored\n";
                    break;
                }
                m_poly_ram[static_cast<std::size_t>(index)] = long_at(offset + 6 + 2 * static_cast<std::size_t>(i));
            }
            offset += 6 + 2 * static_cast<std::size_t>(count);
            break;
        }
        case 0x06: {
            // Upload lighting parameters: first slot, count, then `count` longs.
            const uint32_t first = long_at(offset + 2);
            const uint32_t count = long_at(offset + 4);
            if (!fits_in_list(offset, 6 + 2 * uint64_t{count}, command)) {
                done = true;
                break;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const uint64_t slot = uint64_t{first} + i;
                if (slot >= k_light_param_count) {
                    std::cerr << "[Polygon] WARNING: lighting parameter slot " << slot
                              << " out of range (0-255), rest of the upload ignored\n";
                    break;
                }
                const uint32_t v = long_at(offset + 6 + 2 * static_cast<std::size_t>(i));
                LightParam& lp = m_light_params[static_cast<std::size_t>(slot)];
                lp.diffuse = static_cast<float>(v & 0xFF) / 255.0f;
                lp.ambient = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
                lp.specular = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
                lp.power = static_cast<uint8_t>(v >> 24);
            }
            offset += 6 + 2 * static_cast<std::size_t>(count);
            break;
        }
        case 0x07: // mode word: bit 0 enables the specular term
            m_object_view.specular = (long_at(offset + 2) & 1) != 0;
            offset += k_mode_words;
            break;
        case 0x08: // select mode (no effect known)
            offset += k_mode_words;
            break;
        case 0x09: // zoom (stored x4, as MAME)
            m_object_view.zoom_x = float_at(offset + 2) * 4;
            m_object_view.zoom_y = float_at(offset + 4) * 4;
            recompute_frustum();
            offset += k_zoom_words;
            break;
        case 0x0A: { // light direction (normalized)
            const float x = float_at(offset + 2);
            const float y = float_at(offset + 4);
            const float z = float_at(offset + 6);
            const float length = std::sqrt(x * x + y * y + z * z);
            m_object_view.light = length > 0 ? std::array<float, 3>{x / length, y / length, z / length}
                                             : std::array<float, 3>{0, 0, 0};
            offset += k_light_words;
            break;
        }
        case 0x0B: // object matrix: 3x3 rotation (column-major) then translation
            for (std::size_t i = 0; i < 12; ++i) {
                m_object_view.matrix[i] = float_at(offset + 2 + 2 * i);
            }
            offset += k_matrix_words;
            break;
        case 0x0C: // view translation
            m_object_view.view_x = float_at(offset + 2);
            m_object_view.view_y = float_at(offset + 4);
            recompute_frustum();
            offset += k_trans_words;
            break;
        case 0x0F:
        case 0xFFFFFFFF: // erased / never-written list: MAME ends here too (its "unknown type")
            done = true;
            break;
        default:
            std::cerr << "[Polygon] WARNING: unknown display list command " << Hex{command}
                      << " at word " << Hex{static_cast<uint32_t>(offset)} << ", list ended\n";
            done = true;
            break;
        }
    }
    flush_quads();

    m_list = {};
    m_palette_ram = {};
    m_color_xlat = {};
    m_frame = {};
}

void PolygonRenderer::project(Point& p) const
{
    // Direct polygons are already projected: only the centre offset applies.
    p.sx = to_screen(m_view.xc + p.x);
    p.sy = to_screen(m_view.yc - p.y);
}

std::size_t PolygonRenderer::parse_direct(std::size_t offset)
{
    uint32_t color_address = long_at(offset + 2);
    Point first{float_at(offset + 6), float_at(offset + 8), float_at(offset + 10)};
    Point second{float_at(offset + 14), float_at(offset + 16), float_at(offset + 18)};
    project(first);
    project(second);
    offset += 18;

    for (;;) {
        if (offset >= k_list_words) {
            log_once(0xFD, "WARNING: direct polygon strip runs past the end of the display list");
            return offset;
        }
        const uint32_t flags = long_at(offset + 2);
        const uint32_t type = flags & 3;
        if (type == 0) {
            break;
        }
        if ((flags & k_flag_advance_color) != 0) {
            ++color_address;
        }
        const uint32_t luminance = long_at(offset + 4);

        Point p0{float_at(offset + 8), float_at(offset + 10), float_at(offset + 12)};
        Point p1 = p0;
        float sort_z = p0.z;
        if (type == 2) {
            offset += 12;
        } else {
            sort_z = float_at(offset + 14);
            p1 = Point{float_at(offset + 16), float_at(offset + 18), float_at(offset + 20)};
            offset += 20;
        }
        project(p0);
        project(p1);

        const uint32_t link = (flags >> 8) & 3;
        if (link != 0) {
            Quad quad;
            quad.p = {second, first, p0, p1};
            quad.z = sort_z;
            quad.stipple = (flags & k_flag_stipple) != 0;
            quad.color = quad_color(color_address, luminance, quad.stipple);
            quad.order = m_quads.size();
            m_quads.push_back(quad);
        }

        // How the next quad attaches to this one.
        switch (link) {
        case 0:
        case 2:
            first = p0;
            second = p1;
            break;
        case 1:
            second = p0;
            break;
        default:
            first = p1;
            break;
        }
    }
    return offset + 4;
}

// ---------------------------------------------------------------------------
// 3D objects from polygon ROM / RAM (command 0x01), as MAME's push_object
// ---------------------------------------------------------------------------

void PolygonRenderer::recompute_frustum()
{
    ObjectView& v = m_object_view;
    v.a_left = (static_cast<float>(m_view.x1) - m_view.xc - v.view_x) / v.zoom_x;
    v.a_right = (static_cast<float>(m_view.x2) - m_view.xc - v.view_x) / v.zoom_x;
    v.a_bottom = (-static_cast<float>(m_view.y1) + m_view.yc - v.view_y) / v.zoom_y;
    v.a_top = (-static_cast<float>(m_view.y2) + m_view.yc - v.view_y) / v.zoom_y;
}

void PolygonRenderer::transform_point(Point& p) const
{
    const std::array<float, 12>& m = m_object_view.matrix;
    const Point q = p;
    p.x = m[0] * q.x + m[3] * q.y + m[6] * q.z + m[9];
    p.y = m[1] * q.x + m[4] * q.y + m[7] * q.z + m[10];
    p.z = m[2] * q.x + m[5] * q.y + m[8] * q.z + m[11];
}

void PolygonRenderer::project_perspective(Point& p) const
{
    p.sx = to_screen(m_view.xc + (p.x / p.z * m_object_view.zoom_x + m_object_view.view_x));
    p.sy = to_screen(m_view.yc - (p.y / p.z * m_object_view.zoom_y + m_object_view.view_y));
}

// Object data: a start edge (two points), then 10-word polygon records until
// one with type 0 (or `size` records; 0 = no limit):
//   +0 flags: bits 1-0 type (0 end, 2 triangle: one new point, else two),
//      bits 9-8 link (how the next record attaches; 0 = no polygon here),
//      bits 11-10 sort-z mode (0 previous, 1 nearest, 2 farthest, 3 front),
//      bit 12 advance colour address, bit 13 stipple, bit 14 double-sided,
//      bits 20-17 (+ bit 22 -> +0x80) lighting parameter slot
//   +1..+3 face normal, +4..+6 new point, +7..+9 second new point
// Addresses with bit 23 set read polygon RAM (command 0x05), else ROM.
void PolygonRenderer::push_object(uint32_t color_address, uint32_t poly_address, uint32_t size)
{
    if (color_address == 0xFFFFFFFFu || size >= 0x1000000) {
        return; // bad data (as MAME guards)
    }
    ++m_objects_drawn;
    const bool from_ram = (poly_address & 0x800000) != 0;
    uint32_t address = poly_address & 0x7FFFFF;
    auto word_at = [this, from_ram](uint32_t a) -> uint32_t {
        if (from_ram) {
            return a < k_poly_ram_words ? m_poly_ram[a] : 0;
        }
        return a < k_poly_rom_words ? m_poly_rom[a] : 0;
    };
    auto float_word = [&word_at](uint32_t a) { return std::bit_cast<float>(word_at(a)); };
    auto load_point = [&](uint32_t a) {
        Point p{float_word(a), float_word(a + 1), float_word(a + 2)};
        transform_point(p);
        if (p.z > 0) {
            project_perspective(p);
        } else {
            p.sx = p.sy = 0;
        }
        return p;
    };

    Point old_p0 = load_point(address);
    Point old_p1 = load_point(address + 3);
    address += 6;

    const std::array<float, 12>& m = m_object_view.matrix;
    const uint64_t limit = size == 0 ? k_max_object_polygons : size;
    for (uint64_t i = 0; i < limit; ++i) {
        const uint32_t flags = word_at(address);
        const uint32_t type = flags & 3;
        if (type == 0) {
            break;
        }
        if ((flags & k_flag_advance_color) != 0) {
            ++color_address;
        }
        const uint32_t light_slot = ((flags >> 17) & 15) | ((flags & 0x00400000) != 0 ? 0x80 : 0);

        const float nx = float_word(address + 1);
        const float ny = float_word(address + 2);
        const float nz = float_word(address + 3);
        Point p0 = load_point(address + 4);
        Point p1 = type == 2 ? p0 : load_point(address + 7);
        const uint32_t link = (flags >> 8) & 3;

        // Back-facing (unless double-sided): the triangle (old_p1, old_p0,
        // p0) seen from the origin turns the wrong way.
        const auto determinant = [](const Point& a, const Point& b, const Point& c) {
            const float x1 = b.x - a.x, y1 = b.y - a.y, z1 = b.z - a.z;
            const float x2 = c.x - a.x, y2 = c.y - a.y, z2 = c.z - a.z;
            return a.x * (y1 * z2 - y2 * z1) + a.y * (z1 * x2 - z2 * x1) + a.z * (x1 * y2 - x2 * y1);
        };
        if (link != 0 && ((flags & 0x00004000) != 0 || determinant(old_p1, old_p0, p0) <= 0)) {
            Quad quad;
            quad.p = {old_p1, old_p0, p0, p1};
            switch ((flags >> 10) & 3) {
            case 0: quad.z = m_old_z; break;
            case 1: quad.z = m_old_z = std::min({old_p1.z, old_p0.z, p0.z, p1.z}); break;
            case 2: quad.z = m_old_z = std::max({old_p1.z, old_p0.z, p0.z, p1.z}); break;
            default: quad.z = 0; break;
            }

            // Lighting: rotated, normalized face normal against the light.
            float vx = m[0] * nx + m[3] * ny + m[6] * nz;
            float vy = m[1] * nx + m[4] * ny + m[7] * nz;
            float vz = m[2] * nx + m[5] * ny + m[8] * nz;
            const float length = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (length > 0) {
                vx /= length;
                vy /= length;
                vz /= length;
            }
            const std::array<float, 3>& l = m_object_view.light;
            const float diffuse = vx * l[0] + vy * l[1] + vz * l[2];
            const LightParam& lp = m_light_params[light_slot];
            float specular = 0;
            if (m_object_view.specular && lp.power != 0 && lp.specular > 0) {
                // z of the reflected light vector, raised to a power of two.
                float sr = 2.0f * diffuse * vz - l[2];
                if (sr > 0) {
                    if (lp.power >= 2) sr *= sr;
                    if (lp.power >= 4) sr *= sr;
                    if (lp.power >= 7) sr *= sr;
                    specular = std::min(sr * lp.specular, 1.0f);
                }
            }
            const float level = lp.ambient + lp.diffuse * std::max(0.0f, diffuse) + specular;
            quad.stipple = (flags & k_flag_stipple) != 0;
            quad.color = object_color(color_address, level, quad.stipple);
            clip_and_queue(0, quad);
        }

        address += 10;
        switch (link) {
        case 0:
        case 2:
            old_p0 = p0;
            old_p1 = p1;
            break;
        case 1:
            old_p1 = p0;
            break;
        default:
            old_p0 = p1;
            break;
        }
    }
}

// Frustum planes: 0 bottom, 1 top, 2 left, 3 right (x / z or y / z
// against the slopes of the viewport edges).
bool PolygonRenderer::outside(int plane, const Point& p) const
{
    const ObjectView& v = m_object_view;
    switch (plane) {
    case 0: return p.y > p.z * v.a_bottom;
    case 1: return p.y < p.z * v.a_top;
    case 2: return p.x < p.z * v.a_left;
    default: return p.x > p.z * v.a_right;
    }
}

PolygonRenderer::Point PolygonRenderer::intersect(int plane, const Point& p1, const Point& p2) const
{
    const ObjectView& v = m_object_view;
    float t = 0;
    switch (plane) {
    case 0: t = (p2.z * v.a_bottom - p2.y) / ((p2.z - p1.z) * v.a_bottom - (p2.y - p1.y)); break;
    case 1: t = (p2.z * v.a_top - p2.y) / ((p2.z - p1.z) * v.a_top - (p2.y - p1.y)); break;
    case 2: t = (p2.z * v.a_left - p2.x) / ((p2.z - p1.z) * v.a_left - (p2.x - p1.x)); break;
    default: t = (p2.z * v.a_right - p2.x) / ((p2.z - p1.z) * v.a_right - (p2.x - p1.x)); break;
    }
    Point p{p1.x * t + p2.x * (1 - t), p1.y * t + p2.y * (1 - t), p1.z * t + p2.z * (1 - t)};
    project_perspective(p);
    return p;
}

// Clips `quad` against planes `plane` .. 3 in turn and queues what is left
// (MAME's fclip_push_quad: the cases of 1, 2 or 3 corners outside).
void PolygonRenderer::clip_and_queue(int plane, const Quad& quad)
{
    if (plane == 4) {
        if (m_quads.size() >= k_max_queued_quads) {
            log_once(0xFB, "WARNING: too many polygons in one viewport, the rest are dropped");
            return;
        }
        Quad queued = quad;
        queued.order = m_quads.size();
        m_quads.push_back(queued);
        return;
    }
    std::array<bool, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = outside(plane, quad.p[i]);
    }
    if (!out[0] && !out[1] && !out[2] && !out[3]) {
        clip_and_queue(plane + 1, quad);
        return;
    }
    if (out[0] && out[1] && out[2] && out[3]) {
        return;
    }
    // Rotate so that corner 0 is outside and corner 3 inside.
    std::size_t first = 0;
    while (!(out[first] && !out[(first + 3) & 3])) {
        ++first;
    }
    std::array<Point, 4> pt{};
    std::array<bool, 4> o{};
    for (std::size_t j = 0; j < 4; ++j) {
        pt[j] = quad.p[(first + j) & 3];
        o[j] = out[(first + j) & 3];
    }
    auto next = [&](const Point& a, const Point& b, const Point& c, const Point& d) {
        Quad q = quad;
        q.p = {a, b, c, d};
        clip_and_queue(plane + 1, q);
    };
    if (o[1]) {
        if (o[2]) { // corners 0, 1, 2 out: one triangle left
            const Point i1 = intersect(plane, pt[2], pt[3]);
            const Point i2 = intersect(plane, pt[3], pt[0]);
            next(i1, pt[3], i2, i2);
        } else {    // corners 0, 1 out: one quad left
            const Point i1 = intersect(plane, pt[1], pt[2]);
            const Point i2 = intersect(plane, pt[3], pt[0]);
            next(i1, pt[2], pt[3], i2);
        }
    } else if (o[2]) { // corners 0, 2 out (should not happen): two triangles
        const Point i1 = intersect(plane, pt[0], pt[1]);
        const Point i2 = intersect(plane, pt[1], pt[2]);
        next(i1, pt[1], i2, i2);
        const Point i3 = intersect(plane, pt[2], pt[3]);
        const Point i4 = intersect(plane, pt[3], pt[0]);
        next(i3, pt[3], i4, i4);
    } else { // corner 0 out: a pentagon, as a quad and a triangle
        const Point i1 = intersect(plane, pt[0], pt[1]);
        const Point i2 = intersect(plane, pt[3], pt[0]);
        next(i1, pt[1], pt[2], pt[3]);
        next(pt[3], i2, i1, i1);
    }
}

// Object polygon colour: colour table entry -> palette entry 0x1000 + index,
// shaded by the light level through the colour translation RAM. Entry bits
// 11-10: bit 10 = unlit UI element (full level); mode 01 = blinking (the
// channels rotate on odd frames), as in MAME.
uint32_t PolygonRenderer::object_color(uint32_t color_address, float light_level, bool)
{
    if (color_address < k_color_table_base || color_address - k_color_table_base >= k_color_table_words) {
        log_once(0xFA, "WARNING: object colour address outside the colour table, drawn black");
        return 0xFF000000u;
    }
    const uint16_t entry = m_color_table[color_address - k_color_table_base];
    uint16_t color = read_le16(m_palette_ram, 0x1000u | (entry & 0x3FF));
    if (((entry >> 10) & 3) == 1 && (m_frame_number & 1) != 0) {
        const uint16_t r = color & 0x1F, g = (color >> 5) & 0x1F, b = (color >> 10) & 0x1F;
        color = static_cast<uint16_t>((color & 0x8000) | g | (b << 5) | (r << 10)); // b -> g -> r -> b
    }
    int level = static_cast<int>(255.0f * std::min(1.0f, light_level)) >> 2;
    level = std::clamp(level, 0, 0x3F);
    if ((entry & 0x400) != 0) {
        level = 0x3F;
    }
    return translate_color(color, static_cast<uint32_t>(level));
}

uint32_t PolygonRenderer::translate_color(uint16_t palette_color, uint32_t level) const
{
    auto translate = [this, level](uint32_t channel, uint32_t table) {
        return (read_le16(m_color_xlat, table | (channel << 8) | level) >> 3) & 0x1Fu;
    };
    const uint32_t r = translate(palette_color & 0x1Fu, 0x0000);
    const uint32_t g = translate((palette_color >> 5) & 0x1Fu, 0x2000);
    const uint32_t b = translate((palette_color >> 10) & 0x1Fu, 0x4000);
    return 0xFF000000u | (expand5(r) << 16) | (expand5(g) << 8) | expand5(b);
}

uint32_t PolygonRenderer::quad_color(uint32_t color_address, uint32_t luminance, bool stipple)
{
    if (color_address < k_color_table_base || color_address - k_color_table_base >= k_color_table_words) {
        log_once(0xFC, "WARNING: direct polygon colour address outside the colour table, drawn black");
        return 0xFF000000u;
    }
    const uint16_t index = m_color_table[color_address - k_color_table_base] & 0x3FF;

    // A stippled polygon using colour index 0 is a "fade to backdrop"
    // overlay: it paints the backdrop colour (palette entry 0), as in MAME.
    if (stipple && index == 0) {
        const uint16_t backdrop = read_le16(m_palette_ram, 0);
        return 0xFF000000u | (expand5(backdrop & 0x1F) << 16) | (expand5((backdrop >> 5) & 0x1F) << 8)
             | expand5((backdrop >> 10) & 0x1F);
    }

    const uint16_t color = read_le16(m_palette_ram, 0x1000u | index);
    // Luminance byte -> level 0-63 (MAME: (lum >> 24) * 2 >> 2, clamped).
    return translate_color(color, std::min<uint32_t>((luminance >> 24) >> 1, 0x3F));
}

// Painter's algorithm: draw far quads first (larger z = farther); quads at
// the same depth keep their display-list order.
void PolygonRenderer::flush_quads()
{
    std::stable_sort(m_quads.begin(), m_quads.end(), [](const Quad& a, const Quad& b) { return a.z > b.z; });
    for (const Quad& quad : m_quads) {
        draw_quad(quad);
    }
    m_quads_drawn += m_quads.size();
    m_quads.clear();
}

// ---------------------------------------------------------------------------
// Rasterization
// ---------------------------------------------------------------------------

void PolygonRenderer::plot(int x, int y, uint32_t color, bool stipple)
{
    if (x < m_view.x1 || x > m_view.x2 || y < m_view.y1 || y > m_view.y2
        || x < 0 || x >= k_width || y < 0 || y >= k_height) {
        return;
    }
    if (stipple && ((x ^ y) & 1) != 0) {
        return;
    }
    m_frame[static_cast<std::size_t>(y) * k_width + static_cast<std::size_t>(x)] = color;
}

void PolygonRenderer::draw_quad(const Quad& quad)
{
    // Count distinct corners: 1 = point, 2 = line (wireframe), else fill.
    std::array<const Point*, 4> distinct{};
    std::size_t count = 0;
    for (const Point& p : quad.p) {
        const bool seen = std::any_of(distinct.begin(), distinct.begin() + static_cast<std::ptrdiff_t>(count),
            [&p](const Point* q) { return q->sx == p.sx && q->sy == p.sy; });
        if (!seen) {
            distinct[count++] = &p;
        }
    }
    if (count == 1) {
        // MAME skips degenerate stippled records (a stray dot otherwise).
        if (!quad.stipple) {
            plot(quad.p[0].sx, quad.p[0].sy, quad.color, false);
        }
        return;
    }
    if (count == 2) {
        draw_line(distinct[0]->sx, distinct[0]->sy, distinct[1]->sx, distinct[1]->sy, quad.color, quad.stipple);
        return;
    }
    fill_triangle(quad.p[0], quad.p[1], quad.p[2], quad.color, quad.stipple);
    fill_triangle(quad.p[0], quad.p[2], quad.p[3], quad.color, quad.stipple);
}

// Edge-function fill over the triangle's bounding box (clipped to the
// viewport). Pixels on edges are included; shared edges may be drawn twice,
// which is harmless because a quad's colour and stipple pattern depend only
// on the pixel position. Both windings are filled.
void PolygonRenderer::fill_triangle(const Point& a, const Point& b, const Point& c, uint32_t color, bool stipple)
{
    const int64_t area = static_cast<int64_t>(b.sx - a.sx) * (c.sy - a.sy)
                       - static_cast<int64_t>(b.sy - a.sy) * (c.sx - a.sx);
    if (area == 0) {
        return; // degenerate (the other half of the quad may still be visible)
    }
    const Point* v0 = &a;
    const Point* v1 = area > 0 ? &b : &c;
    const Point* v2 = area > 0 ? &c : &b;

    const int min_x = std::max({std::min({a.sx, b.sx, c.sx}), m_view.x1, 0});
    const int max_x = std::min({std::max({a.sx, b.sx, c.sx}), m_view.x2, k_width - 1});
    const int min_y = std::max({std::min({a.sy, b.sy, c.sy}), m_view.y1, 0});
    const int max_y = std::min({std::max({a.sy, b.sy, c.sy}), m_view.y2, k_height - 1});

    auto edge = [](const Point* p, const Point* q, int x, int y) {
        return static_cast<int64_t>(q->sx - p->sx) * (y - p->sy) - static_cast<int64_t>(q->sy - p->sy) * (x - p->sx);
    };
    for (int y = min_y; y <= max_y; ++y) {
        uint32_t* row = &m_frame[static_cast<std::size_t>(y) * k_width];
        for (int x = min_x; x <= max_x; ++x) {
            if (edge(v0, v1, x, y) >= 0 && edge(v1, v2, x, y) >= 0 && edge(v2, v0, x, y) >= 0) {
                if (!stipple || ((x ^ y) & 1) == 0) {
                    row[x] = color;
                }
            }
        }
    }
}

// Bresenham line, clipped per pixel to the viewport.
void PolygonRenderer::draw_line(int x0, int y0, int x1, int y1, uint32_t color, bool stipple)
{
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int step_x = x0 < x1 ? 1 : -1;
    const int step_y = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        plot(x0, y0, color, stipple);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int doubled = 2 * error;
        if (doubled >= dy) {
            error += dy;
            x0 += step_x;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += step_y;
        }
    }
}

} // namespace model1

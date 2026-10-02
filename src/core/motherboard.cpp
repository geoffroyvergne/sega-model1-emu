#include "core/motherboard.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace model1 {

Motherboard::Motherboard()
    : m_bus(std::make_unique<Bus>())
    , m_tgp(std::make_unique<Tgp>())
    , m_tgp_copro(std::make_unique<TgpCopro>())
    , m_io_shared_ram(std::make_unique<DualPortRam>())
    , m_inputs(std::make_unique<InputManager>(*m_io_shared_ram))
    , m_io_board(std::make_unique<IoBoard>(*m_io_shared_ram, *m_inputs))
    , m_tilemaps(std::make_unique<TilemapRenderer>())
    , m_polygons(std::make_unique<PolygonRenderer>())
    , m_sound_uart(std::make_unique<I8251>("UART main"))
    , m_sound(std::make_unique<SoundBoard>())
    , m_cpu(std::make_unique<V60>(*m_bus))
{
    // TGP host ports. Each register block is mirrored across its 128 KB
    // window, so only the low address bits select the register half. With
    // its ROMs loaded the real coprocessor (TgpCopro) answers; otherwise
    // the high-level Tgp stands in.
    m_bus->map_io("TGP shared RAM address", k_tgp_ram_address_port, k_tgp_port_span, 0x1,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_ram_address(offset) : m_tgp->read_ram_address(offset); },
        [this](uint32_t offset, uint16_t value) {
            if (m_tgp_copro->is_active()) {
                m_tgp_copro->write_ram_address(offset, value);
            } else {
                m_tgp->write_ram_address(offset, value);
            }
        });
    m_bus->map_io("TGP shared RAM data", k_tgp_ram_data_port, k_tgp_port_span, 0x3,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_ram_data(offset) : m_tgp->read_ram_data(offset); },
        [this](uint32_t offset, uint16_t value) {
            if (m_tgp_copro->is_active()) {
                m_tgp_copro->write_ram_data(offset, value);
            } else {
                m_tgp->write_ram_data(offset, value);
            }
        });
    m_bus->map_io("TGP FIFO", k_tgp_fifo_port, k_tgp_port_span, 0x3,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_fifo(offset) : m_tgp->read_fifo(offset); },
        [this](uint32_t offset, uint16_t value) {
            if (m_tgp_copro->is_active()) {
                m_tgp_copro->write_fifo(offset, value);
            } else {
                m_tgp->write_fifo(offset, value);
            }
        });
    // I/O board shared RAM (MB8421, 2 KB): byte n on the V60's low byte lane
    // at 0xC00000 + 2n; the high lane is not connected (reads 0, writes
    // dropped). The I/O board side is driven by InputManager for now.
    DualPortRam& shared_ram = *m_io_shared_ram;
    auto shared_ram_write_byte = [&shared_ram](uint32_t offset, uint8_t value) {
        if ((offset & 1) == 0) {
            shared_ram.main_write(offset >> 1, value);
        }
    };
    m_bus->map_io("I/O board shared RAM", k_io_board_base, DualPortRam::k_main_window,
        DualPortRam::k_main_window - 1,
        [&shared_ram](uint32_t offset) { return static_cast<uint16_t>(shared_ram.main_read(offset >> 1)); },
        [&shared_ram](uint32_t offset, uint16_t value) { shared_ram.main_write(offset >> 1, static_cast<uint8_t>(value)); },
        shared_ram_write_byte);
    // Sound command link: the two UARTs' TxD and RxD lines are crossed.
    m_sound_uart->connect_transmitter_to(m_sound->uart());
    m_sound->uart().connect_transmitter_to(*m_sound_uart);

    // Main board UART: 8-bit device on the low byte lane (data at 0xC40000,
    // control/status at 0xC40002). Programs use byte accesses.
    I8251& uart = *m_sound_uart;
    m_bus->map_io("sound UART", k_sound_uart_port, 4, 0x3,
        [&uart](uint32_t offset) { return static_cast<uint16_t>(uart.read(offset >> 1)); },
        [&uart](uint32_t offset, uint16_t value) { uart.write(offset >> 1, static_cast<uint8_t>(value)); },
        [&uart](uint32_t offset, uint8_t value) {
            if ((offset & 1) == 0) {
                uart.write(offset >> 1, value); // the high byte lane is not connected
            }
        });

    PolygonRenderer& polygons = *m_polygons;
    m_bus->map_io("display list control", k_list_control_port, 4, 0x3,
        [&polygons](uint32_t offset) { return polygons.read_list_control(offset); },
        [&polygons](uint32_t offset, uint16_t value) { polygons.write_list_control(offset, value); });

    // Data ROM bank register (MAME bank_w): low nibble 1 selects the bank in
    // bits 6-4 for the 0x100000 window; 2 and 0xF address other banks that
    // no known game uses.
    Bus& bus = *m_bus;
    auto bank_write = [&bus](uint32_t offset, uint16_t value) {
        if (offset == 0 && (value & 0xF) == 0x1) {
            bus.set_data_bank((value >> 4) & 0x7);
        }
    };
    m_bus->map_io("data ROM bank", k_bank_register_port, 2, 0x1, nullptr, bank_write,
        [bank_write](uint32_t offset, uint8_t value) { bank_write(offset, value); });

    // Video sync registers: written by games, no effect emulated (as in MAME).
    for (uint32_t base : {0x720000u, 0x740000u, 0x760000u, 0x770000u}) {
        m_bus->map_io("video sync register", base, 2, 0x1, nullptr, [](uint32_t, uint16_t) {});
    }
    m_bus->map_io("TGP FIFO status", k_tgp_status_port, k_tgp_port_span, 0x3,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_status(offset) : m_tgp->read_status(offset); },
        nullptr);

    // The same TGP ports are also wired into the V60's I/O address space (IN /
    // OUT instructions), as in MAME's model1_io map. Nothing else is there.
    m_bus->map_io_space("TGP shared RAM address", k_tgp_ram_address_port, k_tgp_port_span, 0x1,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_ram_address(offset) : m_tgp->read_ram_address(offset); },
        [this](uint32_t offset, uint16_t value) {
            if (m_tgp_copro->is_active()) {
                m_tgp_copro->write_ram_address(offset, value);
            } else {
                m_tgp->write_ram_address(offset, value);
            }
        });
    m_bus->map_io_space("TGP shared RAM data", k_tgp_ram_data_port, k_tgp_port_span, 0x3,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_ram_data(offset) : m_tgp->read_ram_data(offset); },
        [this](uint32_t offset, uint16_t value) {
            if (m_tgp_copro->is_active()) {
                m_tgp_copro->write_ram_data(offset, value);
            } else {
                m_tgp->write_ram_data(offset, value);
            }
        });
    m_bus->map_io_space("TGP FIFO", k_tgp_fifo_port, k_tgp_port_span, 0x3,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_fifo(offset) : m_tgp->read_fifo(offset); },
        [this](uint32_t offset, uint16_t value) {
            if (m_tgp_copro->is_active()) {
                m_tgp_copro->write_fifo(offset, value);
            } else {
                m_tgp->write_fifo(offset, value);
            }
        });
    m_bus->map_io_space("TGP FIFO status", k_tgp_status_port, k_tgp_port_span, 0x3,
        [this](uint32_t offset) { return m_tgp_copro->is_active() ? m_tgp_copro->read_status(offset) : m_tgp->read_status(offset); },
        nullptr);

    // I/O-space port 0xC10002: Virtua Racing's boot writes 00 00 00 40 4E
    // there, the i8251 UART reset-then-mode sequence, so it is most likely a
    // serial controller (link / drive board). Nothing is wired there in MAME
    // either. Writes are accepted and logged; reads stay unmapped (logged) so
    // a program polling it for status is noticed.
    auto stub_write = [](uint32_t offset, uint32_t value) {
        std::cerr << "[I/O] Stub port " << Hex{k_io_space_stub_port + offset} << " <- "
                  << Hex{value, 2} << " (serial control, no device emulated)\n";
    };
    m_bus->map_io_space("serial control stub", k_io_space_stub_port, 4, 0x3, nullptr,
        [stub_write](uint32_t offset, uint16_t value) { stub_write(offset, value); },
        [stub_write](uint32_t offset, uint8_t value) { stub_write(offset, value); });

    std::cerr << "[Motherboard] Created\n";
}

Motherboard::~Motherboard()
{
    std::cerr << "[Motherboard] Destroyed after " << m_frame_count << " frames\n";
}

void Motherboard::reset()
{
    m_frame_count = 0;
    m_cycle_balance = 0;
    m_bus->reset();
    m_tgp->reset();
    m_tgp_copro->reset();
    m_io_shared_ram->reset();
    // With its firmware loaded, the I/O board runs for real and owns the
    // shared RAM; otherwise InputManager stands in for it.
    m_inputs->set_publishing(!m_io_board->has_firmware());
    m_inputs->reset();
    m_io_board->reset();
    m_io_quarters = 0;
    m_polygons->reset();
    m_sound_uart->reset();
    m_sound->reset();
    m_main_cycles = 0;
    m_sound_eighths = 0;
    m_uart_remainder = 0;
    m_audio_remainder = 0;
    m_cpu->reset();
    std::cerr << "[Motherboard] Reset\n";
}

void Motherboard::run_frame()
{
    // Active display: run the main CPU for one frame's worth of cycles, in
    // slices; after each slice the serial link and the sound board catch up
    // to the same point in time. (The TGP executes instantly.)
    m_cycle_balance += k_main_cycles_per_frame;
    m_inputs->update_analog(); // wheel and pedals move toward the held keys
    if (!m_io_board->has_firmware()) {
        m_inputs->service(); // stand-in I/O board: acknowledge a pending command, refresh the ports
    }
    while (m_cycle_balance > 0) {
        const int64_t slice = std::min(m_cycle_balance, k_slice_cycles);
        int64_t ran = 0;
        while (ran < slice) {
            ran += m_cpu->execute_cycle();
        }
        m_cycle_balance -= ran;
        run_peripherals(static_cast<uint32_t>(ran));
    }

    // End of active display: compose the frame in the board's layer order.
    m_tilemaps->render_background(m_bus->tile_ram(), m_bus->char_ram(), m_bus->palette_ram(), m_frame);
    m_polygons->render(m_bus->display_list_ram(), m_bus->palette_ram(), m_bus->color_xlat_ram(), m_frame);
    m_tilemaps->render_foreground(m_bus->tile_ram(), m_bus->char_ram(), m_bus->palette_ram(), m_frame);
    m_polygons->end_frame();

    // End of frame: the board's interrupt controller raises VBlank. The CPU
    // takes it before its next instruction if PSW.IE allows.
    m_cpu->request_interrupt(k_vblank_irq_level);
    ++m_frame_count;
}

void Motherboard::run_peripherals(uint32_t main_cycles)
{
    m_main_cycles += main_cycles;

    // Serial clock: 1 tick per 32 V60 cycles.
    m_uart_remainder += main_cycles;
    const uint32_t uart_ticks = m_uart_remainder / 32;
    m_uart_remainder %= 32;
    m_sound_uart->tick(uart_ticks);
    m_sound->uart().tick(uart_ticks);

    // Sound CPU: 5/8 of a cycle per V60 cycle. Overshoot is carried, so the
    // long-run ratio is exact.
    m_sound_eighths += static_cast<int64_t>(main_cycles) * 5;
    while (m_sound_eighths > 0) {
        m_sound_eighths -= static_cast<int64_t>(m_sound->step()) * 8;
    }

    // TGP coprocessor DSP (40 MHz / 3), when its ROMs are loaded.
    if (m_tgp_copro->is_active()) {
        m_tgp_copro->run(main_cycles);
    }

    // I/O board Z80 (4 MHz): a quarter cycle per V60 cycle, overshoot carried.
    if (m_io_board->has_firmware()) {
        m_io_quarters += static_cast<int64_t>(main_cycles);
        while (m_io_quarters > 0) {
            m_io_quarters -= static_cast<int64_t>(m_io_board->step()) * 4;
        }
    }

    // Sound chips: their output for the same stretch of time, after the
    // sound CPU's register writes for it.
    m_audio_remainder += static_cast<uint64_t>(main_cycles) * k_audio_samples_per_step;
    const uint64_t samples = m_audio_remainder / k_audio_cycles_per_step;
    m_audio_remainder %= k_audio_cycles_per_step;
    m_sound->generate_audio(static_cast<std::size_t>(samples));
}

bool Motherboard::run_tgp_self_test()
{
    // Host-side helpers: 32-bit FIFO transfers over the bus, as the V60 does.
    auto send_word = [this](uint32_t word) { m_bus->write_long(k_tgp_fifo_port, word); };
    auto send_float = [&send_word](float value) { send_word(Tgp::float_to_word(value)); };
    auto send_function = [&send_word](uint32_t id) { send_word(Tgp::encode_function(id)); };
    auto receive_float = [this]() { return Tgp::word_to_float(m_bus->read_long(k_tgp_fifo_port)); };

    bool all_passed = true;
    auto check = [&all_passed](const char* label, const std::array<float, 3>& got,
                               const std::array<float, 3>& expected, std::size_t count) {
        bool passed = true;
        std::cout << "[TGP self-test] " << label << " = (";
        for (std::size_t i = 0; i < count; ++i) {
            // Bit-exact comparison: these inputs have exact float results.
            passed = passed && Tgp::float_to_word(got[i]) == Tgp::float_to_word(expected[i]);
            std::cout << (i ? ", " : "") << std::setprecision(9) << got[i];
        }
        std::cout << ") " << (passed ? "PASS" : "FAIL") << '\n';
        all_passed = all_passed && passed;
    };

    m_tgp->reset();

    // 1. Load a translation matrix T = translate(10, 20, 30).
    const std::array<float, Tgp::k_matrix_size> translation = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        10.0f, 20.0f, 30.0f,
    };
    send_function(Tgp::k_fn_matrix_write);
    for (float value : translation) {
        send_float(value);
    }

    // 2. Transform vertex [1, 2, 3] by T: expect [11, 22, 33].
    send_function(Tgp::k_fn_transform_point);
    send_float(1.0f);
    send_float(2.0f);
    send_float(3.0f);
    const std::array<float, 3> moved = {receive_float(), receive_float(), receive_float()};
    check("transform [1, 2, 3] by translate(10, 20, 30)", moved, {11.0f, 22.0f, 33.0f}, 3);

    // 3. Multiply in a 90-degree rotation about Z (x -> y, y -> -x), applied
    //    before T: [1, 2, 3] -> [-2, 1, 3] -> [8, 21, 33].
    const std::array<float, Tgp::k_matrix_size> rotate_z90 = {
         0.0f, 1.0f, 0.0f,
        -1.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 1.0f,
         0.0f, 0.0f, 0.0f,
    };
    send_function(Tgp::k_fn_matrix_mul);
    for (float value : rotate_z90) {
        send_float(value);
    }
    send_function(Tgp::k_fn_transform_point);
    send_float(1.0f);
    send_float(2.0f);
    send_float(3.0f);
    const std::array<float, 3> rotated = {receive_float(), receive_float(), receive_float()};
    check("transform [1, 2, 3] by rotZ(90) * translate", rotated, {8.0f, 21.0f, 33.0f}, 3);

    // 4. Debug projection of [11, 22, 33] with focal length 330:
    //    x = 248 + 330 * 11 / 33 = 358, y = 192 - 330 * 22 / 33 = -28.
    send_function(Tgp::k_fn_debug_project);
    send_float(11.0f);
    send_float(22.0f);
    send_float(33.0f);
    send_float(330.0f);
    const std::array<float, 3> projected = {receive_float(), receive_float(), 0.0f};
    check("project [11, 22, 33], focal 330 -> screen", projected, {358.0f, -28.0f, 0.0f}, 2);

    if (m_tgp->output_count() != 0 || m_tgp->is_desynchronized()) {
        std::cout << "[TGP self-test] FAIL: unexpected TGP state after test\n";
        all_passed = false;
    }
    std::cout << "[TGP self-test] " << (all_passed ? "all checks passed" : "FAILED") << '\n';

    m_tgp->reset();
    return all_passed;
}

void Motherboard::load_tile_demo()
{
    Bus& bus = *m_bus;
    auto palette = [&bus](uint32_t pen, uint16_t color) {
        bus.write_word(Bus::k_palette_base + pen * 2, color);
    };
    // Model 1 colour: xBGR 5:5:5, bit 15 = full intensity.
    auto rgb = [](uint16_t r, uint16_t g, uint16_t b) {
        return static_cast<uint16_t>(0x8000 | (b << 10) | (g << 5) | r);
    };
    // One 8x8 tile from 8 rows of 8 pen values (0-15), left to right.
    auto put_tile = [&bus](uint32_t tile, const std::array<uint32_t, 8>& rows) {
        for (uint32_t row = 0; row < 8; ++row) {
            const uint32_t pens = rows[row]; // 0xP0P1P2P3P4P5P6P7
            const uint32_t address = Bus::k_char_ram_base + tile * 32 + row * 4;
            bus.write_word(address, static_cast<uint16_t>(pens >> 16));
            bus.write_word(address + 2, static_cast<uint16_t>(pens));
        }
    };
    auto solid = [](uint32_t pen) {
        const uint32_t row = pen * 0x11111111u;
        return std::array<uint32_t, 8>{row, row, row, row, row, row, row, row};
    };
    auto tile_entry = [](uint32_t tilemap, uint32_t col, uint32_t row) {
        return Bus::k_tile_ram_base + (tilemap * 0x1000 + row * 64 + col) * 2;
    };

    // Palette 0: background and colour bars.
    palette(0, rgb(2, 2, 8));
    const std::array<uint16_t, 8> bars = {
        rgb(31, 31, 31), rgb(31, 31, 0), rgb(0, 31, 31), rgb(0, 31, 0),
        rgb(31, 0, 31), rgb(31, 0, 0), rgb(0, 0, 31), rgb(12, 12, 12),
    };
    for (uint32_t i = 0; i < bars.size(); ++i) {
        palette(1 + i, bars[i]);
    }
    // Palette 1 (pens 16-31): white and dark frame colours.
    palette(16 + 1, rgb(31, 31, 31));
    palette(16 + 2, rgb(6, 6, 6));
    // Palette 2 (pens 32-47): half-intensity yellow (intensity bit clear).
    palette(32 + 1, static_cast<uint16_t>(rgb(31, 31, 0) & 0x7FFF));

    // Tiles. The palette field of a tile entry overlaps the top tile-number
    // bits, so palette p selects tiles from block p * 128.
    for (uint32_t i = 0; i < 8; ++i) {
        put_tile(1 + i, solid(1 + i));                              // palette 0 block
    }
    put_tile(128 + 1, {0x11111111, 0x12222221, 0x12222221, 0x12222221,
                       0x12222221, 0x12222221, 0x12222221, 0x11111111}); // framed cell
    put_tile(256 + 1, {0x10101010, 0x01010101, 0x10101010, 0x01010101,
                       0x10101010, 0x01010101, 0x10101010, 0x01010101}); // checker, 0 = transparent

    // Tilemap 2 (opaque background): 64-pixel colour bars, scrolled by 20 px.
    for (uint32_t row = 0; row < 64; ++row) {
        for (uint32_t col = 0; col < 64; ++col) {
            bus.write_word(tile_entry(2, col, row), static_cast<uint16_t>(1 + (col / 8) % 8));
        }
    }
    // Horizontal scroll of tilemap 2: 9-bit value (bit 15 would select per-line
    // scrolling). A value v moves the layer right by v pixels: -20 = left by 20.
    bus.write_word(Bus::k_tile_ram_base + 0x5002 * 2, static_cast<uint16_t>((-20) & 0x1FF));

    // Tilemap 0 (transparent foreground): a framed box, low priority.
    for (uint32_t row = 18; row < 30; ++row) {
        for (uint32_t col = 16; col < 46; ++col) {
            bus.write_word(tile_entry(0, col, row), static_cast<uint16_t>(128 + 1));
        }
    }
    // Tilemap 0, high priority: a checkerboard band across the top.
    for (uint32_t row = 2; row < 6; ++row) {
        for (uint32_t col = 0; col < 62; ++col) {
            bus.write_word(tile_entry(0, col, row), static_cast<uint16_t>(0x8000 | (256 + 1)));
        }
    }

    // HUD text: an 8x8 font uploaded to character RAM (Model 1 has no
    // character ROM), drawn as high-priority tiles on tilemap 0.
    // Glyph rows, leftmost pixel in bit 7.
    struct Glyph {
        char character;
        std::array<uint8_t, 8> rows;
    };
    static constexpr std::array<Glyph, 13> k_font = {{
        {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {'0', {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00}},
        {'9', {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00}},
        {':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}},
        {'C', {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}},
        {'E', {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00}},
        {'I', {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}},
        {'M', {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}},
        {'N', {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00}},
        {'O', {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}},
        {'R', {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00}},
        {'S', {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}},
        {'T', {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}},
    }};
    // Font copies in tile blocks 3 (white text) and 4 (yellow text): a tile
    // entry's palette bits double as the tile number's top bits.
    constexpr uint32_t k_white_palette = 3;
    constexpr uint32_t k_yellow_palette = 4;
    palette(k_white_palette * 16 + 1, rgb(31, 31, 31));
    palette(k_white_palette * 16 + 2, rgb(0, 0, 0));
    palette(k_yellow_palette * 16 + 1, rgb(31, 28, 0));
    palette(k_yellow_palette * 16 + 2, rgb(0, 0, 0));
    for (uint32_t block : {k_white_palette, k_yellow_palette}) {
        for (uint32_t g = 0; g < k_font.size(); ++g) {
            std::array<uint32_t, 8> rows{};
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    const auto bit = [&](uint32_t gx, uint32_t gy) {
                        return ((k_font[g].rows[gy] >> (7 - gx)) & 1) != 0;
                    };
                    // Pen 1 = glyph, pen 2 = drop shadow (down-right), 0 = transparent.
                    uint32_t pen = 0;
                    if (bit(x, y)) {
                        pen = 1;
                    } else if (x > 0 && y > 0 && bit(x - 1, y - 1)) {
                        pen = 2;
                    }
                    rows[y] |= pen << (28 - 4 * x);
                }
            }
            put_tile(block * 128 + g, rows);
        }
    }
    auto write_text = [&](uint32_t row, uint32_t col, std::string_view text, uint32_t palette_block) {
        for (char ch : text) {
            uint32_t glyph = 0;
            for (uint32_t g = 0; g < k_font.size(); ++g) {
                if (k_font[g].character == ch) {
                    glyph = g;
                }
            }
            bus.write_word(tile_entry(0, col++, row), static_cast<uint16_t>(0x8000 | (palette_block * 128 + glyph)));
        }
    };
    write_text(k_demo_score_row, k_demo_score_col, "SCORE: 000000", k_white_palette);
    write_text(k_demo_score_row, 52, "TIME: 99", k_white_palette);
    write_text(k_demo_coin_row, k_demo_coin_col, "INSERT COIN", k_yellow_palette);

    std::cerr << "[Motherboard] Tile demo loaded into video memory\n";
}

void Motherboard::load_polygon_demo()
{
    Bus& bus = *m_bus;

    // Backdrop: palette entry 0 is the colour behind every layer.
    bus.write_word(Bus::k_palette_base, 0xFC00); // arcade blue, full intensity

    // Face colours at palette 0x1001-0x1006 (Model 1 xBGR 5:5:5 + intensity).
    const std::array<uint16_t, 6> face_colors = {0x801F, 0x83E0, 0xFFFF, 0x83FF, 0xFC1F, 0xFFE0};
    for (uint32_t i = 0; i < face_colors.size(); ++i) {
        bus.write_word(Bus::k_palette_base + (0x1001 + i) * 2, face_colors[i]);
    }
    // Colour translation: a linear ramp, channel * level / 63, for red, green
    // and blue alike.
    for (uint32_t table : {0x0000u, 0x2000u, 0x4000u}) {
        for (uint32_t channel = 0; channel < 32; ++channel) {
            for (uint32_t level = 0; level < 64; ++level) {
                const auto value = static_cast<uint16_t>(((channel * level) / 63) << 3);
                bus.write_word(Bus::k_color_xlat_base + (table | (channel << 8) | level) * 2, value);
            }
        }
    }

    // Enable rendering from list 0 (manual buffering).
    bus.write_word(k_list_control_port, 0x0000);
    bus.write_word(k_list_control_port + 2, 0x001F);

    const uint32_t faces = update_polygon_demo(0.6f, 0.45f);
    std::cerr << "[Motherboard] Polygon demo loaded: cube transformed by the TGP, " << faces
              << " visible faces in display list 0\n";
}

uint32_t Motherboard::update_polygon_demo(float angle_y, float angle_x)
{
    Bus& bus = *m_bus;
    auto send_word = [&bus](uint32_t word) { bus.write_long(k_tgp_fifo_port, word); };
    auto send_float = [&send_word](float value) { send_word(Tgp::float_to_word(value)); };
    auto send_function = [&send_word](uint32_t id) { send_word(Tgp::encode_function(id)); };
    auto receive_float = [&bus]() { return Tgp::word_to_float(bus.read_long(k_tgp_fifo_port)); };

    // --- Transform the cube on the TGP: p' = p * Rx * Ry * translate(0, 0, 6)
    send_function(Tgp::k_fn_matrix_ident);
    send_function(Tgp::k_fn_matrix_trans);
    send_float(0.0f);
    send_float(0.0f);
    send_float(6.0f);
    auto multiply = [&](const std::array<float, 12>& m) {
        send_function(Tgp::k_fn_matrix_mul);
        for (float v : m) {
            send_float(v);
        }
    };
    const float cy = std::cos(angle_y);
    const float sy = std::sin(angle_y);
    const float cx = std::cos(angle_x);
    const float sx = std::sin(angle_x);
    multiply({cy, 0.0f, -sy, 0.0f, 1.0f, 0.0f, sy, 0.0f, cy, 0.0f, 0.0f, 0.0f});
    multiply({1.0f, 0.0f, 0.0f, 0.0f, cx, sx, 0.0f, -sx, cx, 0.0f, 0.0f, 0.0f});

    struct Vec { float x, y, z; };
    std::array<Vec, 8> view{}; // vertex i: x = bit 0, y = bit 1, z = bit 2
    for (uint32_t i = 0; i < view.size(); ++i) {
        send_function(Tgp::k_fn_transform_point);
        send_float((i & 1) != 0 ? 1.0f : -1.0f);
        send_float((i & 2) != 0 ? 1.0f : -1.0f);
        send_float((i & 4) != 0 ? 1.0f : -1.0f);
        view[i] = {receive_float(), receive_float(), receive_float()};
    }

    // --- Display list 0, written through the bus
    auto put16 = [&bus](std::size_t index, uint16_t value) {
        bus.write_word(Bus::k_display_list_base + static_cast<uint32_t>(index * 2), value);
    };
    auto put32 = [&bus](std::size_t index, uint32_t value) {
        bus.write_long(Bus::k_display_list_base + static_cast<uint32_t>(index * 2), value);
    };
    auto putf = [&put32](std::size_t index, float value) { put32(index, Tgp::float_to_word(value)); };
    std::size_t w = 0;

    // Viewport: full screen, centre (248, 192). y values are stored as 422 - y.
    put32(w + 0, 0x03);
    put32(w + 2, 0);
    put16(w + 4, 248);
    put16(w + 6, static_cast<uint16_t>(422 - 192));
    put16(w + 8, 0);
    put16(w + 10, static_cast<uint16_t>(422 - 383));
    put16(w + 12, 495);
    put16(w + 14, static_cast<uint16_t>(422 - 0));
    w += 16;

    // Colour table entries 0x40000-0x40005 -> palette indices 1-6.
    put32(w + 0, 0x04);
    put32(w + 2, PolygonRenderer::k_color_table_base);
    put32(w + 4, 5); // count - 1
    for (uint32_t i = 0; i < 6; ++i) {
        put32(w + 6 + 2 * i, 1 + i);
    }
    w += 6 + 2 * 6;

    // Faces as corner lists wound counter-clockwise seen from outside, so the
    // normal (b - a) x (c - a) points out of the cube.
    constexpr std::array<std::array<uint32_t, 4>, 6> k_faces = {{
        {0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6},
    }};
    constexpr float k_focal = 300.0f;
    auto project = [](const Vec& v) { return Vec{k_focal * v.x / v.z, k_focal * v.y / v.z, v.z}; };

    uint32_t visible = 0;
    for (std::size_t f = 0; f < k_faces.size(); ++f) {
        const Vec& va = view[k_faces[f][0]];
        const Vec& vb = view[k_faces[f][1]];
        const Vec& vc = view[k_faces[f][2]];
        const Vec e1{vb.x - va.x, vb.y - va.y, vb.z - va.z};
        const Vec e2{vc.x - va.x, vc.y - va.y, vc.z - va.z};
        const Vec n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};

        // Backface culling: the camera is at the origin looking down +z, so a
        // face is visible only if its outward normal points back toward the
        // camera, i.e. against the direction from the camera to the face.
        if (n.x * va.x + n.y * va.y + n.z * va.z >= 0.0f) {
            continue;
        }
        ++visible;

        const Vec a = project(va);
        const Vec b = project(vb);
        const Vec c = project(vc);
        const Vec d = project(view[k_faces[f][3]]);
        const float depth = (a.z + b.z + c.z + d.z) / 4.0f; // painter's sort key

        // Flat shading: brightness from the angle between the face normal
        // and a fixed light direction.
        const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        const float lit = std::fabs((n.x * 0.3f + n.y * 0.5f - n.z * 0.81f) / length);
        const auto level = static_cast<uint32_t>(std::lround(20.0f + 43.0f * lit)); // 0-63

        // One direct-polygon strip drawing quad (A, B, C, D).
        put32(w + 0, 0x02);
        put32(w + 2, PolygonRenderer::k_color_table_base + static_cast<uint32_t>(f));
        putf(w + 6, b.x);  putf(w + 8, b.y);  putf(w + 10, b.z);   // first point
        putf(w + 14, a.x); putf(w + 16, a.y); putf(w + 18, a.z);   // second point
        put32(w + 18 + 2, 0x0201);                                  // two points, link 2
        put32(w + 18 + 4, (level * 2) << 24);                       // luminance
        putf(w + 18 + 8, c.x);  putf(w + 18 + 10, c.y); putf(w + 18 + 12, c.z);
        putf(w + 18 + 14, depth);
        putf(w + 18 + 16, d.x); putf(w + 18 + 18, d.y); putf(w + 18 + 20, d.z);
        put32(w + 38 + 2, 0);                                       // end of strip
        w += 42;
    }
    put32(w, 0x0F); // end of list
    return visible;
}

void Motherboard::load_sound_demo()
{
    // Sample ROM (loaded like a ROM image): header 0 describes a looping
    // 64-sample, 8-bit sine wave stored at 0x000100.
    constexpr uint32_t k_wave_address = 0x000100;
    constexpr uint32_t k_wave_length = 64;
    std::array<uint8_t, 12> header{};
    header[0] = static_cast<uint8_t>(k_wave_address >> 16);
    header[1] = static_cast<uint8_t>(k_wave_address >> 8);
    header[2] = static_cast<uint8_t>(k_wave_address);
    header[3] = 0x00; // loop start: sample 0
    header[4] = 0x00;
    header[5] = static_cast<uint8_t>((0x10000 - k_wave_length) >> 8); // end, stored as 0x10000 - length
    header[6] = static_cast<uint8_t>(0x10000 - k_wave_length);
    // Envelope: attack rate 12 (3 ms), no decay (full level while held),
    // no key rate scaling, release rate 10 (about 170 ms): no clicks.
    header[8] = 0xC0;  // attack 12, decay 1 rate 0
    header[9] = 0x00;  // decay level 0, decay 2 rate 0
    header[10] = 0xFA; // key rate scaling off, release 10
    std::array<uint8_t, k_wave_length> wave{};
    for (uint32_t i = 0; i < k_wave_length; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / k_wave_length;
        wave[i] = static_cast<uint8_t>(static_cast<int8_t>(std::lround(100.0 * std::sin(angle))));
    }
    MultiPCM& pcm = m_sound->pcm1();
    pcm.load_sample_rom(header, 0);
    pcm.load_sample_rom(wave, k_wave_address);

    // Slot 0 programmed through the sound bus, as the 68000 would. Pitch:
    // 440 Hz x 64 samples / 44,642.86 Hz = 0.6308 samples per output sample
    // = 2^(0 - 1) x (1 + 268 / 1024): octave 0, pitch 268.
    SoundBus& bus = m_sound->bus();
    auto pcm_write = [&bus](uint8_t slot, uint8_t reg, uint8_t value) {
        bus.write_byte(SoundBus::k_pcm1_base + 3, slot); // slot select
        bus.write_byte(SoundBus::k_pcm1_base + 5, reg);  // register select
        bus.write_byte(SoundBus::k_pcm1_base + 1, value);
    };
    constexpr uint32_t k_pitch = 268;
    pcm_write(0, 0, 0x00);                                      // pan: centre
    pcm_write(0, 2, static_cast<uint8_t>((k_pitch & 0x3F) << 2)); // pitch low, sample bit 8 = 0
    pcm_write(0, 1, 0);                                         // sample 0 (loads the header)
    pcm_write(0, 3, static_cast<uint8_t>((0 << 4) | (k_pitch >> 6))); // octave 0, pitch high
    pcm_write(0, 5, (16 << 1) | 1);                             // attenuation 16 (-6 dB), set now
    std::cerr << "[Motherboard] Sound demo loaded: 440 Hz sine on MultiPCM 1, slot 0\n";
}

void Motherboard::update_sound_demo(uint64_t frame)
{
    const uint64_t phase = frame % k_refresh_rate_hz;
    if (phase != 0 && phase != k_refresh_rate_hz / 2) {
        return;
    }
    SoundBus& bus = m_sound->bus();
    bus.write_byte(SoundBus::k_pcm1_base + 3, 0); // slot 0
    bus.write_byte(SoundBus::k_pcm1_base + 5, 4); // key register
    bus.write_byte(SoundBus::k_pcm1_base + 1, phase == 0 ? 0x80 : 0x00);
}

} // namespace model1

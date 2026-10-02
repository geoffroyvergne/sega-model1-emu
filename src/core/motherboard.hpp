#pragma once

#include "core/bus.hpp"
#include "core/dual_port_ram.hpp"
#include "core/input_manager.hpp"
#include "core/io_board.hpp"
#include "core/i8251.hpp"
#include "core/polygon_renderer.hpp"
#include "core/sound_board.hpp"
#include "core/tilemap_renderer.hpp"
#include "core/tgp.hpp"
#include "core/tgp_copro.hpp"
#include "core/v60.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace model1 {

// Central bus / lifecycle owner for all emulated components.
// Not emulated yet: the sound chips (MultiPCM, YM3438) and audio output.
// Components never talk to each other directly; they go through this class.
class Motherboard {
public:
    // Main CPU clock: NEC V60 @ 16 MHz, display refresh @ 60 Hz.
    static constexpr uint32_t k_main_cpu_clock_hz = 16'000'000;
    static constexpr uint32_t k_refresh_rate_hz = 60;
    static constexpr uint32_t k_main_cycles_per_frame = k_main_cpu_clock_hz / k_refresh_rate_hz; // 266,666

    // Other clocks, run in lockstep with the V60. The 68000 gets exactly
    // 10/16 of the V60's cycles (166,666.25 per frame); the UARTs' serial
    // clock (500 kHz = 16 x 31.25 kbaud) gets 1/32.
    static constexpr uint32_t k_sound_cpu_clock_hz = SoundBoard::k_cpu_clock_hz;
    static constexpr uint32_t k_uart_clock_hz = 500'000;
    static_assert(k_main_cpu_clock_hz * 5 == k_sound_cpu_clock_hz * 8, "68000 runs at 5/8 of the V60 clock");
    static_assert(k_main_cpu_clock_hz == k_uart_clock_hz * 32, "UART clock is 1/32 of the V60 clock");
    // MultiPCM output: 10 MHz / 224 = 5 samples per 1,792 V60 cycles.
    static constexpr uint32_t k_audio_samples_per_step = 5;
    static constexpr uint32_t k_audio_cycles_per_step = 1792;
    static_assert(static_cast<uint64_t>(k_main_cpu_clock_hz) * k_audio_samples_per_step * MultiPCM::k_clock_divider
                      == static_cast<uint64_t>(k_audio_cycles_per_step) * SoundBoard::k_cpu_clock_hz,
                  "5 samples per 1,792 V60 cycles must equal 10 MHz / 224");

    // The frame is run in slices of this many V60 cycles (256 us); after
    // each slice the UARTs, the sound CPU and the I/O board catch up. The
    // TGP DSP instead advances after every V60 instruction (it shares the
    // copro RAM with the V60).
    static constexpr int64_t k_slice_cycles = 4096;

    // TGP host ports (real Model 1 addresses; each block mirrors over 128 KB).
    static constexpr uint32_t k_tgp_ram_address_port = 0xD00000;
    static constexpr uint32_t k_tgp_ram_data_port    = 0xD20000;
    static constexpr uint32_t k_tgp_fifo_port        = 0xD80000;
    static constexpr uint32_t k_tgp_status_port      = 0xDC0000;
    static constexpr uint32_t k_tgp_port_span        = 0x020000;

    // Display list control register (list selection, render enable).
    static constexpr uint32_t k_list_control_port = 0x680000;

    // Data ROM bank register: writing (bank << 4) | 1 selects the bank.
    static constexpr uint32_t k_bank_register_port = 0xE00004;
    static constexpr uint32_t k_io_space_stub_port = 0xC10000; // V60 I/O space, 0xC10000-0xC10003

    // Main board UART (sound command link): data at +0, control/status at +2.
    static constexpr uint32_t k_sound_uart_port = 0xC40000;

    // I/O board block (inputs, lamps), reached through the dual-port RAM.
    static constexpr uint32_t k_io_board_base = 0xC00000;

    // Model 1 interrupt level raised at the end of every frame (VBlank).
    static constexpr uint8_t k_vblank_irq_level = 1;

    Motherboard();
    ~Motherboard();

    Motherboard(const Motherboard&) = delete;
    Motherboard& operator=(const Motherboard&) = delete;
    Motherboard(Motherboard&&) = delete;
    Motherboard& operator=(Motherboard&&) = delete;

    // Puts every component in its power-on state.
    void reset();

    // Advances emulation by one video frame (1/60 s of machine time): runs the
    // V60 for k_main_cycles_per_frame cycles, composes the frame (tilemap
    // background, 3D polygons, tilemap foreground), then raises the VBlank IRQ.
    void run_frame();

    // Last composed frame (496x384, 0xAARRGGBB, row-major).
    [[nodiscard]] std::span<const uint32_t> frame() const { return m_frame; }

    // Fills palette, character and tile RAM through the bus with a static
    // demonstration screen (colour bars, a scrolled background, a framed box,
    // a high-priority checkerboard band and HUD text: "SCORE: 000000",
    // "TIME: 99", "INSERT COIN" drawn with an 8x8 font uploaded to character
    // RAM), the way game code would. For checking the 2D pipeline without a
    // game ROM; a running program will overwrite it.
    void load_tile_demo();

    // Tile RAM row / column where the demo's HUD strings start (for tests).
    static constexpr uint32_t k_demo_score_row = 1;
    static constexpr uint32_t k_demo_score_col = 1;
    static constexpr uint32_t k_demo_coin_row = 44;
    static constexpr uint32_t k_demo_coin_col = 25;

    // 3D demo, written the way game code would, for checking the 3D path
    // without a game ROM. load_polygon_demo() sets up colours (arcade-blue
    // backdrop in palette entry 0, face colours, a colour translation ramp)
    // and enables rendering, then draws the cube once. update_polygon_demo()
    // re-runs the geometry for new rotation angles (call it every frame to
    // spin the cube): the cube is rotated and translated on the TGP through
    // its FIFO, faces turned away from the camera are culled, and the visible
    // ones are perspective-projected, lit and written as direct polygons to
    // display list 0. Returns the number of faces drawn.
    void load_polygon_demo();

    // Audio demo, written the way sound code would: a 64-point sine wave is
    // placed in MultiPCM 1's sample ROM with a looping sample header, and
    // slot 0 is programmed through the sound bus for 440 Hz.
    // update_sound_demo() keys it on and off (half a second each) based on
    // the frame number, for a repeating beep.
    void load_sound_demo();
    void update_sound_demo(uint64_t frame);
    uint32_t update_polygon_demo(float angle_y, float angle_x);

    // Drives the TGP through its bus FIFO the way V60 code would (vertex
    // [1, 2, 3], translation matrix, transform, matrix multiply, debug
    // projection), prints the results to stdout and returns true if every
    // result is bit-exact. Leaves the TGP reset afterwards.
    bool run_tgp_self_test();

    [[nodiscard]] uint64_t frame_count() const { return m_frame_count; }

    // The system bus. Owned here; components will receive a reference to it.
    [[nodiscard]] Bus& bus() { return *m_bus; }
    [[nodiscard]] V60& cpu() { return *m_cpu; }
    [[nodiscard]] Tgp& tgp() { return *m_tgp; }
    [[nodiscard]] TgpCopro& tgp_copro() { return *m_tgp_copro; }
    [[nodiscard]] InputManager& inputs() { return *m_inputs; }
    [[nodiscard]] DualPortRam& io_shared_ram() { return *m_io_shared_ram; }
    [[nodiscard]] IoBoard& io_board() { return *m_io_board; }
    [[nodiscard]] PolygonRenderer& polygons() { return *m_polygons; }
    [[nodiscard]] I8251& sound_uart() { return *m_sound_uart; }
    [[nodiscard]] SoundBoard& sound() { return *m_sound; }

    // Total cycles run since reset.
    [[nodiscard]] uint64_t main_cpu_cycles() const { return m_main_cycles; }
    [[nodiscard]] uint64_t sound_cpu_cycles() const { return m_sound->cpu().cycle_count(); }

private:
    // Heap-allocated: the Bus holds several MB of fixed-size memory arrays.
    // Declared before the CPU: the CPU holds a reference to the bus.
    std::unique_ptr<Bus> m_bus;
    std::unique_ptr<Tgp> m_tgp;
    std::unique_ptr<TgpCopro> m_tgp_copro; // real TGP (MB86233), active when its ROMs are loaded
    std::unique_ptr<DualPortRam> m_io_shared_ram; // MB8421 shared with the I/O board
    std::unique_ptr<InputManager> m_inputs;
    std::unique_ptr<IoBoard> m_io_board; // runs only when its firmware is loaded
    std::unique_ptr<TilemapRenderer> m_tilemaps;
    std::unique_ptr<PolygonRenderer> m_polygons;
    std::unique_ptr<I8251> m_sound_uart; // main board side of the link
    std::unique_ptr<SoundBoard> m_sound;

    // The composed output frame.
    std::vector<uint32_t> m_frame = std::vector<uint32_t>(TilemapRenderer::k_pixel_count, 0xFF000000u);
    std::unique_ptr<V60> m_cpu;
    uint64_t m_frame_count = 0;

    // Cycles still owed to (positive) or overdrawn by (negative) the CPU.
    // Instructions never stop exactly on the frame boundary, so any overshoot
    // is carried into the next frame to keep the long-run rate exact.
    int64_t m_cycle_balance = 0;

    // Advances the UARTs and the sound board by `main_cycles` V60 cycles.
    void run_peripherals(uint32_t main_cycles);

    uint64_t m_main_cycles = 0;
    // 68000 cycles owed, in eighths of a cycle (1 V60 cycle = 5/8 of one).
    int64_t m_sound_eighths = 0;
    int64_t m_io_quarters = 0; // I/O board Z80: 1/4 cycle per V60 cycle
    // V60 cycles not yet converted to UART clock ticks (32 per tick).
    uint32_t m_uart_remainder = 0;
    // V60 cycles x 5 not yet converted to audio samples (1,792 per 5 samples).
    uint64_t m_audio_remainder = 0;
};

} // namespace model1

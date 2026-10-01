#pragma once

#include "core/bus.hpp"
#include "core/input_manager.hpp"
#include "core/tgp.hpp"
#include "core/v60.hpp"

#include <cstdint>
#include <memory>

namespace model1 {

// Central bus / lifecycle owner for all emulated components.
// Future members: Z80 + MultiPCM, video hardware.
// Components never talk to each other directly; they go through this class.
class Motherboard {
public:
    // Main CPU clock: NEC V60 @ 16 MHz, display refresh @ 60 Hz.
    static constexpr uint32_t k_main_cpu_clock_hz = 16'000'000;
    static constexpr uint32_t k_refresh_rate_hz = 60;
    static constexpr uint32_t k_main_cycles_per_frame = k_main_cpu_clock_hz / k_refresh_rate_hz; // 266,666

    // TGP host ports (real Model 1 addresses; each block mirrors over 128 KB).
    static constexpr uint32_t k_tgp_ram_address_port = 0xD00000;
    static constexpr uint32_t k_tgp_ram_data_port    = 0xD20000;
    static constexpr uint32_t k_tgp_fifo_port        = 0xD80000;
    static constexpr uint32_t k_tgp_status_port      = 0xDC0000;
    static constexpr uint32_t k_tgp_port_span        = 0x020000;

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
    // V60 for k_main_cycles_per_frame cycles, then raises the VBlank IRQ.
    void run_frame();

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
    [[nodiscard]] InputManager& inputs() { return *m_inputs; }

private:
    // Heap-allocated: the Bus holds several MB of fixed-size memory arrays.
    // Declared before the CPU: the CPU holds a reference to the bus.
    std::unique_ptr<Bus> m_bus;
    std::unique_ptr<Tgp> m_tgp;
    std::unique_ptr<InputManager> m_inputs;
    std::unique_ptr<V60> m_cpu;
    uint64_t m_frame_count = 0;

    // Cycles still owed to (positive) or overdrawn by (negative) the CPU.
    // Instructions never stop exactly on the frame boundary, so any overshoot
    // is carried into the next frame to keep the long-run rate exact.
    int64_t m_cycle_balance = 0;
};

} // namespace model1

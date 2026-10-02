#pragma once

#include <cstdint>

namespace model1 {

// Model 1 main board interrupt controller (the "GLUE" chip at 0xE00000), as
// in MAME's model1.cpp. Emulated hardware: no host dependencies.
//
// Eight levels, each with a pending bit. The V60's maskable interrupt line
// is asserted while any bit is pending; on acknowledge the CPU receives the
// lowest pending level as its vector number (handler at vector table entry
// level + 0x40). A level stays pending until the program acknowledges it.
//
//   0xE00000  write  0x10: clear every pending level
//                    0x20: clear the level last acknowledged
//   0xE00002  read / write  mask, active high: bit n set = level n masked.
//             Powers up as 0xFF; Virtua Racing and Virtua Fighter write
//             0xFF then 0xFD (VBlank only), and VF unmasks level 3 while
//             its sound queue holds data.
//
// Sources (the mask is checked when they fire, as on the board):
//   level 1  VBlank (start of line 384)
//   level 3  main board UART ready to send or holding a received byte
//            (its handler pumps the sound command queue)
//   level 0  the two programmable timers (Star Wars Arcade)
class InterruptController {
public:
    static constexpr int k_level_timer = 0;
    static constexpr int k_level_vblank = 1;
    static constexpr int k_level_uart = 3;

    void reset();

    // Marks `level` pending if it is not masked.
    void raise(int level);
    [[nodiscard]] bool masked(int level) const { return (m_mask & (1u << level)) != 0; }

    // The CPU's interrupt line.
    [[nodiscard]] bool line() const { return m_status != 0; }
    // Acknowledge cycle: the lowest pending level, which becomes the one a
    // 0x20 control write clears.
    uint8_t acknowledge();

    void write_control(uint8_t value);
    [[nodiscard]] uint8_t mask() const { return m_mask; }
    void write_mask(uint8_t value) { m_mask = value; }

    [[nodiscard]] uint8_t status() const { return m_status; }

private:
    uint8_t m_status = 0;
    uint8_t m_mask = 0xFF;
    uint8_t m_last = 0;
};

// The GLUE chip's two programmable timers, as in MAME's model1.cpp:
//   0xE00006  mode (stored, no effect)
//   0xE00008  timer 0 period, 0xE0000A timer 1 period: a non-zero period p
//             (re)starts the timer, which then fires every 0x800 x p main
//             CPU cycles; 0 stops it
//   0xE0000C  timer 0 count, 0xE0000E timer 1 count: reads give the time
//             left in units of 0x800 cycles (the last value once stopped);
//             writes are ignored
// Each expiry raises level 0 (when unmasked). Offsets are byte offsets from
// 0xE00006.
class GlueTimers {
public:
    static constexpr uint32_t k_base = 0xE00006;
    static constexpr uint32_t k_size = 10;
    static constexpr uint64_t k_tick_cycles = 0x800;

    void reset();
    [[nodiscard]] uint16_t read(uint32_t offset);
    void write(uint32_t offset, uint16_t value);
    void write_byte(uint32_t offset, uint8_t value);
    // Advances both timers; returns how many times they fired in total.
    uint32_t run(uint32_t cycles);

    [[nodiscard]] uint16_t period(int timer) const { return m_period[timer]; }
    [[nodiscard]] uint16_t mode() const { return m_mode; }

private:
    void set_period(int timer, uint16_t value);

    uint16_t m_mode = 0;
    uint16_t m_period[2] = {};
    uint16_t m_value[2] = {};      // last count read
    uint64_t m_remaining[2] = {};  // cycles to the next expiry (when running)
};

} // namespace model1

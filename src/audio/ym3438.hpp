#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace model1 {

// Yamaha YM3438 (OPN2C) FM chip on the Model 1 sound board, clocked at
// 8 MHz. Emulated hardware: no host (SDL) dependencies.
//
// Only the timers are emulated so far: the sound program waits on Timer A
// before it starts. FM synthesis is not emulated (no FM audio); every
// register write is stored so it can be added later.
//
// CPU interface (byte ports):
//   0  address, part 1 (registers 0x21-0xB6, channels 1-3)  read: status
//   1  data, part 1
//   2  address, part 2 (channels 4-6)                        read: status
//   3  data, part 2
// Status: bit 0 = Timer A overflowed, bit 1 = Timer B overflowed,
// bit 7 = busy (always 0 here: writes take effect immediately).
//
// Timers, counted in FM samples (one per 144 chip clocks, 55.6 kHz):
//   0x24  Timer A bits 9-2     0x25  Timer A bits 1-0
//   0x26  Timer B (8 bits)
//   0x27  bit 0 / 1  run Timer A / B (reloaded when started)
//         bit 2 / 3  let Timer A / B set its status flag on overflow
//         bit 4 / 5  clear Timer A / B status flag (not stored)
//         bits 7-6   channel 3 mode (stored, not emulated)
// Timer A overflows every (1024 - A) samples, Timer B every
// (256 - B) * 16 samples; each then reloads and keeps running.
class Ym3438 {
public:
    static constexpr uint32_t k_clock_hz = 8'000'000;
    static constexpr uint32_t k_clocks_per_sample = 144;
    static constexpr uint32_t k_timer_b_prescale = 16; // samples per Timer B count
    static constexpr uint8_t k_status_timer_a = 0x01;
    static constexpr uint8_t k_status_timer_b = 0x02;

    explicit Ym3438(std::string name);

    void reset();

    // Port access (0-3, see above).
    [[nodiscard]] uint8_t read(uint32_t port) const;
    void write(uint32_t port, uint8_t value);

    // Advances the chip by `clocks` input clocks (8 MHz).
    void clock(uint32_t clocks);

    [[nodiscard]] uint8_t status() const { return m_status; }
    // Interrupt output: asserted while a status flag is set. Not wired to
    // the 68000 on this board model.
    [[nodiscard]] bool irq() const { return m_status != 0; }
    [[nodiscard]] uint8_t register_value(uint32_t part, uint32_t reg) const
    {
        return m_regs[part & 1][reg & 0xFF];
    }
    [[nodiscard]] uint32_t timer_a_period() const { return 1024 - timer_a_value(); }        // in samples
    [[nodiscard]] uint32_t timer_b_period() const { return (256 - m_regs[0][0x26]) * k_timer_b_prescale; }

private:
    void write_register(uint32_t part, uint8_t reg, uint8_t value);
    void write_timer_control(uint8_t value);
    void sample_tick();
    [[nodiscard]] uint32_t timer_a_value() const
    {
        return (static_cast<uint32_t>(m_regs[0][0x24]) << 2) | (m_regs[0][0x25] & 3u);
    }

    std::string m_name;
    std::array<std::array<uint8_t, 256>, 2> m_regs{};
    std::array<uint8_t, 2> m_address{}; // latched register number per part
    uint8_t  m_control = 0;             // register 0x27 (reset bits excluded)
    uint8_t  m_status = 0;
    uint32_t m_timer_a = 0;             // counts up to 1024
    uint32_t m_timer_b = 0;             // counts up to 256
    uint32_t m_timer_b_prescaler = 0;
    uint32_t m_clock_remainder = 0;     // clocks toward the next sample
    bool     m_logged_fm = false;
};

} // namespace model1

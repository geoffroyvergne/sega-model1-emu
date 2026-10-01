#pragma once

#include <array>
#include <cstdint>

namespace model1 {

// Fujitsu MB8421: 2 KB x 8 dual-port RAM between the main board and the
// Model 1 I/O board (837-8950, a Z80 running EPR-14869 with a 315-5338A I/O
// chip, an ADC and a 93C45 EEPROM). Emulated hardware: no host dependencies.
//
// It is plain shared memory: there are no fixed registers in it. What each
// byte means is decided by the I/O board firmware and the game. For example,
// Virtua Racing writes "SEGA" at bytes 0x1A-0x1D, 0x01 at byte 0x20, then
// reads and rewrites a 128-byte block at 0x100-0x17F (the size of the
// board's 93C45 EEPROM).
//
// Main board side (MAME: map(0xc00000, 0xc00fff) ... umask16(0x00ff)):
// byte n sits on the V60's low byte lane at 0xC00000 + 2n; the high lane is
// not connected (reads 0, writes ignored).
//
// The board side is accessed by the I/O board model (for now the high-level
// InputManager; later the real Z80 firmware).
//
// Not emulated: the MB8421's interrupt mailboxes (a write to 0x7FF from one
// side interrupts the other side, 0x7FE in the other direction), and access
// contention (MAME adds one wait state per main-side read).
class DualPortRam {
public:
    static constexpr uint32_t k_size = 0x800;              // bytes
    static constexpr uint32_t k_main_window = k_size * 2;  // 0xC00000-0xC00FFF on the V60

    void reset() { m_bytes.fill(0); }

    // Main board (V60) side: byte index 0-0x7FF.
    [[nodiscard]] uint8_t main_read(uint32_t index) const { return m_bytes[index % k_size]; }
    void main_write(uint32_t index, uint8_t value) { m_bytes[index % k_size] = value; }

    // I/O board side: the same memory.
    [[nodiscard]] uint8_t board_read(uint32_t index) const { return m_bytes[index % k_size]; }
    void board_write(uint32_t index, uint8_t value) { m_bytes[index % k_size] = value; }

private:
    std::array<uint8_t, k_size> m_bytes{};
};

} // namespace model1

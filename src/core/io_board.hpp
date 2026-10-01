#pragma once

#include "core/eeprom_93c46.hpp"
#include "core/z80.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace model1 {

class DualPortRam;
class InputManager;

// Sega Model 1 I/O board (837-8950), as in MAME's model1io device: a Z80 at
// 4 MHz running the board firmware (EPR-14869, a 64 KB EPROM), which talks
// to the main board only through the MB8421 shared RAM. Emulated hardware:
// no host dependencies.
//
// Z80 memory map (no I/O ports, no interrupt sources):
//   0x0000 - 0x3FFF  firmware ROM (first 16 KB of the EPROM, as MAME maps it)
//   0x4000 - 0x5FFF  RAM 8 KB (MB8464)
//   0x8000 - 0x800F  Sega 315-5338A I/O controller
//   0xC000 - 0xC003  OKI MSM6253 ADC: write selects channel 0-3 (latches its
//                    8-bit value), each read shifts one bit out on D0, MSB first
//
// 315-5338A registers:
//   0x00-0x06  ports A-G: input (when the direction bit is 1) or output latch
//   0x08       port direction, bit n = 1: port n is an input
//   0x09       command: 0x00 / 0x01 latch the low / high address byte from
//              register 0x0A; 0x07 writes register 0x0A to the shared RAM at
//              that address; 0x70-0x77 write it to shared RAM bytes 0-7
//   0x0A       data to write (reads back)          0x0B  command (reads back)
//   0x0C       read: shared RAM byte at the latched address
//   0x0D       status: always 0x08 (transfer done), as in MAME
// Ports:
//   A (out)  bit 7 EEPROM CLK, bit 6 CS, bit 5 DI, bit 1 LED, bit 0 selects
//            the DIP switches instead of the inputs on ports B-D
//   B, C, D  system / player 1 / player 2 inputs (InputManager, active low),
//            or DIP switch banks 1-3 (all off: 0xFF)
//   E        drive (force feedback) board: reads 0xFF, writes stored
//   F (out)  lamps / coin meters
//   G (in)   bit 7 EEPROM DO, bits 6-4 high, bits 3-0 board buttons SW4-SW7
//            (active low, released)
class IoBoard final : public Z80Bus {
public:
    static constexpr uint32_t k_clock_hz = 4'000'000;
    static constexpr std::size_t k_rom_size = 0x10000;
    static constexpr uint16_t k_rom_window = 0x4000;
    static constexpr uint16_t k_ram_base = 0x4000;
    static constexpr std::size_t k_ram_size = 0x2000;
    static constexpr uint16_t k_io_chip_base = 0x8000;
    static constexpr uint16_t k_adc_base = 0xC000;

    IoBoard(DualPortRam& shared_ram, const InputManager& inputs);

    IoBoard(const IoBoard&) = delete;
    IoBoard& operator=(const IoBoard&) = delete;

    // Loads the firmware (up to 64 KB). Without it the board does not run
    // (the Motherboard then uses InputManager's high-level stand-in).
    bool load_rom(std::span<const uint8_t> data);
    [[nodiscard]] bool has_firmware() const { return m_has_firmware; }

    // Board reset: CPU, RAM, I/O controller; the EEPROM keeps its contents.
    void reset();

    // Runs one Z80 instruction; returns the T-states it took.
    uint32_t step();

    [[nodiscard]] Z80& cpu() { return *m_cpu; }
    [[nodiscard]] Eeprom93c46& eeprom() { return m_eeprom; }
    [[nodiscard]] uint8_t lamp_outputs() const { return m_port_value[5]; }

    // Z80Bus
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    uint8_t in(uint16_t port) override;
    void out(uint16_t port, uint8_t value) override;

private:
    uint8_t io_chip_read(uint32_t reg);
    void io_chip_write(uint32_t reg, uint8_t value);
    uint8_t port_input(uint32_t port);
    void port_output(uint32_t port, uint8_t value);
    void log_unmapped(const char* what, uint16_t address);

    DualPortRam& m_shared_ram;
    const InputManager& m_inputs;
    std::unique_ptr<Z80> m_cpu;
    Eeprom93c46 m_eeprom{k_clock_hz};

    std::array<uint8_t, k_rom_size> m_rom{};
    std::array<uint8_t, k_ram_size> m_ram{};
    bool m_has_firmware = false;

    // 315-5338A
    std::array<uint8_t, 7> m_port_value{};
    uint8_t  m_port_direction = 0;
    uint8_t  m_command = 0;
    uint8_t  m_serial_output = 0;
    uint16_t m_address = 0;
    bool     m_select_dip_switches = false;

    // MSM6253
    uint8_t m_adc_shift = 0;

    bool m_logged_unmapped = false;
    bool m_logged_unknown_command = false;
};

} // namespace model1

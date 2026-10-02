#include "core/io_board.hpp"

#include "core/dual_port_ram.hpp"
#include "core/input_manager.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <iostream>

namespace model1 {

namespace {

constexpr uint8_t k_dip_switches = 0xFF;   // all three banks: every switch off (unused by the games)
constexpr uint8_t k_board_buttons = 0x0F;  // SW4-SW7 released (active low)

} // namespace

IoBoard::IoBoard(DualPortRam& shared_ram, const InputManager& inputs)
    : m_shared_ram(shared_ram)
    , m_inputs(inputs)
    , m_cpu(std::make_unique<Z80>(*this))
{
    m_rom.fill(0xFF);
}

bool IoBoard::load_rom(std::span<const uint8_t> data)
{
    if (data.empty() || data.size() > k_rom_size) {
        std::cerr << "[I/O board] ERROR: firmware of " << data.size() << " bytes does not fit the 64 KB EPROM\n";
        return false;
    }
    m_rom.fill(0xFF);
    std::copy(data.begin(), data.end(), m_rom.begin());
    m_has_firmware = true;
    return true;
}

void IoBoard::reset()
{
    m_ram.fill(0);
    m_port_value.fill(0xFF);
    m_port_direction = 0;
    m_command = 0;
    m_serial_output = 0;
    m_address = 0;
    m_select_dip_switches = false;
    m_adc_shift = 0;
    m_eeprom.reset();
    m_cpu->reset();
}

uint32_t IoBoard::step()
{
    return m_cpu->step();
}

void IoBoard::log_unmapped(const char* what, uint16_t address)
{
    if (!m_logged_unmapped) {
        m_logged_unmapped = true;
        std::cerr << "[I/O board] WARNING: Z80 " << what << " at unmapped " << Hex{address, 4}
                  << " (further ones not logged)\n";
    }
}

// ---------------------------------------------------------------------------
// Z80 bus
// ---------------------------------------------------------------------------

uint8_t IoBoard::read(uint16_t address)
{
    if (address < k_rom_window) {
        return m_rom[address];
    }
    if (address >= k_ram_base && address < k_ram_base + k_ram_size) {
        return m_ram[address - k_ram_base];
    }
    if (address >= k_io_chip_base && address < k_io_chip_base + 0x10) {
        return io_chip_read(address - k_io_chip_base);
    }
    if (address >= k_adc_base && address < k_adc_base + 4) {
        // D0 = next bit of the latched sample, MSB first.
        const uint8_t bit = (m_adc_shift >> 7) & 1;
        m_adc_shift = static_cast<uint8_t>(m_adc_shift << 1);
        return bit;
    }
    log_unmapped("read", address);
    return 0xFF;
}

void IoBoard::write(uint16_t address, uint8_t value)
{
    if (address >= k_ram_base && address < k_ram_base + k_ram_size) {
        m_ram[address - k_ram_base] = value;
        return;
    }
    if (address >= k_io_chip_base && address < k_io_chip_base + 0x10) {
        io_chip_write(address - k_io_chip_base, value);
        return;
    }
    if (address >= k_adc_base && address < k_adc_base + 4) {
        // Select a channel and latch its value. Port A bit 0 switches the
        // ADC inputs to channels 4-7 (two 74HC4066 analog switches).
        m_adc_shift = m_inputs.analog(address - k_adc_base + (m_select_dip_switches ? 4 : 0));
        return;
    }
    log_unmapped(address < k_rom_window ? "write to ROM" : "write", address);
}

uint8_t IoBoard::in(uint16_t port)
{
    log_unmapped("I/O read", port);
    return 0xFF;
}

void IoBoard::out(uint16_t port, uint8_t)
{
    log_unmapped("I/O write", port);
}

// ---------------------------------------------------------------------------
// 315-5338A
// ---------------------------------------------------------------------------

uint8_t IoBoard::io_chip_read(uint32_t reg)
{
    if (reg < 7) {
        return (m_port_direction & (1u << reg)) != 0 ? port_input(reg) : m_port_value[reg];
    }
    switch (reg) {
    case 0x08: return m_port_direction;
    case 0x0A: return m_serial_output;
    case 0x0B: return m_command;
    case 0x0C: return m_shared_ram.board_read(m_address);
    case 0x0D: return 0x08; // status: transfer finished, command acknowledged
    default: return 0xFF;
    }
}

void IoBoard::io_chip_write(uint32_t reg, uint8_t value)
{
    if (reg < 7) {
        m_port_value[reg] = value;
        port_output(reg, value); // MAME drives the output even when set to input
        return;
    }
    switch (reg) {
    case 0x08: {
        // Ports switching from input to output drive their latched value.
        const uint8_t now_output = static_cast<uint8_t>((value ^ m_port_direction) & ~value);
        for (uint32_t port = 0; port < 7; ++port) {
            if ((now_output & (1u << port)) != 0) {
                port_output(port, m_port_value[port]);
            }
        }
        m_port_direction = value;
        return;
    }
    case 0x09:
        m_command = value;
        switch (value) {
        case 0x00: m_address = static_cast<uint16_t>((m_address & 0xFF00) | m_serial_output); return;
        case 0x01: m_address = static_cast<uint16_t>((m_address & 0x00FF) | (m_serial_output << 8)); return;
        case 0x07: m_shared_ram.board_write(m_address, m_serial_output); return;
        case 0x87: return; // sent before reading (no action needed)
        default:
            if (value >= 0x70 && value <= 0x77) {
                m_shared_ram.board_write(value & 0x07u, m_serial_output);
                return;
            }
            if (!m_logged_unknown_command) {
                m_logged_unknown_command = true;
                std::cerr << "[I/O board] WARNING: unknown 315-5338A command " << Hex{value, 2} << ", ignored\n";
            }
            return;
        }
    case 0x0A:
        m_serial_output = value;
        return;
    default:
        return;
    }
}

uint8_t IoBoard::port_input(uint32_t port)
{
    switch (port) {
    case 1: return m_select_dip_switches ? k_dip_switches : static_cast<uint8_t>(m_inputs.port_value(InputManager::Port::System));
    case 2: return m_select_dip_switches ? k_dip_switches : static_cast<uint8_t>(m_inputs.port_value(InputManager::Port::Player1));
    case 3: return m_select_dip_switches ? k_dip_switches : static_cast<uint8_t>(m_inputs.port_value(InputManager::Port::Player2));
    case 6:
        return static_cast<uint8_t>((m_eeprom.read_do(m_cpu->cycle_count()) ? 0x80 : 0) | 0x70 | k_board_buttons);
    default:
        return 0xFF; // A, F: outputs; E: drive board (not emulated)
    }
}

void IoBoard::port_output(uint32_t port, uint8_t value)
{
    if (port == 0) {
        // EEPROM lines in MAME's order: CLK, DI, CS.
        const uint64_t now = m_cpu->cycle_count();
        m_eeprom.write_clk((value & 0x80) != 0, now);
        m_eeprom.write_di((value & 0x20) != 0);
        m_eeprom.write_cs((value & 0x40) != 0, now);
        m_select_dip_switches = (value & 0x01) != 0;
    }
    // Ports E (drive board) and F (lamps) are just latched.
}

} // namespace model1

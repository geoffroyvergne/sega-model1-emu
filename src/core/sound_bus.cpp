#include "core/sound_bus.hpp"

#include "audio/multipcm.hpp"
#include "audio/ym3438.hpp"
#include "core/i8251.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <vector>

namespace model1 {

SoundBus::SoundBus(I8251& uart, MultiPCM& pcm1, MultiPCM& pcm2, Ym3438& ym)
    : m_uart(uart)
    , m_pcm1(pcm1)
    , m_pcm2(pcm2)
    , m_ym(ym)
{
    m_rom.fill(0xFF); // erased EPROM until loaded
}

void SoundBus::reset()
{
    m_ram.fill(0);
    for (ControlLatch& latch : m_control) {
        latch.bytes.fill(0);
        latch.logged.reset();
    }
}

bool SoundBus::is_ram(uint32_t address) const
{
    return address >= k_ram_base && address - k_ram_base < k_ram_size;
}

const uint8_t* SoundBus::memory_at(uint32_t address) const
{
    if (address < k_rom_size) {
        return &m_rom[address];
    }
    if (address >= k_rom_mirror_base && address - k_rom_mirror_base < k_rom_mirror_size) {
        return &m_rom[k_rom_mirror_offset + (address - k_rom_mirror_base)];
    }
    if (is_ram(address)) {
        return &m_ram[address - k_ram_base];
    }
    return nullptr;
}

// The UART, MultiPCMs and YM3438 are 8-bit devices on the low byte lane:
// on the big-endian 68000 that is the odd address of each word; even
// addresses are not connected.
bool SoundBus::device_read(uint32_t address, uint8_t& value)
{
    auto in = [address](uint32_t base, uint32_t size) { return address >= base && address - base < size; };
    const bool odd = (address & 1) != 0;
    value = 0;
    if (in(k_uart_base, k_uart_size)) {
        if (odd) {
            value = m_uart.read((address - k_uart_base) >> 1);
        }
        return true;
    }
    if (in(k_pcm1_base, k_pcm_size)) {
        value = odd ? m_pcm1.read((address - k_pcm1_base) >> 1) : 0;
        return true;
    }
    if (in(k_pcm2_base, k_pcm_size)) {
        value = odd ? m_pcm2.read((address - k_pcm2_base) >> 1) : 0;
        return true;
    }
    if (ControlLatch* latch = control_latch_at(address)) {
        value = latch->bytes[address - latch->base];
        return true;
    }
    if (in(k_ym_base, k_ym_size)) {
        value = odd ? m_ym.read((address - k_ym_base) >> 1) : 0;
        return true;
    }
    return in(k_pcm1_bank, 2) || in(k_pcm2_bank, 2);
}

bool SoundBus::device_write(uint32_t address, uint8_t value)
{
    auto in = [address](uint32_t base, uint32_t size) { return address >= base && address - base < size; };
    const bool odd = (address & 1) != 0;
    if (in(k_uart_base, k_uart_size)) {
        if (odd) {
            m_uart.write((address - k_uart_base) >> 1, value);
        }
        return true;
    }
    if (in(k_pcm1_base, k_pcm_size)) {
        if (odd) {
            m_pcm1.write((address - k_pcm1_base) >> 1, value);
        }
        return true;
    }
    if (in(k_pcm2_base, k_pcm_size)) {
        if (odd) {
            m_pcm2.write((address - k_pcm2_base) >> 1, value);
        }
        return true;
    }
    if (in(k_pcm1_bank, 2)) {
        if (odd) {
            m_pcm1.set_bank(value);
        }
        return true;
    }
    if (in(k_pcm2_bank, 2)) {
        if (odd) {
            m_pcm2.set_bank(value);
        }
        return true;
    }
    if (in(k_ym_base, k_ym_size)) {
        if (odd) {
            m_ym.write((address - k_ym_base) >> 1, value);
        }
        return true;
    }
    if (ControlLatch* latch = control_latch_at(address)) {
        const uint32_t offset = address - latch->base;
        if (!latch->logged.test(offset)) {
            latch->logged.set(offset);
            std::cerr << "[SoundBus] Sound control write8 at " << Hex{address} << " = " << Hex{value, 2}
                      << " (latched, no device emulated)\n";
        }
        latch->bytes[offset] = value;
        return true;
    }
    return false;
}

SoundBus::ControlLatch* SoundBus::control_latch_at(uint32_t address)
{
    for (ControlLatch& latch : m_control) {
        const uint32_t offset = address - latch.base; // wraps for addresses below base
        if (offset >= k_pcm_size && offset < k_control_window_size) {
            return &latch;
        }
    }
    return nullptr;
}

uint8_t SoundBus::control_latch(uint32_t address) const
{
    for (const ControlLatch& latch : m_control) {
        const uint32_t offset = (address & k_address_mask) - latch.base;
        if (offset >= k_pcm_size && offset < k_control_window_size) {
            return latch.bytes[offset];
        }
    }
    return 0;
}

uint8_t SoundBus::peek_byte(uint32_t address) const
{
    const uint8_t* p = memory_at(address & k_address_mask);
    return p != nullptr ? *p : 0;
}

uint8_t SoundBus::read_byte(uint32_t address)
{
    address &= k_address_mask;
    if (const uint8_t* p = memory_at(address)) {
        return *p;
    }
    if (uint8_t value = 0; device_read(address, value)) {
        return value;
    }
    if (m_logged_unmapped.insert(address).second) {
        std::cerr << "[SoundBus] CRITICAL: unmapped read8 at " << Hex{address} << ", returning 0 (logged once per address)\n";
    }
    return 0;
}

void SoundBus::write_byte(uint32_t address, uint8_t value)
{
    address &= k_address_mask;
    if (is_ram(address)) {
        m_ram[address - k_ram_base] = value;
        return;
    }
    if (memory_at(address) != nullptr) {
        std::cerr << "[SoundBus] WARNING: write8 to ROM at " << Hex{address} << " (value " << Hex{value, 2}
                  << "), ignored\n";
        return;
    }
    if (device_write(address, value)) {
        return;
    }
    if (m_logged_unmapped.insert(address).second) {
        std::cerr << "[SoundBus] CRITICAL: unmapped write8 at " << Hex{address} << " (value " << Hex{value, 2}
                  << "), ignored (logged once per address)\n";
    }
}

// Words and longs are big-endian: the byte at the lower address is the most
// significant.
uint16_t SoundBus::read_word(uint32_t address)
{
    return static_cast<uint16_t>((read_byte(address) << 8) | read_byte(address + 1));
}

uint32_t SoundBus::read_long(uint32_t address)
{
    return (static_cast<uint32_t>(read_word(address)) << 16) | read_word(address + 2);
}

void SoundBus::write_word(uint32_t address, uint16_t value)
{
    write_byte(address, static_cast<uint8_t>(value >> 8));
    write_byte(address + 1, static_cast<uint8_t>(value));
}

void SoundBus::write_long(uint32_t address, uint32_t value)
{
    write_word(address, static_cast<uint16_t>(value >> 16));
    write_word(address + 2, static_cast<uint16_t>(value));
}

bool SoundBus::load_rom(std::span<const uint8_t> data, uint32_t offset)
{
    if (offset > k_rom_size || data.size() > k_rom_size - offset) {
        std::cerr << "[SoundBus] load_rom: " << data.size() << " bytes do not fit at " << Hex{offset} << '\n';
        return false;
    }
    std::copy(data.begin(), data.end(), m_rom.begin() + offset);
    return true;
}

bool SoundBus::load_rom_file(const std::string& path)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > k_rom_size) {
        std::cerr << "[SoundBus] load_rom_file: cannot load '" << path << "'\n";
        return false;
    }
    std::vector<uint8_t> data(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
        std::cerr << "[SoundBus] load_rom_file: short read on '" << path << "'\n";
        return false;
    }
    return load_rom(data, 0);
}

} // namespace model1

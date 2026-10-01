#include "core/bus.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <utility>

namespace model1 {

namespace {

// True if [address, address + size) lies entirely inside [base, base + length).
// Written to avoid 32-bit overflow near the top of the address space.
constexpr bool in_range(uint32_t address, uint32_t size, uint32_t base, uint32_t length)
{
    return address >= base && (address - base) <= length - size;
}

// Little-endian packing, independent of host byte order.
uint32_t load_le(const uint8_t* src, uint32_t size)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; ++i) {
        value |= static_cast<uint32_t>(src[i]) << (8 * i);
    }
    return value;
}

void store_le(uint8_t* dst, uint32_t size, uint32_t value)
{
    for (uint32_t i = 0; i < size; ++i) {
        dst[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

} // namespace

Bus::Bus()
{
    // Erased EPROM contents until a ROM is loaded.
    m_program_rom.fill(0xFF);
    m_data_rom.fill(0xFF);
    m_boot_rom.fill(0xFF);

    m_regions = {
        {"program ROM",        k_program_rom_base,  k_data_window_base,  m_program_rom.data(),  false},
        {"data ROM window",    k_data_window_base,  k_data_bank_size,    m_data_rom.data(),     false},
        {"program ROM",        k_data_window_base + k_data_bank_size, k_program_rom_size - k_data_window_base - k_data_bank_size,
                               m_program_rom.data() + k_data_window_base + k_data_bank_size, false},
        {"work RAM A",         k_ram_a_base,        k_ram_a_size,        m_ram_a.data(),        true},
        {"work RAM B",         k_ram_b_base,        k_ram_b_size,        m_ram_b.data(),        true},
        {"display list RAM",   k_display_list_base, k_display_list_size, m_display_list.data(), true},
        {"tile RAM",           k_tile_ram_base,     k_tile_ram_size,     m_tile_ram.data(),     true},
        {"character RAM",      k_char_ram_base,     k_char_ram_size,     m_char_ram.data(),     true},
        {"palette RAM",        k_palette_base,      k_palette_size,      m_palette.data(),      true},
        {"colour translation", k_color_xlat_base,   k_color_xlat_size,   m_color_xlat.data(),   true},
        {"boot ROM",           k_boot_rom_base,     k_boot_rom_size,     m_boot_rom.data(),     false},
    };

    m_data_window_region = 1;
    std::cerr << "[Bus] Created (Model 1 memory map, " << m_regions.size() << " memory regions)\n";
}

void Bus::reset()
{
    set_data_bank(0);
    m_system_regs.fill(0);
    m_system_regs_logged.reset();
    for (MemoryRegion& region : m_regions) {
        if (region.writable) {
            std::fill_n(region.data, region.size, uint8_t{0});
        }
    }
}

bool Bus::is_work_ram(uint32_t address, uint32_t size) const
{
    return in_range(address, size, k_ram_a_base, k_ram_a_size)
        || in_range(address, size, k_ram_b_base, k_ram_b_size);
}

void Bus::map_io(std::string name, uint32_t base, uint32_t size, uint32_t offset_mask,
                 IoRead read, IoWrite write, IoWriteByte write_byte)
{
    m_io.push_back({std::move(name), base, size, offset_mask, std::move(read), std::move(write),
                    std::move(write_byte)});
}

void Bus::map_io_space(std::string name, uint32_t base, uint32_t size, uint32_t offset_mask,
                       IoRead read, IoWrite write, IoWriteByte write_byte)
{
    m_io_space.push_back({std::move(name), base, size, offset_mask, std::move(read), std::move(write),
                          std::move(write_byte)});
}

// ---------------------------------------------------------------------------
// V60 I/O address space
// ---------------------------------------------------------------------------

uint32_t Bus::io_space_read(uint32_t address, uint32_t size)
{
    auto find = [this](uint32_t a) -> IoMapping* {
        for (IoMapping& io : m_io_space) {
            if (in_range(a, 1, io.base, io.size)) {
                return &io;
            }
        }
        return nullptr;
    };
    IoMapping* io = find(address);
    if (io == nullptr || !io->read || (size > 1 && (address & 1) != 0)) {
        std::cerr << "[Bus] CRITICAL: unmapped I/O-space read" << (size * 8) << " at " << Hex{address}
                  << ", returning 0\n";
        return 0;
    }
    if (size == 1) {
        const uint16_t word = io->read(((address & ~1u) - io->base) & io->offset_mask);
        return (address & 1) != 0 ? static_cast<uint32_t>(word >> 8) : static_cast<uint32_t>(word & 0xFF);
    }
    const uint32_t low = io->read((address - io->base) & io->offset_mask);
    if (size == 2) {
        return low;
    }
    IoMapping* high_io = find(address + 2);
    const uint32_t high = (high_io != nullptr && high_io->read)
        ? high_io->read((address + 2 - high_io->base) & high_io->offset_mask) : 0;
    return low | (high << 16);
}

void Bus::io_space_write(uint32_t address, uint32_t size, uint32_t value)
{
    auto find = [this](uint32_t a) -> IoMapping* {
        for (IoMapping& io : m_io_space) {
            if (in_range(a, 1, io.base, io.size)) {
                return &io;
            }
        }
        return nullptr;
    };
    IoMapping* io = find(address);
    const bool usable = io != nullptr
        && (size == 1 ? static_cast<bool>(io->write_byte) : (static_cast<bool>(io->write) && (address & 1) == 0));
    if (!usable) {
        std::cerr << "[Bus] CRITICAL: unmapped I/O-space write" << (size * 8) << " at " << Hex{address}
                  << " (value " << Hex{value} << "), ignored\n";
        return;
    }
    if (size == 1) {
        io->write_byte((address - io->base) & io->offset_mask, static_cast<uint8_t>(value));
        return;
    }
    io->write((address - io->base) & io->offset_mask, static_cast<uint16_t>(value));
    if (size == 4) {
        if (IoMapping* high_io = find(address + 2); high_io != nullptr && high_io->write) {
            high_io->write((address + 2 - high_io->base) & high_io->offset_mask, static_cast<uint16_t>(value >> 16));
        }
    }
}

// ---------------------------------------------------------------------------
// Address decoding
// ---------------------------------------------------------------------------

Bus::MemoryRegion* Bus::find_region(uint32_t address, uint32_t size)
{
    for (MemoryRegion& region : m_regions) {
        if (in_range(address, size, region.base, region.size)) {
            return &region;
        }
    }
    return nullptr;
}

Bus::IoMapping* Bus::find_io(uint32_t address)
{
    for (IoMapping& io : m_io) {
        if (in_range(address, 1, io.base, io.size)) {
            return &io;
        }
    }
    return nullptr;
}

bool Bus::in_system_registers(uint32_t address, uint32_t size)
{
    return in_range(address, size, k_system_regs_base, k_system_regs_size);
}

// Each byte's first write is logged, so the boot sequence's register
// programming is visible without flooding the log with per-frame writes.
void Bus::log_system_register_write(uint32_t address, uint32_t size, uint32_t value)
{
    bool first = false;
    for (uint32_t i = 0; i < size; ++i) {
        const uint32_t offset = address - k_system_regs_base + i;
        first = first || !m_system_regs_logged.test(offset);
        m_system_regs_logged.set(offset);
    }
    if (first) {
        std::cerr << "[Bus] System register write" << (size * 8) << " at " << Hex{address}
                  << " = " << Hex{value} << " (latched, no device emulated yet)\n";
    }
}

void Bus::log_unmapped_read(uint32_t address, uint32_t size)
{
    std::cerr << "[Bus] CRITICAL: unmapped read" << (size * 8) << " at "
              << Hex{address} << ", returning 0\n";
}

void Bus::log_unmapped_write(uint32_t address, uint32_t size, uint32_t value)
{
    std::cerr << "[Bus] CRITICAL: unmapped write" << (size * 8) << " at "
              << Hex{address} << " (value " << Hex{value} << "), ignored\n";
}

// ---------------------------------------------------------------------------
// Memory and I/O dispatch
// ---------------------------------------------------------------------------

uint16_t Bus::io_read_word(IoMapping& io, uint32_t address)
{
    if (!io.read) {
        log_unmapped_read(address, 2);
        return 0;
    }
    return io.read((address - io.base) & io.offset_mask);
}

void Bus::io_write_word(IoMapping& io, uint32_t address, uint16_t value)
{
    if (!io.write) {
        std::cerr << "[Bus] WARNING: write16 to read-only port '" << io.name << "' at "
                  << Hex{address} << " (value " << Hex{value, 4} << "), ignored\n";
        return;
    }
    io.write((address - io.base) & io.offset_mask, value);
}

uint32_t Bus::read_generic(uint32_t address, uint32_t size)
{
    if (MemoryRegion* region = find_region(address, size)) {
        return load_le(region->data + (address - region->base), size);
    }

    IoMapping* io = find_io(address);
    if (io == nullptr && size == 4 && find_io(address + 2) != nullptr) {
        // Latch low half, device high half: two 16-bit bus cycles.
        const uint32_t low = read_generic(address, 2);
        return low | (read_generic(address + 2, 2) << 16);
    }
    if (io == nullptr && in_system_registers(address, size)) {
        return load_le(m_system_regs.data() + (address - k_system_regs_base), size);
    }
    if (io == nullptr) {
        log_unmapped_read(address, size);
        return 0;
    }
    if ((address & 1) != 0 && size != 1) {
        std::cerr << "[Bus] WARNING: misaligned read" << (size * 8) << " from port '" << io->name
                  << "' at " << Hex{address} << ", returning 0\n";
        return 0;
    }

    switch (size) {
    case 1: {
        // 16-bit port: read the containing word, return the addressed byte.
        const uint16_t word = io_read_word(*io, address & ~1u);
        return (address & 1) != 0 ? static_cast<uint32_t>(word >> 8) : static_cast<uint32_t>(word & 0xFF);
    }
    case 2:
        return io_read_word(*io, address);
    default: {
        // Two bus cycles, low half first.
        const uint32_t low = io_read_word(*io, address);
        const uint32_t high = read_generic(address + 2, 2);
        return low | (high << 16);
    }
    }
}

void Bus::write_generic(uint32_t address, uint32_t size, uint32_t value)
{
    if (MemoryRegion* region = find_region(address, size)) {
        if (!region->writable) {
            std::cerr << "[Bus] WARNING: write" << (size * 8) << " to " << region->name << " at "
                      << Hex{address} << " (value " << Hex{value} << "), ignored\n";
            return;
        }
        store_le(region->data + (address - region->base), size, value);
        return;
    }

    IoMapping* io = find_io(address);
    if (io == nullptr && size == 4 && find_io(address + 2) != nullptr) {
        write_generic(address, 2, value & 0xFFFF);
        write_generic(address + 2, 2, value >> 16);
        return;
    }
    if (io == nullptr && in_system_registers(address, size)) {
        log_system_register_write(address, size, value);
        store_le(m_system_regs.data() + (address - k_system_regs_base), size, value);
        return;
    }
    if (io == nullptr) {
        log_unmapped_write(address, size, value);
        return;
    }
    if (size == 1 && io->write_byte) {
        io->write_byte((address - io->base) & io->offset_mask, static_cast<uint8_t>(value));
        return;
    }
    if (size == 1 || (address & 1) != 0) {
        // Byte writes need a device-specific handler (partial-word semantics
        // differ between devices), so refuse rather than guess.
        std::cerr << "[Bus] WARNING: unsupported write" << (size * 8) << " to port '" << io->name
                  << "' at " << Hex{address} << " (value " << Hex{value} << "), ignored\n";
        return;
    }

    if (size == 2) {
        io_write_word(*io, address, static_cast<uint16_t>(value));
        return;
    }
    // Two bus cycles, low half first.
    io_write_word(*io, address, static_cast<uint16_t>(value));
    write_generic(address + 2, 2, value >> 16);
}

// ---------------------------------------------------------------------------
// CPU-side accessors
// ---------------------------------------------------------------------------

uint8_t Bus::peek_byte(uint32_t address)
{
    if (MemoryRegion* region = find_region(address, 1)) {
        return region->data[address - region->base];
    }
    return 0;
}

uint8_t Bus::read_byte(uint32_t address)
{
    return static_cast<uint8_t>(read_generic(address, 1));
}

uint16_t Bus::read_word(uint32_t address)
{
    return static_cast<uint16_t>(read_generic(address, 2));
}

uint32_t Bus::read_long(uint32_t address)
{
    return read_generic(address, 4);
}

void Bus::write_byte(uint32_t address, uint8_t value)
{
    write_generic(address, 1, value);
}

void Bus::write_word(uint32_t address, uint16_t value)
{
    write_generic(address, 2, value);
}

void Bus::write_long(uint32_t address, uint32_t value)
{
    write_generic(address, 4, value);
}

// ---------------------------------------------------------------------------
// ROM loading
// ---------------------------------------------------------------------------

void Bus::set_data_bank(uint32_t bank)
{
    m_data_bank = bank % k_data_bank_count;
    m_regions[m_data_window_region].data = m_data_rom.data() + m_data_bank * k_data_bank_size;
}

bool Bus::load_rom_data(std::span<const uint8_t> data, uint32_t target_address)
{
    MemoryRegion* region = find_region(target_address, 1);
    if (region == nullptr || region->writable) {
        std::cerr << "[Bus] load_rom_data: target " << Hex{target_address} << " is not inside a ROM region\n";
        return false;
    }
    const uint32_t offset = target_address - region->base;
    if (data.size() > region->size - offset) {
        std::cerr << "[Bus] load_rom_data: " << data.size() << " bytes do not fit at " << Hex{target_address}
                  << " (" << region->name << ")\n";
        return false;
    }
    std::copy(data.begin(), data.end(), region->data + offset);
    return true;
}

bool Bus::load_data_rom(std::span<const uint8_t> data, uint32_t offset)
{
    if (offset > k_data_rom_size || data.size() > k_data_rom_size - offset) {
        std::cerr << "[Bus] load_data_rom: " << data.size() << " bytes do not fit at offset " << Hex{offset} << '\n';
        return false;
    }
    std::copy(data.begin(), data.end(), m_data_rom.begin() + offset);
    return true;
}

bool Bus::load_rom(const std::string& filepath, uint32_t target_address)
{
    MemoryRegion* region = find_region(target_address, 1);
    if (region == nullptr || region->writable) {
        std::cerr << "[Bus] load_rom: target " << Hex{target_address}
                  << " is not inside a ROM region\n";
        return false;
    }

    const std::filesystem::path path(filepath);
    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        std::cerr << "[Bus] load_rom: cannot stat '" << filepath << "': " << ec.message() << '\n';
        return false;
    }
    if (file_size == 0) {
        std::cerr << "[Bus] load_rom: '" << filepath << "' is empty\n";
        return false;
    }

    const uint32_t offset = target_address - region->base;
    const uint32_t space_left = region->size - offset;
    if (file_size > space_left) {
        std::cerr << "[Bus] load_rom: '" << filepath << "' is " << file_size << " bytes, only "
                  << space_left << " bytes fit at " << Hex{target_address} << " (" << region->name << ")\n";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "[Bus] load_rom: cannot open '" << filepath << "'\n";
        return false;
    }

    // Read into a staging buffer first so a failed read never leaves ROM
    // half-overwritten. Size was checked above, so the narrowing is safe.
    const auto byte_count = static_cast<std::streamsize>(file_size);
    std::vector<uint8_t> staging(static_cast<std::size_t>(file_size));
    file.read(reinterpret_cast<char*>(staging.data()), byte_count);
    if (file.gcount() != byte_count) {
        std::cerr << "[Bus] load_rom: short read on '" << filepath << "' (" << file.gcount()
                  << " of " << byte_count << " bytes)\n";
        return false;
    }
    std::copy(staging.begin(), staging.end(), region->data + offset);

    std::cerr << "[Bus] Loaded '" << filepath << "' (" << file_size << " bytes) at "
              << Hex{target_address} << " (" << region->name << ")\n";
    return true;
}

} // namespace model1

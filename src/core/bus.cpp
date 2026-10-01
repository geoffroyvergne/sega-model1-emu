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
    m_boot_rom.fill(0xFF);

    m_regions = {
        {"program ROM",        k_program_rom_base,  k_program_rom_size,  m_program_rom.data(),  false},
        {"work RAM A",         k_ram_a_base,        k_ram_a_size,        m_ram_a.data(),        true},
        {"work RAM B",         k_ram_b_base,        k_ram_b_size,        m_ram_b.data(),        true},
        {"display list RAM",   k_display_list_base, k_display_list_size, m_display_list.data(), true},
        {"debug framebuffer",  k_vram_base,         k_vram_size,         m_vram.data(),         true},
        {"palette RAM",        k_palette_base,      k_palette_size,      m_palette.data(),      true},
        {"colour translation", k_color_xlat_base,   k_color_xlat_size,   m_color_xlat.data(),   true},
        {"boot ROM",           k_boot_rom_base,     k_boot_rom_size,     m_boot_rom.data(),     false},
    };

    std::cerr << "[Bus] Created (Model 1 memory map, " << m_regions.size() << " memory regions)\n";
}

void Bus::reset()
{
    for (MemoryRegion& region : m_regions) {
        if (region.writable) {
            std::fill_n(region.data, region.size, uint8_t{0});
        }
    }
}

void Bus::map_io(std::string name, uint32_t base, uint32_t size, uint32_t offset_mask,
                 IoRead read, IoWrite write)
{
    m_io.push_back({std::move(name), base, size, offset_mask, std::move(read), std::move(write)});
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
        IoMapping* io_high = find_io(address + 2);
        const uint32_t high = io_high != nullptr ? io_read_word(*io_high, address + 2) : 0;
        if (io_high == nullptr) {
            log_unmapped_read(address + 2, 2);
        }
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
    if (io == nullptr) {
        log_unmapped_write(address, size, value);
        return;
    }
    if (size == 1 || (address & 1) != 0) {
        // No device needs byte or misaligned writes yet; partial-word
        // semantics are device-specific, so refuse rather than guess.
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
    if (IoMapping* io_high = find_io(address + 2)) {
        io_write_word(*io_high, address + 2, static_cast<uint16_t>(value >> 16));
    } else {
        log_unmapped_write(address + 2, 2, value >> 16);
    }
}

// ---------------------------------------------------------------------------
// CPU-side accessors
// ---------------------------------------------------------------------------

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

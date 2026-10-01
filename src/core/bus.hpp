#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace model1 {

// Central system bus for the V60's 24-bit address space (16-bit data bus).
//
// Every CPU-visible access goes through here and is routed either to a
// memory region (plain RAM/ROM buffer) or to an I/O handler registered by a
// device. Components never hold pointers into each other's memory.
//
// Byte order: the V60 is little-endian, so multi-byte memory accesses are
// assembled LSB-first, independent of the host CPU.
//
// I/O handlers are 16-bit, like the board's data bus: a 32-bit access to an
// I/O port is performed as two 16-bit accesses, low half (address) first,
// then high half (address + 2).
//
// Memory map (from the Sega Model 1 board, as documented in MAME):
//   0x000000 - 0x2FFFFF  Program ROM          3 MB    R/O (0x100000-0x1FFFFF is
//                                                     a banked data-ROM window,
//                                                     not banked here yet)
//   0x400000 - 0x40FFFF  Work RAM A           64 KB   R/W (battery-backed)
//   0x500000 - 0x53FFFF  Work RAM B           256 KB  R/W
//   0x600000 - 0x61FFFF  Display list RAM     128 KB  R/W (two 64 KB buffers)
//   0x800000 - 0x87FFFF  Debug framebuffer    512 KB  R/W  ** not real hardware:
//                                                     placed in an unused hole
//                                                     for the video test path
//   0x900000 - 0x903FFF  Palette RAM          16 KB   R/W
//   0x910000 - 0x91BFFF  Colour translation   48 KB   R/W
//   0xD00000 - 0xDDFFFF  TGP ports            I/O (registered by the TGP)
//   0xF80000 - 0xFFFFFF  Boot ROM             512 KB  R/O (reset vector here)
//   everything else      unmapped             reads return 0, writes ignored
//
// Unloaded ROM reads as 0xFF, like an erased EPROM.
class Bus {
public:
    static constexpr uint32_t k_address_mask = 0x00FFFFFF; // 24-bit bus

    static constexpr uint32_t k_program_rom_base = 0x000000;
    static constexpr uint32_t k_program_rom_size = 0x300000; // 3 MB
    static constexpr uint32_t k_ram_a_base       = 0x400000;
    static constexpr uint32_t k_ram_a_size       = 0x010000; // 64 KB
    static constexpr uint32_t k_ram_b_base       = 0x500000;
    static constexpr uint32_t k_ram_b_size       = 0x040000; // 256 KB
    static constexpr uint32_t k_display_list_base = 0x600000;
    static constexpr uint32_t k_display_list_size = 0x020000; // 128 KB
    static constexpr uint32_t k_vram_base        = 0x800000; // debug framebuffer
    static constexpr uint32_t k_vram_size        = 0x080000; // 512 KB
    static constexpr uint32_t k_palette_base     = 0x900000;
    static constexpr uint32_t k_palette_size     = 0x004000; // 16 KB
    static constexpr uint32_t k_color_xlat_base  = 0x910000;
    static constexpr uint32_t k_color_xlat_size  = 0x00C000; // 48 KB
    static constexpr uint32_t k_boot_rom_base    = 0xF80000;
    static constexpr uint32_t k_boot_rom_size    = 0x080000; // 512 KB

    // 16-bit I/O handlers. `offset` is relative to the mapping's base, after
    // the mirror mask is applied.
    using IoRead = std::function<uint16_t(uint32_t offset)>;
    using IoWrite = std::function<void(uint32_t offset, uint16_t value)>;

    Bus();

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    Bus(Bus&&) = delete;
    Bus& operator=(Bus&&) = delete;

    // Clears all RAM. ROM contents are preserved across resets.
    void reset();

    // Registers a device port covering [base, base + size). Accesses are
    // passed to the handlers with offset = (address - base) & offset_mask, so
    // a small register block can be mirrored over a larger range.
    // A null handler makes that direction log as unmapped.
    void map_io(std::string name, uint32_t base, uint32_t size, uint32_t offset_mask,
                IoRead read, IoWrite write);

    // --- CPU-side accessors (little-endian) ---------------------------------
    uint8_t  read_byte(uint32_t address);
    uint16_t read_word(uint32_t address);
    uint32_t read_long(uint32_t address);

    void write_byte(uint32_t address, uint8_t value);
    void write_word(uint32_t address, uint16_t value);
    void write_long(uint32_t address, uint32_t value);

    // Loads a raw binary file into ROM. `target_address` is a bus address
    // inside one of the ROM regions (program ROM or boot ROM). Fails without
    // touching ROM if the file cannot be read, is empty, or would not fit
    // between target_address and the end of that region.
    bool load_rom(const std::string& filepath, uint32_t target_address);

    // Read-only view of the debug framebuffer for the video layer.
    [[nodiscard]] std::span<const uint8_t> vram() const { return m_vram; }

private:
    struct MemoryRegion {
        const char* name;
        uint32_t    base;
        uint32_t    size;
        uint8_t*    data;
        bool        writable;
    };

    struct IoMapping {
        std::string name;
        uint32_t    base;
        uint32_t    size;
        uint32_t    offset_mask;
        IoRead      read;
        IoWrite     write;
    };

    // Region that contains all of [address, address + size), or null.
    MemoryRegion* find_region(uint32_t address, uint32_t size);
    // I/O mapping that contains `address`, or null.
    IoMapping* find_io(uint32_t address);

    uint32_t read_generic(uint32_t address, uint32_t size);
    void     write_generic(uint32_t address, uint32_t size, uint32_t value);
    uint16_t io_read_word(IoMapping& io, uint32_t address);
    void     io_write_word(IoMapping& io, uint32_t address, uint16_t value);

    void log_unmapped_read(uint32_t address, uint32_t size);
    void log_unmapped_write(uint32_t address, uint32_t size, uint32_t value);

    std::array<uint8_t, k_program_rom_size>  m_program_rom{};
    std::array<uint8_t, k_ram_a_size>        m_ram_a{};
    std::array<uint8_t, k_ram_b_size>        m_ram_b{};
    std::array<uint8_t, k_display_list_size> m_display_list{};
    std::array<uint8_t, k_vram_size>         m_vram{};
    std::array<uint8_t, k_palette_size>      m_palette{};
    std::array<uint8_t, k_color_xlat_size>   m_color_xlat{};
    std::array<uint8_t, k_boot_rom_size>     m_boot_rom{};

    std::vector<MemoryRegion> m_regions;
    std::vector<IoMapping>    m_io;
};

} // namespace model1

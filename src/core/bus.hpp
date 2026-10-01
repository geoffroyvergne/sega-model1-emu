#pragma once

#include <array>
#include <bitset>
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
//   0x000000 - 0x0FFFFF  Program ROM (ROMA)   1 MB    R/O
//   0x100000 - 0x1FFFFF  Data ROM window      1 MB    R/O, one of 8 banks of the
//                                                     8 MB data ROM (bank register
//                                                     at 0xE00004, see set_data_bank)
//   0x200000 - 0x2FFFFF  Program ROM (ROMX)   1 MB    R/O
//   0x400000 - 0x40FFFF  Work RAM A           64 KB   R/W (battery-backed)
//   0x500000 - 0x53FFFF  Work RAM B           256 KB  R/W
//   0x600000 - 0x61FFFF  Display list RAM     128 KB  R/W (two 64 KB buffers)
//   0x700000 - 0x70FFFF  Tile RAM             64 KB   R/W (tilemaps, scroll, windows)
//   0x720000 - 0x770001  Video sync registers I/O (write-only, registered by the Motherboard)
//   0x780000 - 0x7FFFFF  Character RAM        512 KB  R/W (8x8 4bpp tile graphics)
//   0x900000 - 0x903FFF  Palette RAM          16 KB   R/W
//   0x910000 - 0x91BFFF  Colour translation   48 KB   R/W
//   0xD00000 - 0xDDFFFF  TGP ports            I/O (registered by the TGP)
//   0xE00000 - 0xE00FFF  System registers     4 KB    R/W latch: interrupt controller,
//                                                     timers and control flags the boot
//                                                     code programs. Devices mapped here
//                                                     (bank register 0xE00004) take
//                                                     priority; other addresses read back
//                                                     what was written.
//   0xF80000 - 0xFFFFFF  Boot ROM             512 KB  R/O (reset vector here)
//   everything else      unmapped             reads return 0, writes ignored
//
// Unloaded ROM reads as 0xFF, like an erased EPROM.
class Bus {
public:
    static constexpr uint32_t k_address_mask = 0x00FFFFFF; // 24-bit bus

    static constexpr uint32_t k_program_rom_base = 0x000000;
    static constexpr uint32_t k_program_rom_size = 0x300000; // 3 MB span (middle MB = data window)
    static constexpr uint32_t k_data_window_base = 0x100000;
    static constexpr uint32_t k_data_bank_size   = 0x100000; // 1 MB
    static constexpr uint32_t k_data_bank_count  = 8;
    static constexpr uint32_t k_data_rom_size    = k_data_bank_size * k_data_bank_count; // 8 MB
    static constexpr uint32_t k_ram_a_base       = 0x400000;
    static constexpr uint32_t k_ram_a_size       = 0x010000; // 64 KB
    static constexpr uint32_t k_ram_b_base       = 0x500000;
    static constexpr uint32_t k_ram_b_size       = 0x040000; // 256 KB
    static constexpr uint32_t k_display_list_base = 0x600000;
    static constexpr uint32_t k_display_list_size = 0x020000; // 128 KB
    static constexpr uint32_t k_tile_ram_base    = 0x700000;
    static constexpr uint32_t k_tile_ram_size    = 0x010000; // 64 KB
    static constexpr uint32_t k_char_ram_base    = 0x780000;
    static constexpr uint32_t k_char_ram_size    = 0x080000; // 512 KB
    static constexpr uint32_t k_palette_base     = 0x900000;
    static constexpr uint32_t k_palette_size     = 0x004000; // 16 KB
    static constexpr uint32_t k_color_xlat_base  = 0x910000;
    static constexpr uint32_t k_color_xlat_size  = 0x00C000; // 48 KB
    static constexpr uint32_t k_system_regs_base = 0xE00000;
    static constexpr uint32_t k_system_regs_size = 0x001000; // 4 KB
    static constexpr uint32_t k_boot_rom_base    = 0xF80000;
    static constexpr uint32_t k_boot_rom_size    = 0x080000; // 512 KB

    // 16-bit I/O handlers. `offset` is relative to the mapping's base, after
    // the mirror mask is applied.
    using IoRead = std::function<uint16_t(uint32_t offset)>;
    using IoWrite = std::function<void(uint32_t offset, uint16_t value)>;
    // Optional byte-write handler, for 8-bit devices that programs access
    // with byte stores. `offset` is the byte offset within the mapping.
    using IoWriteByte = std::function<void(uint32_t offset, uint8_t value)>;

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
                IoRead read, IoWrite write, IoWriteByte write_byte = nullptr);

    // The V60 also has a separate I/O address space, reached only with its
    // IN / OUT instructions. On Model 1 only the TGP ports are wired there
    // (mirroring their memory-space addresses). Same handler rules as map_io.
    void map_io_space(std::string name, uint32_t base, uint32_t size, uint32_t offset_mask,
                      IoRead read, IoWrite write, IoWriteByte write_byte = nullptr);
    // I/O space accesses (size 1, 2 or 4 bytes; 32-bit = two 16-bit cycles,
    // low half first). Unmapped addresses read 0 and are logged.
    uint32_t io_space_read(uint32_t address, uint32_t size);
    void     io_space_write(uint32_t address, uint32_t size, uint32_t value);

    // --- CPU-side accessors (little-endian) ---------------------------------
    uint8_t  read_byte(uint32_t address);
    // Memory byte without side effects (for traces and debuggers): ROM/RAM
    // contents, 0 for device ports and unmapped addresses, nothing logged.
    [[nodiscard]] uint8_t peek_byte(uint32_t address);
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

    // Copies `data` into ROM at bus address `target_address` (program or boot
    // ROM; the data window writes into the current bank). Fails without
    // touching ROM if it does not fit inside one ROM region.
    bool load_rom_data(std::span<const uint8_t> data, uint32_t target_address);

    // Copies `data` into the banked data ROM at `offset` (0 - 8 MB).
    bool load_data_rom(std::span<const uint8_t> data, uint32_t offset);

    // Selects which megabyte of the data ROM appears at 0x100000 (0-7).
    void set_data_bank(uint32_t bank);
    [[nodiscard]] uint32_t data_bank() const { return m_data_bank; }

    // True if [address, address + size) lies entirely inside work RAM A or B,
    // where the program's stacks live. Used by the CPU's stack checks.
    [[nodiscard]] bool is_work_ram(uint32_t address, uint32_t size) const;

    // Read-only views of video memory for the tilemap renderer.
    [[nodiscard]] std::span<const uint8_t> tile_ram() const { return m_tile_ram; }
    [[nodiscard]] std::span<const uint8_t> char_ram() const { return m_char_ram; }
    [[nodiscard]] std::span<const uint8_t> palette_ram() const { return m_palette; }
    [[nodiscard]] std::span<const uint8_t> display_list_ram() const { return m_display_list; }
    [[nodiscard]] std::span<const uint8_t> color_xlat_ram() const { return m_color_xlat; }
    // System register latch (0xE00000-0xE00FFF), for tests and debugging.
    [[nodiscard]] std::span<const uint8_t> system_registers() const { return m_system_regs; }

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
        IoWriteByte write_byte;
    };

    // Region that contains all of [address, address + size), or null.
    MemoryRegion* find_region(uint32_t address, uint32_t size);
    // I/O mapping that contains `address`, or null.
    IoMapping* find_io(uint32_t address);

    uint32_t read_generic(uint32_t address, uint32_t size);
    void     write_generic(uint32_t address, uint32_t size, uint32_t value);
    uint16_t io_read_word(IoMapping& io, uint32_t address);
    void     io_write_word(IoMapping& io, uint32_t address, uint16_t value);

    // System register latch: the fallback after I/O mappings in its range.
    [[nodiscard]] static bool in_system_registers(uint32_t address, uint32_t size);
    void log_system_register_write(uint32_t address, uint32_t size, uint32_t value);

    void log_unmapped_read(uint32_t address, uint32_t size);
    void log_unmapped_write(uint32_t address, uint32_t size, uint32_t value);

    std::array<uint8_t, k_program_rom_size>  m_program_rom{};
    std::array<uint8_t, k_data_rom_size>     m_data_rom{};
    uint32_t m_data_bank = 0;
    std::size_t m_data_window_region = 0; // index in m_regions
    std::array<uint8_t, k_ram_a_size>        m_ram_a{};
    std::array<uint8_t, k_ram_b_size>        m_ram_b{};
    std::array<uint8_t, k_display_list_size> m_display_list{};
    std::array<uint8_t, k_tile_ram_size>     m_tile_ram{};
    std::array<uint8_t, k_char_ram_size>     m_char_ram{};
    std::array<uint8_t, k_palette_size>      m_palette{};
    std::array<uint8_t, k_color_xlat_size>   m_color_xlat{};
    std::array<uint8_t, k_boot_rom_size>     m_boot_rom{};
    std::array<uint8_t, k_system_regs_size>  m_system_regs{};
    std::bitset<k_system_regs_size>          m_system_regs_logged; // first write per byte is logged

    std::vector<MemoryRegion> m_regions;
    std::vector<IoMapping>    m_io;
    std::vector<IoMapping>    m_io_space; // V60 I/O address space
};

} // namespace model1

#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>

namespace model1 {

class I8251;
class MultiPCM;
class Ym3438;

// Address space of the Model 1 sound board's 68000 (24-bit addresses,
// 16-bit data bus, big-endian). Layout from MAME's segam1audio device:
//
//   0x000000 - 0x03FFFF  sound program ROM   256 KB  R/O
//   0x080000 - 0x09FFFF  mirror of ROM 0x20000-0x3FFFF (upper ROM socket)
//   0xC20000 - 0xC20003  i8251 UART (command link from the main board),
//                        8-bit device on the low byte lane: data at
//                        0xC20001, control/status at 0xC20003
//   0xC40000 - 0xC40007  MultiPCM #1: 8-bit ports on the low byte lane
//                        (data 0xC40001, slot 0xC40003, register 0xC40005)
//   0xC50000 - 0xC50001  MultiPCM #1 sample bank (value & 3)
//   0xC40008 - 0xC4FFFF  sound control latch #1: written by the program
//                        (e.g. a 24-bit value as bytes at 0xC40007/09/0B),
//                        no device emulated. Writes are stored and read
//                        back; the first write to each byte is logged.
//                        MAME ignores these addresses.
//   0xC60000 - 0xC60007  MultiPCM #2, same layout
//   0xC60008 - 0xC6FFFF  sound control latch #2, same behaviour
//   0xC70000 - 0xC70001  MultiPCM #2 sample bank
//   0xD00000 - 0xD00007  YM3438 FM chip, 8-bit ports on the low byte lane
//                        (0xD00001/3/5/7); timers only, see Ym3438
//   0xF00000 - 0xF0FFFF  work RAM 64 KB as in MAME (the real PCB fits 16 KB)
//   everything else      unmapped: reads 0, writes ignored, both logged
//
// Unloaded ROM reads as 0xFF, like an erased EPROM.
class SoundBus {
public:
    static constexpr uint32_t k_address_mask = 0x00FFFFFF;
    static constexpr uint32_t k_rom_size = 0x40000;
    static constexpr uint32_t k_rom_mirror_base = 0x080000;
    static constexpr uint32_t k_rom_mirror_size = 0x20000;
    static constexpr uint32_t k_rom_mirror_offset = 0x20000;
    static constexpr uint32_t k_uart_base = 0xC20000;
    static constexpr uint32_t k_uart_size = 4;
    static constexpr uint32_t k_pcm1_base = 0xC40000;
    static constexpr uint32_t k_pcm1_bank = 0xC50000;
    static constexpr uint32_t k_pcm2_base = 0xC60000;
    static constexpr uint32_t k_pcm2_bank = 0xC70000;
    static constexpr uint32_t k_pcm_size = 8;
    static constexpr uint32_t k_control_window_size = 0x10000; // each MultiPCM window: 8 ports + latch
    static constexpr uint32_t k_ym_base = 0xD00000;
    static constexpr uint32_t k_ym_size = 8;
    static constexpr uint32_t k_ram_base = 0xF00000;
    static constexpr uint32_t k_ram_size = 0x10000;

    SoundBus(I8251& uart, MultiPCM& pcm1, MultiPCM& pcm2, Ym3438& ym);

    SoundBus(const SoundBus&) = delete;
    SoundBus& operator=(const SoundBus&) = delete;

    // Clears RAM. ROM contents are preserved.
    void reset();

    uint8_t  read_byte(uint32_t address);
    // Memory byte without side effects: ROM/RAM contents, 0 elsewhere.
    [[nodiscard]] uint8_t peek_byte(uint32_t address) const;
    uint16_t read_word(uint32_t address);
    uint32_t read_long(uint32_t address);
    void     write_byte(uint32_t address, uint8_t value);
    void     write_word(uint32_t address, uint16_t value);
    void     write_long(uint32_t address, uint32_t value);

    // Copies `data` into the program ROM at `offset`; false (and nothing
    // copied) if it does not fit.
    bool load_rom(std::span<const uint8_t> data, uint32_t offset);
    // Loads a raw binary file into the program ROM at offset 0.
    bool load_rom_file(const std::string& path);

    // Latched value of a sound control register (0xC4xxxx / 0xC6xxxx
    // outside the MultiPCM ports), for tests and debugging.
    [[nodiscard]] uint8_t control_latch(uint32_t address) const;

private:
    // Pointer to the byte backing `address` in ROM or RAM, or null.
    [[nodiscard]] const uint8_t* memory_at(uint32_t address) const;
    [[nodiscard]] bool is_ram(uint32_t address) const;

    // Byte access to the 8-bit devices; returns false if `address` is not a
    // device register.
    bool device_read(uint32_t address, uint8_t& value);
    bool device_write(uint32_t address, uint8_t value);

    // One 64 KB window above a MultiPCM's ports: stores writes so the
    // program's register setup survives without a device behind it.
    struct ControlLatch {
        uint32_t base;
        std::array<uint8_t, k_control_window_size> bytes{};
        std::bitset<k_control_window_size> logged;
    };
    // The latch window containing `address` (outside the chip ports), or null.
    [[nodiscard]] ControlLatch* control_latch_at(uint32_t address);

    I8251& m_uart;
    MultiPCM& m_pcm1;
    MultiPCM& m_pcm2;
    Ym3438& m_ym;
    // Unmapped addresses already reported (VR's program writes 0xC10001,
    // unmapped in MAME too, many times per second).
    std::unordered_set<uint32_t> m_logged_unmapped;
    std::array<ControlLatch, 2> m_control{ControlLatch{k_pcm1_base, {}, {}}, ControlLatch{k_pcm2_base, {}, {}}};
    std::array<uint8_t, k_rom_size> m_rom{};
    std::array<uint8_t, k_ram_size> m_ram{};
};

} // namespace model1

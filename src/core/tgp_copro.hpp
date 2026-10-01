#pragma once

#include "core/mb86233.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace model1 {

// The Model 1 TGP coprocessor board at the hardware level, as in MAME
// (model1_m.cpp): an MB86233 DSP running the game's TGP program (Virtua
// Racing: 315-5573), fed by the V60 through 32-bit FIFOs. Emulated
// hardware: no host dependencies. Used when the program, table and data
// ROMs are loaded; otherwise the high-level Tgp stands in.
//
// DSP program space: 0x000-0x7FF program ROM (2K words).
// DSP data space:    0x000-0x0FF RAM, 0x100 input FIFO (read; empty -> the
//                    DSP stalls), 0x200-0x3FF RAM, 0x400 output FIFO (write).
// DSP I/O space:
//   0x00 / 0x08 / 0x10 / 0x18  copro RAM address register 0-3
//   0x01 / 0x09 / 0x11 / 0x19  copro RAM data through register 0-3; each
//                              access advances the address by 1, or by 4
//                              when address bit 18 is set (strided access)
//   0x20-0x23  sin / cos: write the angle base; read offset n = table at
//              base + n * 0x4000 (16-bit angle, quarter-wave table, signs)
//   0x24-0x27  atan: write the three operands (and index), read the angle
//   0x28-0x29  1/x: write x; read the table value with the exponent fixed
//   0x2A-0x2B  1/sqrt(x), likewise
//   0x2E       data ROM page (bits 15 and up of the word address)
//   0x8000-0xFFFF  data ROM words in the current page
// The tables are the 64K-word copro table ROM (opr14742 / opr14743),
// sin/cos at 0x0000, atan at 0x4000, 1/x at 0x8000, 1/sqrt(x) at 0xC000.
//
// V60 side (the same ports as the high-level Tgp):
//   0xD00000  copro RAM address (16-bit; bit 15 = auto-increment)
//   0xD20000  copro RAM data: low half, then high half (completes the access)
//   0xD80000  FIFO: write the low half, then the high half (pushes the word);
//             read pops a word (low half), the next read returns its high half
//   0xDC0000  FIFO status: 0xFFFF (as in MAME)
//
// FIFOs hold 16 words. On the board the V60 is halted while it writes a
// full input FIFO or reads an empty output FIFO; here the DSP is run on
// the spot until it can proceed (the same order of events). The DSP pauses
// while the output FIFO is full.
class TgpCopro final : private Mb86233Bus {
public:
    static constexpr std::size_t k_program_words = 0x800;
    static constexpr std::size_t k_table_words = 0x10000;
    static constexpr std::size_t k_data_rom_words = 0x80000; // 2 MB
    static constexpr std::size_t k_ram_words = 0x2000;
    static constexpr std::size_t k_fifo_depth = 16;
    // DSP instructions per V60 cycle: (40 MHz / 3) / 16 MHz = 5 / 6.
    static constexpr int64_t k_dsp_per_main_numerator = 5;
    static constexpr int64_t k_dsp_per_main_denominator = 6;

    TgpCopro();

    TgpCopro(const TgpCopro&) = delete;
    TgpCopro& operator=(const TgpCopro&) = delete;

    // ROM loading (little-endian 32-bit words). The board is active only
    // when all three are loaded.
    bool load_program(std::span<const uint8_t> bytes);
    bool load_tables(std::span<const uint8_t> bytes);
    bool load_data_rom(std::span<const uint8_t> bytes);
    [[nodiscard]] bool is_active() const { return m_has_program && m_has_tables && m_has_data; }

    void reset();

    // Runs the DSP for `main_cycles` V60 cycles' worth of time.
    void run(uint32_t main_cycles);

    // V60 ports (16-bit handlers, `offset` = byte offset in the port).
    uint16_t read_ram_address(uint32_t offset);
    void     write_ram_address(uint32_t offset, uint16_t value);
    uint16_t read_ram_data(uint32_t offset);
    void     write_ram_data(uint32_t offset, uint16_t value);
    uint16_t read_fifo(uint32_t offset);
    void     write_fifo(uint32_t offset, uint16_t value);
    uint16_t read_status(uint32_t offset);

    // Logs the next `count` FIFO transfers (V60 pushes and pops, with the
    // DSP's PC) to std::cerr, for debugging the V60 / TGP protocol.
    void set_trace(uint64_t count) { m_trace_remaining = count; }

    [[nodiscard]] Mb86233& dsp() { return *m_dsp; }
    [[nodiscard]] std::size_t input_fifo_size() const { return m_fifo_in.size(); }
    [[nodiscard]] std::size_t output_fifo_size() const { return m_fifo_out.size(); }
    [[nodiscard]] uint32_t ram_word(uint32_t index) const { return m_ram[index % k_ram_words]; }

private:
    // Mb86233Bus
    uint32_t program_read(uint16_t address) override;
    uint32_t data_read(uint16_t address) override;
    void     data_write(uint16_t address, uint32_t value) override;
    uint32_t io_read(uint16_t address) override;
    void     io_write(uint16_t address, uint32_t value) override;
    uint32_t rf_read(uint16_t) override { return 0; }
    void     rf_write(uint16_t, uint32_t) override {} // LEDs

    // Runs the DSP until `done()` or a limit; false if it could not make
    // progress (waiting for input that will never come).
    template <typename Done> bool run_until(Done done);
    [[nodiscard]] bool dsp_can_run() const { return m_fifo_out.size() < k_fifo_depth; }

    uint32_t sincos(uint32_t offset) const;
    uint32_t inverse(uint32_t offset) const;
    uint32_t inverse_sqrt(uint32_t offset) const;
    uint32_t atan() const;

    std::unique_ptr<Mb86233> m_dsp;

    std::vector<uint32_t> m_program = std::vector<uint32_t>(k_program_words, 0);
    std::vector<uint32_t> m_tables = std::vector<uint32_t>(k_table_words, 0);
    std::vector<uint32_t> m_data_rom = std::vector<uint32_t>(k_data_rom_words, 0);
    std::array<uint32_t, 0x100> m_dram0{};
    std::array<uint32_t, 0x200> m_dram1{};
    std::vector<uint32_t> m_ram = std::vector<uint32_t>(k_ram_words, 0);
    bool m_has_program = false;
    bool m_has_tables = false;
    bool m_has_data = false;

    std::deque<uint32_t> m_fifo_in;
    std::deque<uint32_t> m_fifo_out;

    // DSP-side units
    std::array<uint32_t, 4> m_ram_address{};
    uint32_t m_sincos_base = 0;
    uint32_t m_inv_base = 0;
    uint32_t m_isqrt_base = 0;
    std::array<uint32_t, 4> m_atan_base{};
    uint32_t m_data_base = 0;

    // V60-side latches
    uint16_t m_v60_ram_address = 0;
    std::array<uint16_t, 2> m_v60_ram_latch{};
    uint32_t m_v60_fifo_write = 0;
    uint32_t m_v60_fifo_read = 0;

    int64_t m_dsp_budget = 0; // in sixths of a DSP instruction
    uint64_t m_trace_remaining = 0;
    bool m_logged_deadlock = false;
};

} // namespace model1

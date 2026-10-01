#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace model1 {

// 93C46-style serial EEPROM, 64 x 16 bits (the Model 1 I/O board's 93C45,
// 128 bytes), driven bit by bit through CS / CLK / DI and read on DO.
// Behaviour follows MAME's eeprom_serial_93cxx device:
//
//   CS rising, then on CLK rising edges: a start bit (DI = 1, only accepted
//   while ready), 2 opcode bits and 6 address bits:
//     10 aaaaaa  READ   a dummy 0 on DO, then 16 data bits, MSB first,
//                       one per CLK rising edge
//     01 aaaaaa  WRITE  followed by 16 data bits
//     11 aaaaaa  ERASE  (word = 0xFFFF)
//     00 11xxxx  EWEN   enable writes      00 00xxxx  EWDS  disable writes
//     00 10xxxx  ERAL   erase all          00 01xxxx  WRAL  write all (ANDed
//                                                     with the current data)
//   CS falling ends any command. Writes and erases need EWEN first: the chip
//   powers up write-protected. While a write / erase is in progress, DO
//   reads 0 (busy) once CS is raised again, then 1 (ready). Outside of a
//   READ, DO floats high.
//
// Programming times (MAME's): write 1.75 ms, erase 1 ms, write / erase all
// 8 ms, measured in the owner's clock (`now` arguments, in ticks of
// `clock_hz`).
class Eeprom93c46 {
public:
    static constexpr uint32_t k_words = 64;

    explicit Eeprom93c46(uint32_t clock_hz);

    // Power-on: write-protected, waiting for CS. The contents are kept
    // (non-volatile).
    void reset();

    void set_contents(std::span<const uint16_t> words);
    [[nodiscard]] uint16_t word(uint32_t address) const { return m_data[address % k_words]; }

    // Pin writes, in the order the I/O board firmware's port write drives
    // them (CLK, DI, then CS). `now` is the current time in clock ticks.
    void write_clk(bool level, uint64_t now);
    void write_di(bool level) { m_di = level; }
    void write_cs(bool level, uint64_t now);
    [[nodiscard]] bool read_do(uint64_t now) const;

    [[nodiscard]] bool write_enabled() const { return !m_locked; }

private:
    enum class State { InReset, WaitForStartBit, WaitForCommand, ReadingData, WaitForData, WaitForCompletion };
    enum class Command { Read, Write, Erase, Lock, Unlock, WriteAll, EraseAll };

    void clock_rising(uint64_t now);
    void execute_command(uint64_t now);
    void execute_write(uint64_t now);
    [[nodiscard]] bool ready(uint64_t now) const { return now >= m_completion_time; }

    std::array<uint16_t, k_words> m_data{};
    uint64_t m_write_ticks;
    uint64_t m_erase_ticks;
    uint64_t m_all_ticks;

    State    m_state = State::InReset;
    Command  m_command = Command::Read;
    bool     m_cs = false;
    bool     m_clk = false;
    bool     m_di = false;
    bool     m_locked = true;
    uint32_t m_bits = 0;
    uint32_t m_accumulator = 0; // command + address bits
    uint32_t m_address = 0;
    uint32_t m_shift = 0;       // read / write data, MSB at bit 31 when reading
    uint64_t m_cs_rise_time = 0;
    uint64_t m_completion_time = 0;
};

} // namespace model1

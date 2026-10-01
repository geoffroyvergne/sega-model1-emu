#include "core/eeprom_93c46.hpp"

#include <algorithm>

namespace model1 {

Eeprom93c46::Eeprom93c46(uint32_t clock_hz)
    : m_write_ticks(static_cast<uint64_t>(clock_hz) * 1750 / 1'000'000)
    , m_erase_ticks(static_cast<uint64_t>(clock_hz) * 1000 / 1'000'000)
    , m_all_ticks(static_cast<uint64_t>(clock_hz) * 8000 / 1'000'000)
{
    m_data.fill(0xFFFF); // erased
}

void Eeprom93c46::reset()
{
    m_state = State::InReset;
    m_cs = m_clk = m_di = false;
    m_locked = true;
    m_bits = 0;
    m_accumulator = 0;
    m_shift = 0;
    m_cs_rise_time = 0;
    m_completion_time = 0;
}

void Eeprom93c46::set_contents(std::span<const uint16_t> words)
{
    m_data.fill(0xFFFF);
    std::copy_n(words.begin(), std::min<std::size_t>(words.size(), k_words), m_data.begin());
}

void Eeprom93c46::write_cs(bool level, uint64_t now)
{
    if (level == m_cs) {
        return;
    }
    m_cs = level;
    if (!level) {
        m_state = State::InReset; // ends any command; a started write completes on its own
        return;
    }
    if (m_state == State::InReset) {
        m_state = State::WaitForStartBit;
        m_cs_rise_time = now;
    }
}

void Eeprom93c46::write_clk(bool level, uint64_t now)
{
    if (level == m_clk) {
        return;
    }
    m_clk = level;
    if (level && m_cs) {
        clock_rising(now);
    }
}

bool Eeprom93c46::read_do(uint64_t now) const
{
    if (m_state == State::WaitForStartBit) {
        return ready(now);
    }
    if (m_state == State::ReadingData) {
        return (m_shift & 0x80000000u) != 0;
    }
    return true; // tristated, pulled up
}

void Eeprom93c46::clock_rising(uint64_t now)
{
    switch (m_state) {
    case State::WaitForStartBit:
        // A clock edge at the same instant as CS rising is ignored, as in MAME.
        if (m_di && ready(now) && now > m_cs_rise_time) {
            m_accumulator = 0;
            m_bits = 0;
            m_state = State::WaitForCommand;
        }
        return;
    case State::WaitForCommand:
        m_accumulator = (m_accumulator << 1) | (m_di ? 1u : 0u);
        if (++m_bits == 2 + 6) {
            execute_command(now);
        }
        return;
    case State::ReadingData: {
        // First edge: the addressed word replaces the dummy 0; later edges
        // shift it out (ones follow the 16 data bits).
        const uint32_t bit_index = m_bits++;
        if (bit_index == 0) {
            m_shift = static_cast<uint32_t>(m_data[m_address]) << 16;
        } else {
            m_shift = (m_shift << 1) | 1u;
        }
        return;
    }
    case State::WaitForData:
        m_shift = (m_shift << 1) | (m_di ? 1u : 0u);
        if (++m_bits == 16) {
            execute_write(now);
        }
        return;
    case State::InReset:
    case State::WaitForCompletion:
        return;
    }
}

void Eeprom93c46::execute_command(uint64_t now)
{
    m_address = m_accumulator & 0x3F;
    switch (m_accumulator >> 6) {
    case 0:
        switch (m_address >> 4) {
        case 0: m_locked = true; m_state = State::InReset; return;   // EWDS
        case 1: m_command = Command::WriteAll; break;               // WRAL
        case 2:                                                      // ERAL
            if (!m_locked) {
                m_data.fill(0xFFFF);
                m_completion_time = now + m_all_ticks;
                m_state = State::WaitForCompletion;
            } else {
                m_state = State::InReset;
            }
            return;
        default: m_locked = false; m_state = State::InReset; return; // EWEN
        }
        break;
    case 1: m_command = Command::Write; break;
    case 2: // READ: DO shows the dummy 0 until the first clock
        m_shift = 0;
        m_bits = 0;
        m_state = State::ReadingData;
        return;
    default: // ERASE
        if (!m_locked) {
            m_data[m_address] = 0xFFFF;
            m_completion_time = now + m_erase_ticks;
            m_state = State::WaitForCompletion;
        } else {
            m_state = State::InReset;
        }
        return;
    }
    // WRITE / WRAL: collect 16 data bits.
    m_shift = 0;
    m_bits = 0;
    m_state = State::WaitForData;
}

void Eeprom93c46::execute_write(uint64_t now)
{
    if (m_locked) {
        m_state = State::InReset;
        return;
    }
    const auto value = static_cast<uint16_t>(m_shift);
    if (m_command == Command::WriteAll) {
        for (uint16_t& word : m_data) {
            word &= value;
        }
        m_completion_time = now + m_all_ticks;
    } else {
        m_data[m_address] = value;
        m_completion_time = now + m_write_ticks;
    }
    m_state = State::WaitForCompletion;
}

} // namespace model1

#include "core/tgp_copro.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <iostream>

namespace model1 {

namespace {

// Little-endian 32-bit words from bytes; false if the size is wrong.
bool load_words(std::span<const uint8_t> bytes, std::vector<uint32_t>& words)
{
    if (bytes.size() != words.size() * 4) {
        return false;
    }
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<uint32_t>(bytes[i * 4]) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8)
                 | (static_cast<uint32_t>(bytes[i * 4 + 2]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 3]) << 24);
    }
    return true;
}

// Bound on DSP instructions run on the V60's behalf for one FIFO access
// (a few frames' worth): beyond it the DSP is waiting for data the V60
// has not sent - a deadlock on hardware.
constexpr int k_sync_step_limit = 1'000'000;

} // namespace

TgpCopro::TgpCopro()
    : m_dsp(std::make_unique<Mb86233>(static_cast<Mb86233Bus&>(*this)))
{
}

bool TgpCopro::load_program(std::span<const uint8_t> bytes)
{
    m_has_program = load_words(bytes, m_program);
    return m_has_program;
}

bool TgpCopro::load_tables(std::span<const uint8_t> bytes)
{
    m_has_tables = load_words(bytes, m_tables);
    return m_has_tables;
}

bool TgpCopro::load_data_rom(std::span<const uint8_t> bytes)
{
    m_has_data = load_words(bytes, m_data_rom);
    return m_has_data;
}

void TgpCopro::reset()
{
    m_dram0.fill(0);
    m_dram1.fill(0);
    std::fill(m_ram.begin(), m_ram.end(), 0u);
    m_fifo_in.clear();
    m_fifo_out.clear();
    m_ram_address.fill(0);
    m_sincos_base = m_inv_base = m_isqrt_base = m_data_base = 0;
    m_atan_base.fill(0);
    m_v60_ram_address = 0;
    m_v60_ram_latch.fill(0);
    m_v60_fifo_write = m_v60_fifo_read = 0;
    m_dsp_budget = 0;
    m_dsp->reset();
}

void TgpCopro::run(uint32_t main_cycles)
{
    m_dsp_budget += static_cast<int64_t>(main_cycles) * k_dsp_per_main_numerator;
    while (m_dsp_budget > 0) {
        if (!dsp_can_run()) {
            m_dsp_budget = 0; // halted until the V60 drains the output FIFO
            return;
        }
        const uint64_t stalls = m_dsp->stall_count();
        m_dsp_budget -= static_cast<int64_t>(m_dsp->step()) * k_dsp_per_main_denominator;
        if (m_dsp->stall_count() != stalls && m_fifo_in.empty()) {
            // Waiting for input: nothing changes until the V60 writes, and
            // a retried read has no side effects, so skip the idle time.
            m_dsp_budget = 0;
            return;
        }
    }
}

template <typename Done>
bool TgpCopro::run_until(Done done)
{
    for (int i = 0; i < k_sync_step_limit && !done(); ++i) {
        if (!dsp_can_run()) {
            return done();
        }
        const uint64_t stalls = m_dsp->stall_count();
        m_dsp->step();
        if (m_dsp->stall_count() != stalls && m_fifo_in.empty() && !done()) {
            break; // the DSP waits for input while the V60 waits for the DSP: a deadlock
        }
    }
    if (done()) {
        return true;
    }
    if (!m_logged_deadlock) {
        m_logged_deadlock = true;
        std::cerr << "[TGP] WARNING: the V60 waits on the TGP while the DSP waits for input (deadlock; "
                  << "with an old TGP program dump this is expected). DSP PC=" << Hex{m_dsp->pc(), 4}
                  << ", input FIFO " << m_fifo_in.size() << ", output FIFO " << m_fifo_out.size()
                  << "; the read returns 0 (further ones not logged)\n";
    }
    return false;
}

// ---------------------------------------------------------------------------
// V60 side
// ---------------------------------------------------------------------------

uint16_t TgpCopro::read_ram_address(uint32_t)
{
    return m_v60_ram_address;
}

void TgpCopro::write_ram_address(uint32_t, uint16_t value)
{
    m_v60_ram_address = value;
}

uint16_t TgpCopro::read_ram_data(uint32_t offset)
{
    const uint32_t word = m_ram[m_v60_ram_address & 0x1FFF];
    if ((offset & 2) == 0) {
        return static_cast<uint16_t>(word);
    }
    if ((m_v60_ram_address & 0x8000) != 0) {
        ++m_v60_ram_address;
    }
    return static_cast<uint16_t>(word >> 16);
}

void TgpCopro::write_ram_data(uint32_t offset, uint16_t value)
{
    m_v60_ram_latch[(offset >> 1) & 1] = value;
    if ((offset & 2) != 0) {
        m_ram[m_v60_ram_address & 0x1FFF] = m_v60_ram_latch[0] | (static_cast<uint32_t>(m_v60_ram_latch[1]) << 16);
        if ((m_v60_ram_address & 0x8000) != 0) {
            ++m_v60_ram_address;
        }
    }
}

uint16_t TgpCopro::read_fifo(uint32_t offset)
{
    if ((offset & 2) != 0) {
        return static_cast<uint16_t>(m_v60_fifo_read >> 16);
    }
    // The V60 would be halted until the DSP produces a word.
    if (m_fifo_out.empty() && !run_until([this] { return !m_fifo_out.empty(); })) {
        m_v60_fifo_read = 0;
        return 0;
    }
    m_v60_fifo_read = m_fifo_out.front();
    m_fifo_out.pop_front();
    if (m_trace_remaining > 0) {
        --m_trace_remaining;
        std::cerr << "[TGP trace] V60 <- " << Hex{m_v60_fifo_read} << " (DSP PC " << Hex{m_dsp->pc(), 4} << ")\n";
    }
    return static_cast<uint16_t>(m_v60_fifo_read);
}

void TgpCopro::write_fifo(uint32_t offset, uint16_t value)
{
    if ((offset & 2) == 0) {
        m_v60_fifo_write = (m_v60_fifo_write & 0xFFFF0000u) | value;
        return;
    }
    m_v60_fifo_write = (m_v60_fifo_write & 0x0000FFFFu) | (static_cast<uint32_t>(value) << 16);
    // The V60 would be halted while the input FIFO is full.
    if (m_fifo_in.size() >= k_fifo_depth) {
        run_until([this] { return m_fifo_in.size() < k_fifo_depth; });
    }
    m_fifo_in.push_back(m_v60_fifo_write);
    if (m_trace_remaining > 0) {
        --m_trace_remaining;
        std::cerr << "[TGP trace] V60 -> " << Hex{m_v60_fifo_write} << " (DSP PC " << Hex{m_dsp->pc(), 4}
                  << ", input FIFO " << m_fifo_in.size() << ")\n";
    }
}

uint16_t TgpCopro::read_status(uint32_t)
{
    return 0xFFFF;
}

// ---------------------------------------------------------------------------
// DSP side
// ---------------------------------------------------------------------------

uint32_t TgpCopro::program_read(uint16_t address)
{
    return m_program[address & (k_program_words - 1)];
}

uint32_t TgpCopro::data_read(uint16_t address)
{
    if (address < 0x100) {
        return m_dram0[address];
    }
    if (address == 0x100) {
        if (m_fifo_in.empty()) {
            m_dsp->stall();
            return 0;
        }
        const uint32_t value = m_fifo_in.front();
        m_fifo_in.pop_front();
        return value;
    }
    if (address >= 0x200 && address < 0x400) {
        return m_dram1[address - 0x200];
    }
    return 0;
}

void TgpCopro::data_write(uint16_t address, uint32_t value)
{
    if (address < 0x100) {
        m_dram0[address] = value;
    } else if (address >= 0x200 && address < 0x400) {
        m_dram1[address - 0x200] = value;
    } else if (address == 0x400) {
        m_fifo_out.push_back(value);
    }
}

uint32_t TgpCopro::io_read(uint16_t address)
{
    if (address >= 0x8000) { // data ROM window
        const uint32_t index = ((m_data_base & ~0x7FFFu) | (address & 0x7FFFu)) & (k_data_rom_words - 1);
        return m_data_rom[index];
    }
    if (address < 0x20) {
        uint32_t& ram_address = m_ram_address[(address >> 3) & 3];
        if ((address & 7) == 0) {
            return ram_address;
        }
        if ((address & 7) == 1) {
            const uint32_t value = m_ram[ram_address & 0x1FFF];
            ram_address += (ram_address & 0x40000) != 0 ? 4 : 1;
            return value;
        }
        return 0;
    }
    switch (address) {
    case 0x20: case 0x21: case 0x22: case 0x23: return sincos(address - 0x20u);
    case 0x24: case 0x25: case 0x26: case 0x27: return atan();
    case 0x28: case 0x29: return inverse(address - 0x28u);
    case 0x2A: case 0x2B: return inverse_sqrt(address - 0x2Au);
    default: return 0;
    }
}

void TgpCopro::io_write(uint16_t address, uint32_t value)
{
    if (address < 0x20) {
        uint32_t& ram_address = m_ram_address[(address >> 3) & 3];
        if ((address & 7) == 0) {
            ram_address = value;
        } else if ((address & 7) == 1) {
            m_ram[ram_address & 0x1FFF] = value;
            ram_address += (ram_address & 0x40000) != 0 ? 4 : 1;
        }
        return;
    }
    switch (address) {
    case 0x20: case 0x21: case 0x22: case 0x23: m_sincos_base = value; return;
    case 0x24: case 0x25: case 0x26: case 0x27: m_atan_base[address - 0x24u] = value; return;
    case 0x28: case 0x29: m_inv_base = value; return;
    case 0x2A: case 0x2B: m_isqrt_base = value; return;
    case 0x2E: m_data_base = value; return;
    default: return;
    }
}

// Table units, exactly as MAME's model1_m.cpp.

uint32_t TgpCopro::sincos(uint32_t offset) const
{
    const uint32_t angle = m_sincos_base + offset * 0x4000;
    uint32_t index = angle & 0x3FFF;
    if ((angle & 0x4000) != 0) {
        index = static_cast<uint32_t>(std::min(0x4000 - static_cast<int>(index), 0x3FFF));
    }
    uint32_t result = m_tables[index];
    if ((angle & 0x8000) != 0) {
        result ^= 0x80000000u;
    }
    return result;
}

uint32_t TgpCopro::inverse(uint32_t offset) const
{
    const uint32_t index = ((m_inv_base >> 9) & 0x3FFE) | (offset & 1);
    uint32_t result = m_tables[index | 0x8000];
    const auto base_exp = static_cast<uint8_t>(m_inv_base >> 23);
    const auto exp = static_cast<uint8_t>((result >> 23) + (0x7F - base_exp));
    result = (result & 0x807FFFFFu) | (static_cast<uint32_t>(exp) << 23);
    if ((m_inv_base & 0x80000000u) != 0) {
        result ^= 0x80000000u;
    }
    return result;
}

uint32_t TgpCopro::inverse_sqrt(uint32_t offset) const
{
    const uint32_t index = 0x2000 ^ (((m_isqrt_base >> 10) & 0x3FFE) | (offset & 1));
    uint32_t result = m_tables[index | 0xC000];
    const auto base_exp = static_cast<uint8_t>((m_isqrt_base >> 24) & 0x7F);
    const auto exp = static_cast<uint8_t>((result >> 23) + (0x3F - base_exp));
    result = (result & 0x807FFFFFu) | (static_cast<uint32_t>(exp) << 23);
    if ((offset & 1) == 0) {
        result &= 0x7FFFFFFFu;
    }
    return result;
}

uint32_t TgpCopro::atan() const
{
    uint32_t index = m_atan_base[3] & 0xFFFF;
    if ((index & 0xC000) != 0) {
        index = 0x3FFF;
    }
    uint32_t result = m_tables[index | 0x4000];

    // MAME's correction for a table bug (the hardware does something
    // equivalent, or later boards have fixed ROMs).
    const auto dt = static_cast<uint16_t>((result >> 16) + result);
    if ((dt & 0x001) != 0) {
        result -= (result & 0x00F) == 0x00E ? 0x00000001u : 0x00010000u;
    }
    if ((dt & 0x010) != 0) {
        result -= (result & 0x0F0) == 0x0E0 ? 0x00000010u : 0x00100000u;
    }
    if ((dt & 0x100) != 0) {
        result -= (result & 0xF00) == 0xE00 ? 0x00000100u : 0x01000000u;
    }

    const bool s0 = (m_atan_base[0] & 0x80000000u) != 0;
    const bool s1 = (m_atan_base[1] & 0x80000000u) != 0;
    const bool s2 = (m_atan_base[2] & 0x80000000u) != 0;
    if (s0 ^ s1 ^ s2) {
        result >>= 16;
    }
    if (s2) {
        result += 0x4000;
    }
    if ((s0 && !s2) || (s1 && s2)) {
        result += 0x8000;
    }
    return result & 0xFFFF;
}

} // namespace model1

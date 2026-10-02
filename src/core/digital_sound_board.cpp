#include "core/digital_sound_board.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <iostream>

namespace model1 {

DigitalSoundBoard::DigitalSoundBoard(double output_rate_hz)
    : m_output_rate(output_rate_hz)
    , m_uart(std::make_unique<I8251>("UART DSB"))
    , m_cpu(std::make_unique<Z80>(*this))
    , m_mpeg(k_mpeg_rom_size, 0xFF)
{
    m_program.fill(0xFF);
}

bool DigitalSoundBoard::load_program(std::span<const uint8_t> data)
{
    if (data.empty() || data.size() > k_program_rom_size) {
        std::cerr << "[DSB] ERROR: program of " << data.size() << " bytes does not fit the 128 KB EPROM\n";
        return false;
    }
    // Only the first 32 KB is in the Z80's address space.
    m_program.fill(0xFF);
    std::copy_n(data.begin(), std::min(data.size(), k_program_window), m_program.begin());
    m_has_program = true;
    return true;
}

bool DigitalSoundBoard::load_mpeg_rom(std::span<const uint8_t> data, std::size_t offset)
{
    if (offset > m_mpeg.size() || data.size() > m_mpeg.size() - offset) {
        return false;
    }
    std::copy(data.begin(), data.end(), m_mpeg.begin() + static_cast<std::ptrdiff_t>(offset));
    m_has_mpeg = true;
    return true;
}

void DigitalSoundBoard::reset()
{
    m_ram.fill(0);
    m_uart->reset();
    m_mp_start = m_mp_end = m_lp_start = m_lp_end = 0;
    m_start = m_end = 0;
    m_volume = 0x7F;
    m_pan = 0;
    m_state = 0;
    m_pos = 0;
    m_decoder.reset();
    m_decoded_pos = m_decoded_count = 0;
    m_source_rate = 32000;
    m_previous = m_next = {0, 0};
    m_phase = 0.0;
    m_cpu->reset();
}

uint32_t DigitalSoundBoard::step()
{
    m_cpu->set_int_line(m_uart->rx_ready());
    return m_cpu->step();
}

// ---------------------------------------------------------------------------
// Z80 bus
// ---------------------------------------------------------------------------

uint8_t DigitalSoundBoard::read(uint16_t address)
{
    return address < k_ram_base ? m_program[address] : m_ram[address - k_ram_base];
}

void DigitalSoundBoard::write(uint16_t address, uint8_t value)
{
    if (address >= k_ram_base) {
        m_ram[address - k_ram_base] = value;
    }
}

uint8_t DigitalSoundBoard::in(uint16_t port)
{
    const auto p = static_cast<uint8_t>(port);
    const uint32_t playing_byte = static_cast<uint32_t>(m_pos >> 3);
    switch (p) {
    case 0xE2: return static_cast<uint8_t>(playing_byte >> 16);
    case 0xE3: return static_cast<uint8_t>(playing_byte >> 8);
    case 0xE4: return static_cast<uint8_t>(playing_byte);
    case 0xF0: return m_uart->read(0);
    case 0xF1: return m_uart->read(1);
    default: break;
    }
    if (!m_logged_ports.test(p)) {
        m_logged_ports.set(p);
        std::cerr << "[DSB] WARNING: Z80 read from unmapped port " << Hex{p, 2} << " (logged once)\n";
    }
    return 0xFF;
}

void DigitalSoundBoard::out(uint16_t port, uint8_t value)
{
    const auto p = static_cast<uint8_t>(port);
    // Assembles a 24-bit address, high byte first.
    auto set_byte = [value](uint32_t& address, int index) {
        const int shift = 16 - 8 * index;
        address = (address & ~(0xFFu << shift)) | (uint32_t{value} << shift);
    };
    switch (p) {
    case 0xE0:
        trigger(value);
        return;
    case 0xE2:
    case 0xE3:
    case 0xE4:
        set_byte(m_start, p - 0xE2);
        if (p == 0xE4) {
            (m_state == 0 ? m_mp_start : m_lp_start) = m_start;
        }
        return;
    case 0xE5:
    case 0xE6:
    case 0xE7:
        set_byte(m_end, p - 0xE5);
        if (p == 0xE7) {
            (m_state == 0 ? m_mp_end : m_lp_end) = m_end;
        }
        return;
    case 0xE8:
        m_volume = static_cast<uint32_t>(~value & 0x7F);
        return;
    case 0xE9:
        m_pan = value & 3u;
        return;
    case 0xEA:
    case 0xEB:
        return; // unknown (see the header)
    case 0xF0:
        m_uart->write(0, value);
        return;
    case 0xF1:
        m_uart->write(1, value);
        return;
    default:
        break;
    }
    if (!m_logged_ports.test(p)) {
        m_logged_ports.set(p);
        std::cerr << "[DSB] WARNING: Z80 write " << Hex{value, 2} << " to unmapped port " << Hex{p, 2} << " (logged once)\n";
    }
}

void DigitalSoundBoard::trigger(uint8_t value)
{
    m_state = value;
    if (value == 0) {
        m_decoded_pos = m_decoded_count = 0;
    } else if (value == 1 || value == 2) {
        m_pos = uint64_t{m_mp_start} * 8;
    }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void DigitalSoundBoard::next_source_frame(int32_t& left, int32_t& right)
{
    for (;;) {
        if (m_decoded_pos < m_decoded_count) {
            const int32_t l = m_decoded[m_decoded_pos * 2];
            const int32_t r = m_decoded[m_decoded_pos * 2 + 1];
            const int32_t a = m_pan == 2 ? r : l;
            const int32_t b = m_pan == 1 ? l : r;
            left = a * static_cast<int32_t>(m_volume) / 128;
            right = b * static_cast<int32_t>(m_volume) / 128;
            ++m_decoded_pos;
            return;
        }
        if (m_state == 0) {
            left = right = 0;
            return;
        }
        Mp2Decoder::FrameInfo info;
        if (m_decoder.decode(m_mpeg, m_pos, uint64_t{m_mp_end} * 8, m_decoded, info)) {
            m_decoded_pos = 0;
            m_decoded_count = Mp2Decoder::k_samples_per_frame;
            m_source_rate = info.sample_rate;
            ++m_frames_decoded;
            continue;
        }
        // End of the stream: loop to the latched start, or stop.
        if (m_state == 2) {
            if (m_pos == uint64_t{m_lp_start} * 8) {
                m_state = 0; // nothing decodable at the loop start
            }
            m_pos = uint64_t{m_lp_start} * 8;
            if (m_lp_end != 0) {
                m_mp_end = m_lp_end;
            }
        } else {
            m_state = 0;
        }
    }
}

void DigitalSoundBoard::render(std::span<int16_t> out)
{
    const double step = static_cast<double>(m_source_rate) / m_output_rate;
    for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
        m_phase += step;
        while (m_phase >= 1.0) {
            m_phase -= 1.0;
            m_previous = m_next;
            next_source_frame(m_next[0], m_next[1]);
        }
        for (std::size_t c = 0; c < 2; ++c) {
            const double v = m_previous[c] + (m_next[c] - m_previous[c]) * m_phase;
            out[i + c] = static_cast<int16_t>(std::clamp(static_cast<int32_t>(v), -32768, 32767));
        }
    }
}

} // namespace model1

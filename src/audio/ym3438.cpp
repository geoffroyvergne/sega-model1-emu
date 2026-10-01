#include "audio/ym3438.hpp"

#include "core/log.hpp"

#include <iostream>
#include <utility>

namespace model1 {

Ym3438::Ym3438(std::string name)
    : m_name(std::move(name))
{
}

void Ym3438::reset()
{
    for (auto& part : m_regs) {
        part.fill(0);
    }
    m_address.fill(0);
    m_control = 0;
    m_status = 0;
    m_timer_a = 0;
    m_timer_b = 0;
    m_timer_b_prescaler = 0;
    m_clock_remainder = 0;
}

uint8_t Ym3438::read(uint32_t /*port*/) const
{
    return m_status; // busy (bit 7) never set: writes complete immediately
}

void Ym3438::write(uint32_t port, uint8_t value)
{
    const uint32_t part = (port >> 1) & 1;
    if ((port & 1) == 0) {
        m_address[part] = value;
        return;
    }
    write_register(part, m_address[part], value);
}

void Ym3438::write_register(uint32_t part, uint8_t reg, uint8_t value)
{
    m_regs[part][reg] = value;
    if (part == 0 && reg == 0x27) {
        write_timer_control(value);
        return;
    }
    // Key on/off (0x28) or channel / operator parameters: the sound program
    // is using FM, which is not synthesized yet.
    const bool fm_register = (part == 0 && reg == 0x28) || reg >= 0x30;
    if (fm_register && !m_logged_fm) {
        m_logged_fm = true;
        std::cerr << "[" << m_name << "] WARNING: FM synthesis not emulated (timers only), no FM audio\n";
    }
}

void Ym3438::write_timer_control(uint8_t value)
{
    // A timer reloads when it is started (run bit going from 0 to 1).
    if ((value & 0x01) != 0 && (m_control & 0x01) == 0) {
        m_timer_a = timer_a_value();
    }
    if ((value & 0x02) != 0 && (m_control & 0x02) == 0) {
        m_timer_b = m_regs[0][0x26];
        m_timer_b_prescaler = 0;
    }
    if ((value & 0x10) != 0) {
        m_status &= static_cast<uint8_t>(~k_status_timer_a);
    }
    if ((value & 0x20) != 0) {
        m_status &= static_cast<uint8_t>(~k_status_timer_b);
    }
    m_control = static_cast<uint8_t>(value & 0xCF); // reset bits are not stored
}

void Ym3438::clock(uint32_t clocks)
{
    m_clock_remainder += clocks;
    while (m_clock_remainder >= k_clocks_per_sample) {
        m_clock_remainder -= k_clocks_per_sample;
        sample_tick();
    }
}

void Ym3438::sample_tick()
{
    if ((m_control & 0x01) != 0 && ++m_timer_a >= 1024) {
        m_timer_a = timer_a_value();
        if ((m_control & 0x04) != 0) {
            m_status |= k_status_timer_a;
        }
    }
    if ((m_control & 0x02) != 0 && ++m_timer_b_prescaler >= k_timer_b_prescale) {
        m_timer_b_prescaler = 0;
        if (++m_timer_b >= 256) {
            m_timer_b = m_regs[0][0x26];
            if ((m_control & 0x08) != 0) {
                m_status |= k_status_timer_b;
            }
        }
    }
}

} // namespace model1

#include "core/interrupt_controller.hpp"

namespace model1 {

void InterruptController::reset()
{
    m_status = 0;
    m_mask = 0xFF;
    m_last = 0;
}

void InterruptController::raise(int level)
{
    if (!masked(level)) {
        m_status = static_cast<uint8_t>(m_status | (1u << level));
    }
}

uint8_t InterruptController::acknowledge()
{
    for (uint8_t level = 0; level < 8; ++level) {
        if ((m_status & (1u << level)) != 0) {
            m_last = level;
            break;
        }
    }
    return m_last;
}

void InterruptController::write_control(uint8_t value)
{
    if (value == 0x10) {
        m_status = 0;
    } else if (value == 0x20) {
        m_status = static_cast<uint8_t>(m_status & ~(1u << m_last));
    }
}

void GlueTimers::reset()
{
    m_mode = 0;
    for (int t = 0; t < 2; ++t) {
        m_period[t] = m_value[t] = 0;
        m_remaining[t] = 0;
    }
}

void GlueTimers::set_period(int timer, uint16_t value)
{
    m_period[timer] = value;
    m_remaining[timer] = k_tick_cycles * value;
}

uint16_t GlueTimers::read(uint32_t offset)
{
    if (offset == 6 || offset == 8) {
        const int t = offset == 6 ? 0 : 1;
        if (m_period[t] != 0) {
            m_value[t] = static_cast<uint16_t>(m_remaining[t] / k_tick_cycles);
        }
        return m_value[t];
    }
    return 0; // mode and periods are write-only
}

void GlueTimers::write(uint32_t offset, uint16_t value)
{
    switch (offset) {
    case 0: m_mode = value; break;
    case 2: set_period(0, value); break;
    case 4: set_period(1, value); break;
    default: break; // count registers: games write 0 at init, ignored
    }
}

void GlueTimers::write_byte(uint32_t offset, uint8_t value)
{
    // Little-endian bus: the even byte is the low half of the register.
    const uint32_t reg = offset & ~1u;
    const bool high = (offset & 1u) != 0;
    uint16_t current = 0;
    switch (reg) {
    case 0: current = m_mode; break;
    case 2: current = m_period[0]; break;
    case 4: current = m_period[1]; break;
    default: return;
    }
    current = high ? static_cast<uint16_t>((current & 0x00FF) | (value << 8))
                   : static_cast<uint16_t>((current & 0xFF00) | value);
    write(reg, current);
}

uint32_t GlueTimers::run(uint32_t cycles)
{
    uint32_t fired = 0;
    for (int t = 0; t < 2; ++t) {
        if (m_period[t] == 0) {
            continue;
        }
        uint64_t left = cycles;
        while (left >= m_remaining[t]) {
            left -= m_remaining[t];
            m_remaining[t] = k_tick_cycles * m_period[t];
            ++fired;
        }
        m_remaining[t] -= left;
    }
    return fired;
}

} // namespace model1

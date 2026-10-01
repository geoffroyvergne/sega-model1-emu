#include "core/i8251.hpp"

#include "core/log.hpp"

#include <iostream>
#include <utility>

namespace model1 {

I8251::I8251(std::string name)
    : m_name(std::move(name))
{
}

void I8251::reset()
{
    m_expecting_mode = true;
    m_sync_chars_expected = 0;
    m_mode = 0;
    m_command = 0;
    m_status = k_status_tx_empty | k_status_tx_ready;
    m_data_mask = 0xFF;
    m_character_ticks = 0;
    m_rx_data = 0;
    m_tx_holding = 0;
    m_tx_shift = 0;
    m_tx_shifting = false;
    m_tx_ticks_left = 0;
}

uint8_t I8251::read(uint32_t reg)
{
    if (reg == 0) {
        m_status &= static_cast<uint8_t>(~k_status_rx_ready);
        return m_rx_data;
    }
    return m_status; // DSR (bit 7) not connected: reads 0
}

void I8251::write(uint32_t reg, uint8_t value)
{
    if (reg == 0) {
        // Data: goes into the holding buffer; TxRDY clears until the
        // transmitter takes it.
        m_tx_holding = value;
        m_status &= static_cast<uint8_t>(~k_status_tx_ready);
        try_start_transmit();
        return;
    }
    if (m_expecting_mode) {
        write_mode(value);
    } else if (m_sync_chars_expected > 0) {
        --m_sync_chars_expected; // SYNC character, not emulated beyond accepting it
    } else {
        write_command(value);
    }
}

void I8251::write_mode(uint8_t mode)
{
    m_mode = mode;
    m_expecting_mode = false;

    const uint32_t factor_code = mode & 0x03;
    if (factor_code == 0) {
        m_character_ticks = 0;
        m_sync_chars_expected = (mode & 0x80) != 0 ? 1 : 2; // bit 7: single SYNC character
        return;
    }
    const uint32_t factor = factor_code == 1 ? 1 : (factor_code == 2 ? 16 : 64);
    const uint32_t data_bits = ((mode >> 2) & 0x03) + 5;
    const uint32_t parity_bits = (mode & 0x10) != 0 ? 1 : 0;
    // Stop bits in half-bit units: 00 = inhibit (treated as 1), 1, 1.5, 2.
    const uint32_t stop_codes[4] = {2, 2, 3, 4};
    const uint32_t stop_half_bits = stop_codes[(mode >> 6) & 0x03];
    const uint32_t half_bits = 2 * (1 + data_bits + parity_bits) + stop_half_bits;
    m_character_ticks = (half_bits * factor + 1) / 2;
    m_data_mask = static_cast<uint8_t>((1u << data_bits) - 1);
}

void I8251::write_command(uint8_t command)
{
    m_command = command;
    if ((command & 0x10) != 0) {
        m_status &= static_cast<uint8_t>(~(k_status_parity_error | k_status_overrun_error | k_status_framing_error));
    }
    if ((command & 0x40) != 0) {
        m_expecting_mode = true; // internal reset: back to the mode format
        return;
    }
    // Only warn once synchronous mode is actually used, not for the mode
    // byte of a software reset sequence.
    if ((m_mode & 0x03) == 0 && (tx_enabled() || rx_enabled()) && !m_logged_sync_mode) {
        m_logged_sync_mode = true;
        std::cerr << "[" << m_name << "] WARNING: synchronous mode (" << Hex{m_mode, 2}
                  << ") not emulated, transmitter idle\n";
    }
    try_start_transmit();
}

void I8251::try_start_transmit()
{
    // A byte waits in the holding buffer (TxRDY clear) until the transmitter
    // is enabled and the shift register is free.
    if (!tx_enabled() || m_tx_shifting || (m_status & k_status_tx_ready) != 0 || m_character_ticks == 0) {
        return;
    }
    m_tx_shift = m_tx_holding;
    m_tx_shifting = true;
    m_tx_ticks_left = m_character_ticks;
    m_status |= k_status_tx_ready;
    m_status &= static_cast<uint8_t>(~k_status_tx_empty);
}

void I8251::tick(uint32_t ticks)
{
    while (ticks > 0 && m_tx_shifting) {
        if (ticks < m_tx_ticks_left) {
            m_tx_ticks_left -= ticks;
            return;
        }
        ticks -= m_tx_ticks_left;
        m_tx_ticks_left = 0;
        m_tx_shifting = false;

        // Character complete: it arrives at the other end of the line.
        if (m_peer != nullptr) {
            m_peer->receive_character(static_cast<uint8_t>(m_tx_shift & m_data_mask));
        }
        try_start_transmit(); // next byte from the holding buffer, if any
        if (!m_tx_shifting) {
            m_status |= k_status_tx_empty;
        }
    }
}

void I8251::receive_character(uint8_t value)
{
    if (!rx_enabled()) {
        if (!m_logged_rx_disabled) {
            m_logged_rx_disabled = true;
            std::cerr << "[" << m_name << "] WARNING: byte " << Hex{value, 2}
                      << " arrived while the receiver is disabled, dropped\n";
        }
        return;
    }
    if ((m_status & k_status_rx_ready) != 0) {
        m_status |= k_status_overrun_error; // previous byte was never read
    }
    m_rx_data = value;
    m_status |= k_status_rx_ready;
}

} // namespace model1

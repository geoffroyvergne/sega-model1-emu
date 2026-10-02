#pragma once

#include <cstdint>
#include <string>

namespace model1 {

// Intel 8251 USART (and compatibles: NEC uPD71051, Toshiba T82C51), as used
// on Model 1 for the serial command link between the main board and the
// sound board. Behaviour follows MAME's i8251 device.
//
// Registers (selected by the C/D pin):
//   0  data      read: received byte (clears RxRDY); write: byte to send
//   1  control   write: mode byte after reset, then command bytes
//      status    read: bit 0 TxRDY, 1 RxRDY, 2 TxEMPTY, 3 parity error,
//                      4 overrun error, 5 framing error, 7 DSR
//
// Mode byte (asynchronous): bits 1-0 clock factor (01 = x1, 10 = x16,
// 11 = x64; 00 = synchronous, not emulated), bits 3-2 character length 5-8,
// bit 4 parity enable, bit 5 even parity, bits 7-6 stop bits (1, 1.5, 2).
// Command byte: bit 0 transmit enable, bit 2 receive enable, bit 4 reset
// error flags, bit 6 internal reset (next control write is a mode byte).
//
// Serial timing: tick() advances the UART clock (TxC/RxC). One character
// takes (start + data + parity + stop) bits x clock factor clock ticks; when
// it completes it is delivered to the connected peer's receiver, which takes
// it only while receive is enabled.
class I8251 {
public:
    static constexpr uint8_t k_status_tx_ready = 0x01;
    static constexpr uint8_t k_status_rx_ready = 0x02;
    static constexpr uint8_t k_status_tx_empty = 0x04;
    static constexpr uint8_t k_status_parity_error = 0x08;
    static constexpr uint8_t k_status_overrun_error = 0x10;
    static constexpr uint8_t k_status_framing_error = 0x20;

    explicit I8251(std::string name);

    I8251(const I8251&) = delete;
    I8251& operator=(const I8251&) = delete;

    void reset();

    // Wires this UART's TxD to `peer`'s RxD (one direction).
    void connect_transmitter_to(I8251& peer) { m_peer = &peer; }
    // Wires TxD to a second receiver as well: the line is shared (Star Wars
    // Arcade's sound board UART also feeds the Digital Sound Board).
    void add_second_receiver(I8251& peer) { m_second_peer = &peer; }

    // CPU interface. `reg` is the C/D pin: 0 = data, 1 = control/status.
    uint8_t read(uint32_t reg);
    void    write(uint32_t reg, uint8_t value);

    // Advances the serial clock by `ticks` TxC/RxC cycles.
    void tick(uint32_t ticks);

    // Output pins.
    [[nodiscard]] bool rx_ready() const { return (m_status & k_status_rx_ready) != 0; }
    [[nodiscard]] bool tx_ready() const { return tx_enabled() && (m_status & k_status_tx_ready) != 0; }
    [[nodiscard]] uint8_t status() const { return m_status; }

    // Called by the peer when a character finishes arriving on RxD.
    void receive_character(uint8_t value);

    // Clock ticks one character currently takes on the line (0 if the mode
    // has not been programmed or is synchronous).
    [[nodiscard]] uint32_t character_ticks() const { return m_character_ticks; }

private:
    void write_mode(uint8_t mode);
    void write_command(uint8_t command);
    void try_start_transmit();
    [[nodiscard]] bool tx_enabled() const { return (m_command & 0x01) != 0; }
    [[nodiscard]] bool rx_enabled() const { return (m_command & 0x04) != 0; }

    std::string m_name;
    I8251* m_peer = nullptr;
    I8251* m_second_peer = nullptr;

    bool    m_expecting_mode = true;
    // Synchronous mode: after the mode byte, the next 1 or 2 control writes
    // are SYNC characters, not commands. Programs rely on this in the usual
    // software reset (00 00 00 40: mode 00, two SYNC bytes, internal reset).
    uint32_t m_sync_chars_expected = 0;
    uint8_t m_mode = 0;
    uint8_t m_command = 0;
    uint8_t m_status = k_status_tx_empty | k_status_tx_ready;
    uint8_t m_data_mask = 0xFF;     // character length
    uint32_t m_character_ticks = 0; // line time of one character

    uint8_t m_rx_data = 0;
    uint8_t m_tx_holding = 0;       // written by the CPU, waiting to be sent
    uint8_t m_tx_shift = 0;         // being sent
    bool    m_tx_shifting = false;
    uint32_t m_tx_ticks_left = 0;

    bool m_logged_sync_mode = false;
    bool m_logged_rx_disabled = false;
};

} // namespace model1

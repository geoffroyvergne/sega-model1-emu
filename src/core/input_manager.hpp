#pragma once

#include <cstddef>
#include <cstdint>

namespace model1 {

// Player and cabinet inputs as the Model 1 I/O board presents them to the
// main CPU. Host-independent: the host layer reports presses/releases through
// set_input(); this class never sees SDL.
//
// On the real board an I/O PCB (Z80 + 315-5338A) samples the controls and
// copies them into a dual-port RAM at 0xC00000, one byte per 16-bit word.
// This class emulates that block at a high level, using the layout of
// MAME's former HLE I/O handler (MAME 0.190):
//
//   0xC00000 - 0xC0000F  analog channels 0-7 (no analog controls yet: 0)
//   0xC00010             system port   (IN.0)
//   0xC00012             player 1 port (IN.1)
//   0xC00014             player 2 port (IN.2)
//   0xC00016 - 0xC0001C  unused digital ports (0xFF)
//   0xC0001E             lamp / coin meter outputs (read back as written)
//   0xC00022             drive board command (write-only, ignored)
//
// Digital ports are active-low: a bit reads 0 while its input is held.
// Ports are 16 bits wide; the upper byte is unused and reads all ones, so an
// idle port reads 0xFFFF. Bit assignments (Virtua Fighter):
//   system : 0 coin 1, 1 coin 2, 2 test, 3 service, 4 start 1, 5 start 2
//   player : 0 button 1, 1 button 2, 2 button 3,
//            4 down, 5 up, 6 right, 7 left
class InputManager {
public:
    enum class Input {
        // System port
        Coin1, Coin2, Test, Service, Start1, Start2,
        // Player 1 port
        P1Button1, P1Button2, P1Button3, P1Down, P1Up, P1Right, P1Left,
        // Player 2 port
        P2Button1, P2Button2, P2Button3, P2Down, P2Up, P2Right, P2Left,
        Count
    };
    static constexpr std::size_t k_input_count = static_cast<std::size_t>(Input::Count);

    enum class Port { System, Player1, Player2 };

    static constexpr uint16_t k_idle_mask = 0xFFFF;
    static constexpr uint32_t k_io_block_size = 0x40; // 0xC00000 - 0xC0003F

    InputManager() = default;

    // Releases every input (power-on state).
    void reset();

    // Records a press (true) or release (false) of one input.
    void set_input(Input input, bool pressed);

    // Current active-low value of a port.
    [[nodiscard]] uint16_t port_value(Port port) const;

    // I/O block handlers (registered on the Bus by the Motherboard).
    // `offset` is the byte offset inside the 0xC00000 block.
    uint16_t read_io(uint32_t offset) const;
    void     write_io(uint32_t offset, uint16_t value);

    [[nodiscard]] static const char* input_name(Input input);

private:
    struct BitLocation {
        Port     port;
        uint16_t mask;
    };
    static BitLocation locate(Input input);

    uint16_t m_system  = k_idle_mask;
    uint16_t m_player1 = k_idle_mask;
    uint16_t m_player2 = k_idle_mask;
    uint16_t m_lamps   = 0;
};

} // namespace model1

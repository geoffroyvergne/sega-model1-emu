#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace model1 {

class DualPortRam;

// Player and cabinet inputs as the Model 1 I/O board presents them to the
// main CPU. Host-independent: the host layer reports presses/releases through
// set_input(); this class never sees SDL.
//
// On the real board an I/O PCB (Z80 running EPR-14869 + 315-5338A) samples
// the controls and copies them into the MB8421 dual-port RAM that the main
// CPU sees at 0xC00000 (one byte per 16-bit word, see DualPortRam). This
// class stands in for that firmware at a high level: publish() writes the
// ports into the shared RAM from the board side, using the layout of MAME's
// former HLE I/O handler (MAME 0.190). Byte index (V60 address):
//
//   0x00 - 0x07  (0xC00000 - 0xC0000E)  analog channels 0-7 (none yet: 0)
//   0x08         (0xC00010)             system port   (IN.0)
//   0x09         (0xC00012)             player 1 port (IN.1)
//   0x0A         (0xC00014)             player 2 port (IN.2)
//   0x0B - 0x0E  (0xC00016 - 0xC0001C)  unused digital ports (0xFF)
//   0x0F         (0xC0001E)             lamp / coin meter outputs, written by the game
//
//   0x20         (0xC00040)             command from the game; the board
//                                       clears it when done (VR posts 1 and
//                                       2 at boot and waits for 0)
//
// Commands are acknowledged (cleared) at the next service() call, once per
// frame, and otherwise ignored: what the real firmware does for them is not
// known (VR's pattern - command 1, read the 128-byte block at 0x100, write
// it, command 2 - suggests EEPROM load / save, which is not emulated).
//
// Everything else in the shared RAM belongs to the game / firmware protocol
// (e.g. VR's "SEGA" signature at 0x1A-0x1D) and is left alone. Whether the
// real EPR-14869 firmware uses this exact layout is not verified; emulating
// the board's Z80 would replace this class.
//
// Digital ports are active-low: a bit reads 0 while its input is held. The
// shared RAM carries the low byte of each port. Bit assignments (Virtua
// Fighter):
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
        P1Button4, P1Button5, P1Button6,
        // Player 2 port
        P2Button1, P2Button2, P2Button3, P2Down, P2Up, P2Right, P2Left,
        Count
    };
    static constexpr std::size_t k_input_count = static_cast<std::size_t>(Input::Count);

    enum class Port { System, Player1, Player2 };

    // Which game's control panel the inputs drive (set by the ROM loader).
    //   VirtuaFighter: two players, joystick + 3 buttons each (layout above).
    //   VirtuaRacing (MAME's "vr" ports):
    //     system  : 0 coin 1, 1 coin 2, 2 test, 3 service, 4 start,
    //               5-7 view buttons VR1-VR3 (red, blue, yellow)
    //     player 1: 0 view button VR4 (green), 4 shift down, 5 shift up
    //     analog  : 0 steering wheel (0x80 centre), 1 accelerator,
    //               2 brake (0x30 released, 0xFF fully pressed)
    //     Joystick left / right steer, up accelerates, down brakes;
    //     buttons 1-4 are VR1-VR4, 5 / 6 shift down / up.
    enum class Profile { VirtuaFighter, VirtuaRacing };
    static constexpr std::size_t k_analog_channels = 4;
    static constexpr uint8_t k_wheel_centre = 0x80;
    static constexpr uint8_t k_wheel_left = 0x00;
    static constexpr uint8_t k_wheel_right = 0xFF;
    static constexpr uint8_t k_pedal_released = 0x30;
    static constexpr int k_wheel_step = 0x10;  // per frame while steering / recentring
    static constexpr int k_pedal_step = 0x20;  // per frame while pressing / releasing

    static constexpr uint16_t k_idle_mask = 0xFFFF;
    // Shared RAM byte indexes (see above).
    static constexpr uint32_t k_analog_index = 0x00;
    static constexpr uint32_t k_analog_count = 8;
    static constexpr uint32_t k_system_index = 0x08;
    static constexpr uint32_t k_player1_index = 0x09;
    static constexpr uint32_t k_player2_index = 0x0A;
    static constexpr uint32_t k_unused_first = 0x0B;
    static constexpr uint32_t k_unused_last = 0x0E;
    static constexpr uint32_t k_lamp_index = 0x0F;
    static constexpr uint32_t k_command_index = 0x20;

    explicit InputManager(DualPortRam& shared_ram);

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Releases every input (power-on state) and publishes the idle ports.
    void reset();

    void set_profile(Profile profile);
    [[nodiscard]] Profile profile() const { return m_profile; }

    // Analog channel value (ADC channels 0-3), and the once-per-frame
    // update that moves the wheel and pedals toward their held positions.
    [[nodiscard]] uint8_t analog(std::size_t channel) const { return m_analog[channel % k_analog_channels]; }
    void update_analog();

    // Records a press (true) or release (false) of one input.
    void set_input(Input input, bool pressed);

    // Current active-low value of a port.
    [[nodiscard]] uint16_t port_value(Port port) const;

    // Writes the analog and digital ports into the shared RAM, as the I/O
    // board firmware does continuously (called on every change and once per
    // frame by the Motherboard). Does nothing while publishing is off: when
    // the real I/O board firmware runs (IoBoard), it owns the shared RAM and
    // only reads the port values from here.
    void publish();
    void set_publishing(bool enabled) { m_publishing = enabled; }

    // The board's periodic work, called once per frame by the Motherboard:
    // acknowledges a pending command, then refreshes the ports.
    void service();

    // Lamp / coin meter outputs as last written by the game.
    [[nodiscard]] uint8_t lamps() const;

    [[nodiscard]] static const char* input_name(Input input);

private:
    struct BitLocation {
        Port     port;
        uint16_t mask;
    };
    [[nodiscard]] BitLocation locate(Input input) const;
    void reset_analog();

    DualPortRam& m_shared_ram;
    bool m_publishing = true;
    std::bitset<256> m_logged_commands;
    uint16_t m_system  = k_idle_mask;
    uint16_t m_player1 = k_idle_mask;
    uint16_t m_player2 = k_idle_mask;
    Profile m_profile = Profile::VirtuaFighter;
    std::array<uint8_t, k_analog_channels> m_analog{};
    bool m_steer_left = false, m_steer_right = false, m_accelerate = false, m_brake = false;
};

} // namespace model1

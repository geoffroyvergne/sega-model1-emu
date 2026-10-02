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
    //     The keyboard wheel behaves like the cabinet's: while a pedal is
    //     pressed (racing) it turns quickly and springs back to centre on
    //     release; with no pedal pressed (menus) it turns slowly and stays
    //     where it is left. The game picks the course from the wheel's
    //     position (centre = course 1, turned right = courses 2 and 3), so
    //     a self-centring wheel would always fall back to course 1. The
    //     spring starts k_spring_delay_frames after a pedal goes down, so
    //     the wheel holds still while the game reads the confirmation.
    //   StarWars (MAME's "swa" ports):
    //     system  : 0 coin 1, 1 coin 2, 2 test, 3 service, 4 start 1, 5 start 2
    //     player 1: 0 P1 trigger, 1 P1 button 2, 2 P2 trigger, 3 P2 button 2,
    //               4 P1 button 3
    //     analog  : 0 stick 1 X (227 left, 27 right), 1 stick 1 Y (27 up,
    //               227 down), 2 throttle (200 idle, 28 full), 4 / 5 stick 2
    //               X / Y; 0x7F centre. Channels 4-7 are read with port A
    //               bit 0 set (the same switch that selects the DIP switches).
    //     The keyboard sticks self-centre; the throttle is a lever that
    //     stays where it is left (P1 buttons 5 / 6 open / close it).
    enum class Profile { VirtuaFighter, VirtuaRacing, StarWars };
    static constexpr std::size_t k_analog_channels = 8;
    static constexpr uint8_t k_stick_centre = 0x7F;
    static constexpr uint8_t k_stick_min = 27;
    static constexpr uint8_t k_stick_max = 227;
    static constexpr int k_stick_step = 0x10;
    static constexpr uint8_t k_throttle_idle = 200;
    static constexpr uint8_t k_throttle_full = 28;
    static constexpr int k_throttle_step = 4;

    // Analog host controls (game controller sticks and triggers), on top of
    // the keys. Sticks run -1 (left / up) .. +1 (right / down), pedals
    // (triggers) 0 .. 1; set_axis() applies the dead zones. How each game
    // uses them:
    //   VirtuaRacing  Stick1X steers. While racing (a pedal held, as for
    //                 the keys) the wheel follows the stick directly; in
    //                 menus a stick pushed past halfway acts like the arrow
    //                 keys (the wheel moves slowly and stays), so a course
    //                 stays selected when the stick is let go.
    //                 Pedal1 / Pedal2: accelerator / brake, proportional.
    //   StarWars      Stick1 / Stick2: the pilot's / gunner's flight sticks,
    //                 proportional over MAME's range. Pedal1 / Pedal2 move
    //                 the throttle lever toward fast / slow, faster the
    //                 harder they are pressed; it stays where it is left.
    //   VirtuaFighter none (the host turns sticks into joystick directions).
    // A centred stick or released pedal leaves the keys in control.
    enum class Axis { Stick1X, Stick1Y, Stick2X, Stick2Y, Pedal1, Pedal2, Count };
    static constexpr std::size_t k_axis_count = static_cast<std::size_t>(Axis::Count);
    static constexpr float k_stick_dead_zone = 0.15f;
    static constexpr float k_pedal_dead_zone = 0.05f;
    static constexpr float k_pedal_held = 0.25f;  // VR: a pedal counts as held (racing) past this
    static constexpr float k_menu_steer = 0.5f;   // VR menus: stick deflection that acts as a key
    static constexpr uint8_t k_wheel_centre = 0x80;
    static constexpr uint8_t k_wheel_left = 0x00;
    static constexpr uint8_t k_wheel_right = 0xFF;
    static constexpr uint8_t k_pedal_released = 0x30;
    static constexpr int k_wheel_step = 0x10;  // per frame while steering / recentring (pedal pressed)
    static constexpr int k_wheel_menu_step = 0x04;   // per frame with no pedal pressed: centre to lock in ~0.5 s
    static constexpr int k_spring_delay_frames = 20; // pedal held this long before the wheel self-centres
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
    // Board status byte next to the command byte: 0x40 = ready (MAME's old
    // high-level driver returned 0x40 for 0xC00042). Virtua Fighter waits
    // for it at boot.
    static constexpr uint32_t k_status_index = 0x21;
    static constexpr uint8_t k_status_ready = 0x40;

    explicit InputManager(DualPortRam& shared_ram);

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Releases every input (power-on state) and publishes the idle ports.
    void reset();

    void set_profile(Profile profile);
    [[nodiscard]] Profile profile() const { return m_profile; }

    // Analog channel value (channels 0-7; 4-7 are 0xFF unless wired), and the once-per-frame
    // update that moves the wheel and pedals toward their held positions.
    [[nodiscard]] uint8_t analog(std::size_t channel) const { return m_analog[channel % k_analog_channels]; }
    void update_analog();

    // Records a press (true) or release (false) of one input.
    void set_input(Input input, bool pressed);

    // Sets an analog host control (see Axis); takes effect at the next
    // update_analog(). Values outside the range are clamped.
    void set_axis(Axis axis, float value);
    [[nodiscard]] float axis_value(Axis axis) const { return m_axes[static_cast<std::size_t>(axis)]; }

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
    bool m_p2_left = false, m_p2_right = false, m_p2_up = false, m_p2_down = false;
    bool m_throttle_up = false, m_throttle_down = false;
    std::array<float, k_axis_count> m_axes{};         // after the dead zones
    std::array<uint8_t, 2> m_key_pedals{};            // VR: accelerator / brake as the keys move them
    int m_pedal_frames = 0; // consecutive frames with a pedal key held
};

} // namespace model1

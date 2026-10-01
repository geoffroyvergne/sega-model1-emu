#include "core/input_manager.hpp"

#include "core/dual_port_ram.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <iostream>

namespace model1 {

InputManager::InputManager(DualPortRam& shared_ram)
    : m_shared_ram(shared_ram)
{
}

void InputManager::reset()
{
    m_system = k_idle_mask;
    m_player1 = k_idle_mask;
    m_player2 = k_idle_mask;
    m_steer_left = m_steer_right = m_accelerate = m_brake = false;
    reset_analog();
    publish();
}

void InputManager::set_profile(Profile profile)
{
    m_profile = profile;
    reset();
}

void InputManager::reset_analog()
{
    if (m_profile == Profile::VirtuaRacing) {
        m_analog = {k_wheel_centre, k_pedal_released, k_pedal_released, 0xFF};
    } else {
        m_analog = {0xFF, 0xFF, 0xFF, 0xFF};
    }
}

namespace {
// Moves `value` toward `target` by at most `step`.
uint8_t approach(uint8_t value, int target, int step)
{
    const int v = value;
    if (v < target) return static_cast<uint8_t>(std::min(v + step, target));
    if (v > target) return static_cast<uint8_t>(std::max(v - step, target));
    return value;
}
} // namespace

void InputManager::update_analog()
{
    if (m_profile != Profile::VirtuaRacing) {
        return;
    }
    // Wheel: a paddle as in MAME, lower values to the left.
    int wheel_target = k_wheel_centre;
    if (m_steer_left != m_steer_right) {
        wheel_target = m_steer_left ? k_wheel_left : k_wheel_right;
    }
    m_analog[0] = approach(m_analog[0], wheel_target, k_wheel_step);
    m_analog[1] = approach(m_analog[1], m_accelerate ? 0xFF : k_pedal_released, k_pedal_step);
    m_analog[2] = approach(m_analog[2], m_brake ? 0xFF : k_pedal_released, k_pedal_step);
    publish();
}

void InputManager::publish()
{
    if (!m_publishing) {
        return;
    }
    // The firmware publishes the four ADC channels twice (0-3 and 4-7).
    for (uint32_t i = 0; i < k_analog_count; ++i) {
        m_shared_ram.board_write(k_analog_index + i, m_analog[i % k_analog_channels]);
    }
    m_shared_ram.board_write(k_system_index, static_cast<uint8_t>(m_system));
    m_shared_ram.board_write(k_player1_index, static_cast<uint8_t>(m_player1));
    m_shared_ram.board_write(k_player2_index, static_cast<uint8_t>(m_player2));
    for (uint32_t i = k_unused_first; i <= k_unused_last; ++i) {
        m_shared_ram.board_write(i, 0xFF);
    }
}

void InputManager::service()
{
    const uint8_t command = m_shared_ram.board_read(k_command_index);
    if (command != 0) {
        if (!m_logged_commands.test(command)) {
            m_logged_commands.set(command);
            std::cerr << "[I/O board] Command " << Hex{command, 2}
                      << " acknowledged (board firmware not emulated, no action taken)\n";
        }
        m_shared_ram.board_write(k_command_index, 0);
    }
    publish();
}

uint8_t InputManager::lamps() const
{
    return m_shared_ram.board_read(k_lamp_index);
}

InputManager::BitLocation InputManager::locate(Input input) const
{
    if (m_profile == Profile::VirtuaRacing) {
        switch (input) {
        case Input::Coin1:     return {Port::System, 1u << 0};
        case Input::Coin2:     return {Port::System, 1u << 1};
        case Input::Test:      return {Port::System, 1u << 2};
        case Input::Service:   return {Port::System, 1u << 3};
        case Input::Start1:    return {Port::System, 1u << 4};
        case Input::P1Button1: return {Port::System, 1u << 5};  // VR1 (red)
        case Input::P1Button2: return {Port::System, 1u << 6};  // VR2 (blue)
        case Input::P1Button3: return {Port::System, 1u << 7};  // VR3 (yellow)
        case Input::P1Button4: return {Port::Player1, 1u << 0}; // VR4 (green)
        case Input::P1Button5: return {Port::Player1, 1u << 4}; // shift down
        case Input::P1Button6: return {Port::Player1, 1u << 5}; // shift up
        default:               return {Port::System, 0};        // analog or not wired
        }
    }
    switch (input) {
    case Input::Coin1:     return {Port::System, 1u << 0};
    case Input::Coin2:     return {Port::System, 1u << 1};
    case Input::Test:      return {Port::System, 1u << 2};
    case Input::Service:   return {Port::System, 1u << 3};
    case Input::Start1:    return {Port::System, 1u << 4};
    case Input::Start2:    return {Port::System, 1u << 5};
    case Input::P1Button1: return {Port::Player1, 1u << 0};
    case Input::P1Button2: return {Port::Player1, 1u << 1};
    case Input::P1Button3: return {Port::Player1, 1u << 2};
    case Input::P1Down:    return {Port::Player1, 1u << 4};
    case Input::P1Up:      return {Port::Player1, 1u << 5};
    case Input::P1Right:   return {Port::Player1, 1u << 6};
    case Input::P1Left:    return {Port::Player1, 1u << 7};
    case Input::P1Button4:
    case Input::P1Button5:
    case Input::P1Button6: return {Port::System, 0}; // not wired on Virtua Fighter
    case Input::P2Button1: return {Port::Player2, 1u << 0};
    case Input::P2Button2: return {Port::Player2, 1u << 1};
    case Input::P2Button3: return {Port::Player2, 1u << 2};
    case Input::P2Down:    return {Port::Player2, 1u << 4};
    case Input::P2Up:      return {Port::Player2, 1u << 5};
    case Input::P2Right:   return {Port::Player2, 1u << 6};
    case Input::P2Left:    return {Port::Player2, 1u << 7};
    case Input::Count:     break;
    }
    return {Port::System, 0};
}

const char* InputManager::input_name(Input input)
{
    switch (input) {
    case Input::Coin1:     return "Coin 1";
    case Input::Coin2:     return "Coin 2";
    case Input::Test:      return "Test";
    case Input::Service:   return "Service";
    case Input::Start1:    return "Start 1";
    case Input::Start2:    return "Start 2";
    case Input::P1Button1: return "P1 Button 1";
    case Input::P1Button2: return "P1 Button 2";
    case Input::P1Button3: return "P1 Button 3";
    case Input::P1Down:    return "P1 Down";
    case Input::P1Up:      return "P1 Up";
    case Input::P1Right:   return "P1 Right";
    case Input::P1Left:    return "P1 Left";
    case Input::P1Button4: return "P1 Button 4";
    case Input::P1Button5: return "P1 Button 5";
    case Input::P1Button6: return "P1 Button 6";
    case Input::P2Button1: return "P2 Button 1";
    case Input::P2Button2: return "P2 Button 2";
    case Input::P2Button3: return "P2 Button 3";
    case Input::P2Down:    return "P2 Down";
    case Input::P2Up:      return "P2 Up";
    case Input::P2Right:   return "P2 Right";
    case Input::P2Left:    return "P2 Left";
    case Input::Count:     break;
    }
    return "?";
}

void InputManager::set_input(Input input, bool pressed)
{
    switch (input) {
    case Input::P1Left:  m_steer_left = pressed; break;
    case Input::P1Right: m_steer_right = pressed; break;
    case Input::P1Up:    m_accelerate = pressed; break;
    case Input::P1Down:  m_brake = pressed; break;
    default: break;
    }
    const BitLocation location = locate(input);
    uint16_t& port = location.port == Port::System  ? m_system
                   : location.port == Port::Player1 ? m_player1
                                                    : m_player2;
    const uint16_t before = port;
    // Active-low: pressed clears the bit, released sets it.
    if (pressed) {
        port = static_cast<uint16_t>(port & ~location.mask);
    } else {
        port = static_cast<uint16_t>(port | location.mask);
    }

    publish();
    if constexpr (k_trace_input) {
        const bool coin_or_start = input == Input::Coin1 || input == Input::Coin2
                                || input == Input::Start1 || input == Input::Start2;
        if (coin_or_start && pressed && port != before) {
            std::cout << "[Input] " << (input == Input::Coin1 || input == Input::Coin2 ? "Coin inserted" : "Start pressed")
                      << " (" << input_name(input) << ")! System port bitmask: " << Hex{before, 4}
                      << " -> " << Hex{port, 4} << '\n';
        }
    }
}

uint16_t InputManager::port_value(Port port) const
{
    switch (port) {
    case Port::System:  return m_system;
    case Port::Player1: return m_player1;
    case Port::Player2: return m_player2;
    }
    return k_idle_mask;
}

} // namespace model1

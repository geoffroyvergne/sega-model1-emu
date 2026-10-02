#include "core/input_manager.hpp"

#include "core/dual_port_ram.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>
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
    m_p2_left = m_p2_right = m_p2_up = m_p2_down = false;
    m_throttle_up = m_throttle_down = false;
    m_axes.fill(0.0f);
    m_key_pedals = {k_pedal_released, k_pedal_released};
    m_pedal_frames = 0;
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
    m_analog.fill(0xFF);
    if (m_profile == Profile::VirtuaRacing) {
        m_analog[0] = k_wheel_centre;
        m_analog[1] = m_analog[2] = k_pedal_released;
    } else if (m_profile == Profile::StarWars) {
        m_analog[0] = m_analog[1] = m_analog[4] = m_analog[5] = k_stick_centre;
        m_analog[2] = k_throttle_idle;
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

// A self-centring keyboard axis: `low` / `high` push toward min / max.
// A self-centring stick channel: the controller stick when it is off
// centre (`stick` -1..1, mapped over min..max, reversed if asked), else the
// keys.
uint8_t axis(uint8_t value, bool low, bool high, float stick, bool reversed)
{
    using IM = InputManager;
    if (stick != 0.0f) {
        const float half = (IM::k_stick_max - IM::k_stick_min) / 2.0f;
        const float v = IM::k_stick_centre + (reversed ? -stick : stick) * half;
        return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), int{IM::k_stick_min}, int{IM::k_stick_max}));
    }
    const int target = low == high ? IM::k_stick_centre : low ? IM::k_stick_min : IM::k_stick_max;
    return approach(value, target, IM::k_stick_step);
}

// Maps a pedal 0..1 onto released..0xFF.
uint8_t pedal_value(float pedal, uint8_t released)
{
    return static_cast<uint8_t>(std::lround(released + pedal * (0xFF - released)));
}
} // namespace

void InputManager::update_analog()
{
    if (m_profile == Profile::StarWars) {
        // X is reversed in MAME (left = high), Y is not (up = low).
        auto a = [this](Axis axis) { return axis_value(axis); };
        m_analog[0] = axis(m_analog[0], m_steer_right, m_steer_left, a(Axis::Stick1X), true);
        m_analog[1] = axis(m_analog[1], m_accelerate, m_brake, a(Axis::Stick1Y), false);
        m_analog[4] = axis(m_analog[4], m_p2_right, m_p2_left, a(Axis::Stick2X), true);
        m_analog[5] = axis(m_analog[5], m_p2_up, m_p2_down, a(Axis::Stick2Y), false);
        // Throttle lever: keys at full speed, pedals in proportion.
        const float faster = std::max(m_throttle_up ? 1.0f : 0.0f, a(Axis::Pedal1));
        const float slower = std::max(m_throttle_down ? 1.0f : 0.0f, a(Axis::Pedal2));
        if (faster != slower) {
            const float push = faster > slower ? faster : slower;
            const int step = std::max(1, static_cast<int>(std::lround(push * k_throttle_step)));
            m_analog[2] = approach(m_analog[2], faster > slower ? k_throttle_full : k_throttle_idle, step);
        }
        publish();
        return;
    }
    if (m_profile != Profile::VirtuaRacing) {
        return;
    }
    // Wheel: a paddle as in MAME, lower values to the left. Racing (a pedal
    // held long enough): fast, self-centring. Menus (no pedal): slow, and it
    // stays where it is left, as the cabinet's wheel does.
    // A controller adds a proportional wheel (while racing) and pedals.
    const float stick = axis_value(Axis::Stick1X);
    const float accelerator = axis_value(Axis::Pedal1);
    const float brake = axis_value(Axis::Pedal2);
    const bool pedal = m_accelerate || m_brake || accelerator > k_pedal_held || brake > k_pedal_held;
    m_pedal_frames = pedal ? std::min(m_pedal_frames + 1, k_spring_delay_frames) : 0;
    const bool racing = m_pedal_frames >= k_spring_delay_frames;
    const bool left = m_steer_left || (!racing && stick <= -k_menu_steer);
    const bool right = m_steer_right || (!racing && stick >= k_menu_steer);
    if (left != right) {
        m_analog[0] = approach(m_analog[0], left ? k_wheel_left : k_wheel_right,
                               racing ? k_wheel_step : k_wheel_menu_step);
    } else if (racing && stick != 0.0f) {
        const float v = k_wheel_centre + stick * 0x80;
        m_analog[0] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 0xFF));
    } else if (racing) {
        m_analog[0] = approach(m_analog[0], k_wheel_centre, k_wheel_step);
    }
    m_key_pedals[0] = approach(m_key_pedals[0], m_accelerate ? 0xFF : k_pedal_released, k_pedal_step);
    m_key_pedals[1] = approach(m_key_pedals[1], m_brake ? 0xFF : k_pedal_released, k_pedal_step);
    m_analog[1] = std::max(m_key_pedals[0], pedal_value(accelerator, k_pedal_released));
    m_analog[2] = std::max(m_key_pedals[1], pedal_value(brake, k_pedal_released));
    publish();
}

void InputManager::publish()
{
    if (!m_publishing) {
        return;
    }
    // Star Wars uses all eight channels; for the other games the stand-in
    // publishes the four wired ones twice (0-3 and 4-7).
    const uint32_t wired = m_profile == Profile::StarWars ? 8 : 4;
    for (uint32_t i = 0; i < k_analog_count; ++i) {
        m_shared_ram.board_write(k_analog_index + i, m_analog[i % wired]);
    }
    m_shared_ram.board_write(k_system_index, static_cast<uint8_t>(m_system));
    m_shared_ram.board_write(k_player1_index, static_cast<uint8_t>(m_player1));
    m_shared_ram.board_write(k_player2_index, static_cast<uint8_t>(m_player2));
    for (uint32_t i = k_unused_first; i <= k_unused_last; ++i) {
        m_shared_ram.board_write(i, 0xFF);
    }
    m_shared_ram.board_write(k_status_index, k_status_ready);
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
    if (m_profile == Profile::StarWars) {
        switch (input) {
        case Input::Coin1:     return {Port::System, 1u << 0};
        case Input::Coin2:     return {Port::System, 1u << 1};
        case Input::Test:      return {Port::System, 1u << 2};
        case Input::Service:   return {Port::System, 1u << 3};
        case Input::Start1:    return {Port::System, 1u << 4};
        case Input::Start2:    return {Port::System, 1u << 5};
        case Input::P1Button1: return {Port::Player1, 1u << 0};
        case Input::P1Button2: return {Port::Player1, 1u << 1};
        case Input::P2Button1: return {Port::Player1, 1u << 2};
        case Input::P2Button2: return {Port::Player1, 1u << 3};
        case Input::P1Button3: return {Port::Player1, 1u << 4};
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
    case Input::P2Left:  m_p2_left = pressed; break;
    case Input::P2Right: m_p2_right = pressed; break;
    case Input::P2Up:    m_p2_up = pressed; break;
    case Input::P2Down:  m_p2_down = pressed; break;
    case Input::P1Button5: m_throttle_up = pressed; break;
    case Input::P1Button6: m_throttle_down = pressed; break;
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

void InputManager::set_axis(Axis axis, float value)
{
    const bool pedal = axis == Axis::Pedal1 || axis == Axis::Pedal2;
    const float dead = pedal ? k_pedal_dead_zone : k_stick_dead_zone;
    const float v = std::clamp(value, pedal ? 0.0f : -1.0f, 1.0f);
    const float magnitude = std::abs(v);
    // Outside the dead zone the range is rescaled so it still reaches 1.
    const float scaled = magnitude <= dead ? 0.0f : (magnitude - dead) / (1.0f - dead);
    m_axes[static_cast<std::size_t>(axis)] = v < 0 ? -scaled : scaled;
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

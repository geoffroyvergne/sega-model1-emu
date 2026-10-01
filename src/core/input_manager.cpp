#include "core/input_manager.hpp"

#include "core/log.hpp"

#include <iostream>

namespace model1 {

namespace {

// I/O block byte offsets (one byte per 16-bit word on the real board).
constexpr uint32_t k_analog_end      = 0x10; // 0x00-0x0F: analog channels 0-7
constexpr uint32_t k_system_offset   = 0x10;
constexpr uint32_t k_player1_offset  = 0x12;
constexpr uint32_t k_player2_offset  = 0x14;
constexpr uint32_t k_digital_end     = 0x20; // 0x16-0x1C: unused digital ports
constexpr uint32_t k_lamp_offset     = 0x1E;
constexpr uint32_t k_drive_offset    = 0x22;

} // namespace

void InputManager::reset()
{
    m_system = k_idle_mask;
    m_player1 = k_idle_mask;
    m_player2 = k_idle_mask;
    m_lamps = 0;
}

InputManager::BitLocation InputManager::locate(Input input)
{
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

uint16_t InputManager::read_io(uint32_t offset) const
{
    if (offset < k_analog_end) {
        return 0x0000; // no analog controls connected
    }
    switch (offset) {
    case k_system_offset:  return m_system;
    case k_player1_offset: return m_player1;
    case k_player2_offset: return m_player2;
    case k_lamp_offset:    return m_lamps;
    default:
        break;
    }
    if (offset < k_digital_end) {
        return 0x00FF; // unused digital port, nothing pressed
    }
    std::cerr << "[Input] WARNING: read from unknown I/O board offset " << Hex{offset, 2}
              << ", returning 0xFFFF\n";
    return 0xFFFF;
}

void InputManager::write_io(uint32_t offset, uint16_t value)
{
    switch (offset) {
    case k_lamp_offset:
        // Bits 0-1 coin meters, 2 start lamp, 3-7 game-specific lamps.
        m_lamps = value;
        return;
    case k_drive_offset:
        // Force-feedback drive board command (driving games); no drive board.
        return;
    default:
        std::cerr << "[Input] WARNING: write " << Hex{value, 4} << " to unknown I/O board offset "
                  << Hex{offset, 2} << ", ignored\n";
        return;
    }
}

} // namespace model1

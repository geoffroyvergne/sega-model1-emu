#pragma once

#include "core/input_manager.hpp"

#include <array>
#include <cstdint>

namespace model1 {

// Combines the presses of several host input devices (keyboard, game
// controllers) on the same Model 1 input: the input is pressed while any
// device holds it, and released when the last one lets go. Each device
// reports its own transitions only (press once, release once).
class SharedPresses {
public:
    explicit SharedPresses(InputManager& inputs) : m_inputs(inputs) {}

    void press(InputManager::Input input)
    {
        uint8_t& count = m_counts[static_cast<std::size_t>(input)];
        if (++count == 1) {
            m_inputs.set_input(input, true);
        }
    }

    void release(InputManager::Input input)
    {
        uint8_t& count = m_counts[static_cast<std::size_t>(input)];
        if (count > 0 && --count == 0) {
            m_inputs.set_input(input, false);
        }
    }

    void set(InputManager::Input input, bool pressed) { pressed ? press(input) : release(input); }

    [[nodiscard]] bool pressed(InputManager::Input input) const
    {
        return m_counts[static_cast<std::size_t>(input)] != 0;
    }

    [[nodiscard]] InputManager& inputs() { return m_inputs; }

private:
    InputManager& m_inputs;
    std::array<uint8_t, InputManager::k_input_count> m_counts{};
};

} // namespace model1

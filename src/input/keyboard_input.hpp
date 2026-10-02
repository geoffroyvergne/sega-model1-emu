#pragma once

#include "core/input_manager.hpp"
#include "input/shared_presses.hpp"

#include <array>
#include <cstdint>

union SDL_Event;

namespace model1 {

// Host-side keyboard layer: translates SDL keyboard events into Model 1
// inputs. Keys are matched by physical position (SDL scancodes), so the
// layout is the same on QWERTY, AZERTY, etc.
//
// Default bindings:
//   Arrow keys / W A S D   Player 1 up, left, down, right
//   J K L / Z X C          Player 1 buttons 1, 2, 3
//   U I O / V B N          Player 1 buttons 4, 5, 6
//   Keypad 8 5 4 6         Player 2 up, down, left, right
//   Keypad 1 2 3           Player 2 buttons 1, 2, 3
//   1 / 2                  Start 1 / Start 2
//   5 / 6                  Coin 1 / Coin 2
//   F2                     Test switch
//   9                      Service switch
//
// Several keys (and game controllers, through SharedPresses) may drive the
// same input; the input stays pressed until all of them are released. All inputs are released when the window loses focus,
// so a key released in another application does not stay stuck.
class KeyboardInput {
public:
    explicit KeyboardInput(SharedPresses& presses) : m_presses(presses) {}

    // Call for every polled SDL event; non-keyboard events are ignored
    // except window focus loss. Never blocks.
    void process_sdl_event(const SDL_Event& event);

private:
    void key_changed(int scancode, bool pressed);
    void release_all();

    SharedPresses& m_presses;
    // Number of currently held keys bound to each input.
    std::array<uint8_t, InputManager::k_input_count> m_held_keys{};
};

} // namespace model1

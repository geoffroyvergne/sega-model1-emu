#pragma once

#include "core/input_manager.hpp"
#include "input/shared_presses.hpp"

#include <array>
#include <cstdint>
#include <string>

union SDL_Event;
struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

namespace model1 {

// Host-side game controller layer (SDL GameController API: Xbox,
// PlayStation, Switch Pro and other pads SDL knows, named by their Xbox
// equivalents below). Up to two controllers, in the order they are
// connected: the first plays player 1, the second player 2. Controllers can
// be plugged and unplugged at any time; unplugging one releases everything
// it held.
//
// Buttons (player 1 / player 2):
//   A B X                 buttons 1 2 3 (VR: view buttons VR1-VR3;
//                         SWA: trigger, torpedo, VR view button)
//   Y, LB, RB             player 1 buttons 4, 5, 6 (VR: VR4, shift down /
//                         up; SWA: LB / RB throttle faster / slower)
//   D-pad                 joystick directions
//   Start                 start 1 / start 2
//   Back (View, Select)   coin 1 / coin 2
//
// Left stick: Virtua Fighter: joystick directions (pushed past halfway).
// Virtua Racing: the steering wheel (player 1). Star Wars Arcade: the
// pilot's (player 1) or gunner's (player 2) flight stick.
// Triggers (player 1): VR: RT accelerator, LT brake; SWA: RT / LT throttle
// faster / slower. See InputManager::Axis for how the analog controls
// combine with the keys.
//
// Extra controller mappings can be added in SDL's gamecontrollerdb.txt
// format (see load_mappings).
class GamepadInput {
public:
    static constexpr int k_max_pads = 2;
    // Left stick as joystick directions (Virtua Fighter): pressed beyond
    // k_press, released below k_release (hysteresis, no chatter).
    static constexpr float k_direction_press = 0.5f;
    static constexpr float k_direction_release = 0.35f;

    explicit GamepadInput(SharedPresses& presses);
    ~GamepadInput();

    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

    // Adds controller mappings from a gamecontrollerdb.txt-format file if it
    // exists; returns the number added (0 when there is no file).
    static int load_mappings(const std::string& path);

    // Call for every polled SDL event; non-controller events are ignored.
    void process_sdl_event(const SDL_Event& event);

    // Device-independent part, also used by the tests: `pad` is the player
    // slot (0 or 1); `button` / `axis` are SDL_GameControllerButton /
    // SDL_GameControllerAxis values; axis values are SDL's (-32768..32767,
    // triggers 0..32767).
    void button_changed(int pad, int button, bool pressed);
    void axis_changed(int pad, int axis, int16_t value);
    // Releases every input the slot holds and centres its analog controls.
    void release_pad(int pad);

    [[nodiscard]] int connected_count() const;

private:
    struct Pad {
        SDL_GameController* controller = nullptr;
        int32_t instance = -1;   // SDL joystick instance id; -1 = slot free
        std::array<bool, InputManager::k_input_count> held{}; // inputs this pad presses
        std::array<bool, 4> stick_direction{};                // left stick as up, down, left, right
    };

    void open_device(int device_index);
    void close_instance(int32_t instance);
    [[nodiscard]] int slot_of(int32_t instance) const;
    void set_held(Pad& pad, InputManager::Input input, bool pressed);
    void stick_directions(int pad, int axis, float value);

    SharedPresses& m_presses;
    std::array<Pad, k_max_pads> m_pads{};
};

} // namespace model1

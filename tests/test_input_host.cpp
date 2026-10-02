// Host input tests (SDL types, no devices): game controller mapping per
// game, analog sticks / triggers through InputManager, and keyboard and
// controller sharing the same inputs.

#include "test_framework.hpp"

#include "core/dual_port_ram.hpp"
#include "core/input_manager.hpp"
#include "input/gamepad_input.hpp"
#include "input/keyboard_input.hpp"
#include "input/shared_presses.hpp"

#include <SDL.h>

#include <cstdint>

namespace {

using model1::DualPortRam;
using model1::GamepadInput;
using model1::InputManager;
using model1::KeyboardInput;
using model1::SharedPresses;
using In = InputManager::Input;
using Port = InputManager::Port;
using Profile = InputManager::Profile;

struct InputRig {
    DualPortRam ram;
    InputManager inputs{ram};
    SharedPresses presses{inputs};
    KeyboardInput keyboard{presses};
    GamepadInput pads{presses};

    explicit InputRig(Profile profile) { inputs.set_profile(profile); }

    void key(SDL_Scancode code, bool down)
    {
        SDL_Event event{};
        event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.keysym.scancode = code;
        keyboard.process_sdl_event(event);
    }
    void frames(int n)
    {
        for (int i = 0; i < n; ++i) inputs.update_analog();
    }
    [[nodiscard]] uint16_t port(Port p) const { return inputs.port_value(p) & 0xFF; }
};

} // namespace

TEST_CASE(gamepad_buttons_map_to_both_players)
{
    InputRig rig(Profile::VirtuaFighter);
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_A, true);
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_DPAD_LEFT, true);
    rig.pads.button_changed(1, SDL_CONTROLLER_BUTTON_X, true);
    rig.pads.button_changed(1, SDL_CONTROLLER_BUTTON_START, true);
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_BACK, true);
    CHECK_EQ(rig.port(Port::Player1), 0x7Eu); // button 1, left
    CHECK_EQ(rig.port(Port::Player2), 0xFBu); // button 3
    CHECK_EQ(rig.port(Port::System), 0xDEu);  // coin 1, start 2
    // Player 2 has no buttons 4-6: Y does nothing.
    rig.pads.button_changed(1, SDL_CONTROLLER_BUTTON_Y, true);
    CHECK_EQ(rig.port(Port::Player2), 0xFBu);
    rig.pads.release_pad(0);
    CHECK_EQ(rig.port(Port::Player1), 0xFFu);
    CHECK_EQ(rig.port(Port::System), 0xDFu);
}

TEST_CASE(gamepad_and_keyboard_share_inputs)
{
    InputRig rig(Profile::VirtuaFighter);
    rig.key(SDL_SCANCODE_J, true);                             // P1 button 1
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_A, true); // the same input
    rig.key(SDL_SCANCODE_J, false);
    CHECK_EQ(rig.port(Port::Player1), 0xFEu); // still held by the pad
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_A, false);
    CHECK_EQ(rig.port(Port::Player1), 0xFFu);
    // A repeated button event does not count twice.
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_A, true);
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_A, true);
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_A, false);
    CHECK_EQ(rig.port(Port::Player1), 0xFFu);
}

TEST_CASE(gamepad_stick_is_a_joystick_in_virtua_fighter)
{
    InputRig rig(Profile::VirtuaFighter);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    CHECK_EQ(rig.port(Port::Player1), 0xBFu); // right
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 13000); // 0.4: above release, held
    CHECK_EQ(rig.port(Port::Player1), 0xBFu);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 9000);  // below release
    CHECK_EQ(rig.port(Port::Player1), 0xFFu);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 13000); // not past press yet
    CHECK_EQ(rig.port(Port::Player1), 0xFFu);
    rig.pads.axis_changed(1, SDL_CONTROLLER_AXIS_LEFTY, -32768);
    CHECK_EQ(rig.port(Port::Player2), 0xDFu); // up
    // The d-pad and the stick on the same direction: released when both are.
    rig.pads.button_changed(1, SDL_CONTROLLER_BUTTON_DPAD_UP, true);
    rig.pads.axis_changed(1, SDL_CONTROLLER_AXIS_LEFTY, 0);
    CHECK_EQ(rig.port(Port::Player2), 0xDFu);
    rig.pads.button_changed(1, SDL_CONTROLLER_BUTTON_DPAD_UP, false);
    CHECK_EQ(rig.port(Port::Player2), 0xFFu);
    // No analog effect in VF.
    CHECK_EQ(rig.inputs.axis_value(InputManager::Axis::Stick1X), 0.0f);
}

TEST_CASE(gamepad_wheel_and_pedals_in_virtua_racing)
{
    InputRig rig(Profile::VirtuaRacing);
    // Menus (no pedal): the stick pushed right moves the wheel slowly, like
    // the arrow key, and it stays when the stick is let go.
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    rig.frames(8);
    CHECK_EQ(rig.inputs.analog(0), 0x80u + 8 * 4);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 0);
    rig.frames(10);
    CHECK_EQ(rig.inputs.analog(0), 0x80u + 8 * 4);
    // A slight push in menus does nothing.
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 10000);
    rig.frames(5);
    CHECK_EQ(rig.inputs.analog(0), 0x80u + 8 * 4);

    // Accelerator trigger: proportional, and racing once held.
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    rig.frames(1);
    CHECK_EQ(rig.inputs.analog(1), 0xFFu);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 16384);
    rig.frames(InputManager::k_spring_delay_frames);
    const unsigned half = rig.inputs.analog(1);
    CHECK(half > 0x80u && half < 0xA0u);
    CHECK_EQ(rig.inputs.analog(2), 0x30u); // brake released
    // Racing: the wheel follows the stick directly.
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, -32768);
    rig.frames(1);
    CHECK_EQ(rig.inputs.analog(0), 0x00u);
    // Let go: it springs back to centre like the keyboard wheel (8 frames).
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 0);
    rig.frames(1);
    CHECK_EQ(rig.inputs.analog(0), 0x10u);
    rig.frames(7);
    CHECK_EQ(rig.inputs.analog(0), 0x80u);
    // Brake trigger; the key still works and the larger value wins.
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    rig.frames(1);
    CHECK_EQ(rig.inputs.analog(2), 0xFFu);
    rig.pads.release_pad(0);
    rig.key(SDL_SCANCODE_DOWN, true);
    rig.frames(1);
    CHECK_EQ(rig.inputs.analog(2), 0x50u); // the key's ramp: 0x30 + 0x20
    CHECK_EQ(rig.inputs.analog(1), 0x30u); // trigger released with the pad
    // Shoulder buttons are the shifter.
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
    CHECK_EQ(rig.port(Port::Player1), 0xDFu); // shift up
}

TEST_CASE(gamepad_sticks_and_throttle_in_star_wars)
{
    InputRig rig(Profile::StarWars);
    // Pilot's stick: proportional over 27-227, X reversed.
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, -32768);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTY, -32768);
    rig.frames(1);
    CHECK_EQ(rig.inputs.analog(0), 227u);
    CHECK_EQ(rig.inputs.analog(1), 27u);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 16384); // half right
    rig.frames(1);
    const unsigned x = rig.inputs.analog(0);
    CHECK(x > 80u && x < 100u);
    // Released: back to centre (the keys' spring).
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTX, 0);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_LEFTY, 0);
    rig.frames(20);
    CHECK_EQ(rig.inputs.analog(0), 0x7Fu);
    CHECK_EQ(rig.inputs.analog(1), 0x7Fu);
    // Gunner's stick from the second controller, on channels 4 / 5.
    rig.pads.axis_changed(1, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    rig.frames(1);
    CHECK_EQ(rig.inputs.analog(4), 27u);
    // Throttle: RT pushes toward fast (28) in proportion, and it stays.
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    rig.frames(10);
    CHECK_EQ(rig.inputs.analog(2), 160u); // 200 - 10 x 4
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);
    rig.frames(10);
    CHECK_EQ(rig.inputs.analog(2), 160u);
    rig.pads.axis_changed(0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    rig.frames(100);
    CHECK_EQ(rig.inputs.analog(2), 200u);
    // Trigger and torpedo.
    rig.pads.button_changed(0, SDL_CONTROLLER_BUTTON_A, true);
    rig.pads.button_changed(1, SDL_CONTROLLER_BUTTON_B, true);
    CHECK_EQ(rig.port(Port::Player1), 0xF6u); // pilot trigger, gunner torpedo
}

TEST_CASE(gamepad_without_device_is_idle)
{
    InputRig rig(Profile::VirtuaRacing);
    CHECK_EQ(rig.pads.connected_count(), 0);
    // Events for controllers that were never opened are ignored.
    SDL_Event event{};
    event.type = SDL_CONTROLLERBUTTONDOWN;
    event.cbutton.which = 1234;
    event.cbutton.button = SDL_CONTROLLER_BUTTON_START;
    rig.pads.process_sdl_event(event);
    CHECK_EQ(rig.port(Port::System), 0xFFu);
    CHECK_EQ(GamepadInput::load_mappings("no-such-file.txt"), 0);
}

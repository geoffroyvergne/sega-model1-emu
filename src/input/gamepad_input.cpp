#include "input/gamepad_input.hpp"

#include <SDL.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <system_error>

namespace model1 {

namespace {

using Input = InputManager::Input;
using Axis = InputManager::Axis;
using Profile = InputManager::Profile;

std::optional<Input> input_for_button(int pad, int button)
{
    const bool p1 = pad == 0;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:             return p1 ? Input::P1Button1 : Input::P2Button1;
    case SDL_CONTROLLER_BUTTON_B:             return p1 ? Input::P1Button2 : Input::P2Button2;
    case SDL_CONTROLLER_BUTTON_X:             return p1 ? Input::P1Button3 : Input::P2Button3;
    case SDL_CONTROLLER_BUTTON_Y:             if (p1) return Input::P1Button4; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  if (p1) return Input::P1Button5; break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (p1) return Input::P1Button6; break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return p1 ? Input::P1Up : Input::P2Up;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return p1 ? Input::P1Down : Input::P2Down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return p1 ? Input::P1Left : Input::P2Left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return p1 ? Input::P1Right : Input::P2Right;
    case SDL_CONTROLLER_BUTTON_START:         return p1 ? Input::Start1 : Input::Start2;
    case SDL_CONTROLLER_BUTTON_BACK:          return p1 ? Input::Coin1 : Input::Coin2;
    default: break;
    }
    return std::nullopt;
}

float stick_value(int16_t value)
{
    return value < 0 ? static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
}

} // namespace

GamepadInput::GamepadInput(SharedPresses& presses)
    : m_presses(presses)
{
}

GamepadInput::~GamepadInput()
{
    for (Pad& pad : m_pads) {
        if (pad.controller != nullptr) {
            SDL_GameControllerClose(pad.controller);
        }
    }
}

int GamepadInput::load_mappings(const std::string& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return 0;
    }
    const int added = SDL_GameControllerAddMappingsFromFile(path.c_str());
    if (added < 0) {
        std::cerr << "[Gamepad] WARNING: could not read controller mappings from '" << path << "': "
                  << SDL_GetError() << '\n';
        return 0;
    }
    std::cout << "[Gamepad] " << added << " controller mappings added from '" << path << "'\n";
    return added;
}

void GamepadInput::process_sdl_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_CONTROLLERDEVICEADDED:
        open_device(event.cdevice.which); // a device index here
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        close_instance(event.cdevice.which); // an instance id here
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (const int pad = slot_of(event.cbutton.which); pad >= 0) {
            button_changed(pad, event.cbutton.button, event.type == SDL_CONTROLLERBUTTONDOWN);
        }
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (const int pad = slot_of(event.caxis.which); pad >= 0) {
            axis_changed(pad, event.caxis.axis, event.caxis.value);
        }
        break;
    default:
        break;
    }
}

void GamepadInput::open_device(int device_index)
{
    const int32_t instance = SDL_JoystickGetDeviceInstanceID(device_index);
    if (instance < 0 || slot_of(instance) >= 0) {
        return; // unknown, or already open (SDL can report a pad twice at startup)
    }
    for (int slot = 0; slot < k_max_pads; ++slot) {
        Pad& pad = m_pads[static_cast<std::size_t>(slot)];
        if (pad.instance >= 0) {
            continue;
        }
        pad.controller = SDL_GameControllerOpen(device_index);
        if (pad.controller == nullptr) {
            std::cerr << "[Gamepad] WARNING: could not open controller " << device_index << ": " << SDL_GetError() << '\n';
            return;
        }
        pad.instance = instance;
        const char* name = SDL_GameControllerName(pad.controller);
        std::cout << "[Gamepad] '" << (name != nullptr ? name : "controller") << "' connected as player " << (slot + 1)
                  << '\n';
        return;
    }
    std::cout << "[Gamepad] A controller was connected, but players 1 and 2 already have one: ignored\n";
}

void GamepadInput::close_instance(int32_t instance)
{
    const int slot = slot_of(instance);
    if (slot < 0) {
        return;
    }
    release_pad(slot);
    Pad& pad = m_pads[static_cast<std::size_t>(slot)];
    SDL_GameControllerClose(pad.controller);
    pad.controller = nullptr;
    pad.instance = -1;
    std::cout << "[Gamepad] Player " << (slot + 1) << "'s controller disconnected\n";
}

int GamepadInput::slot_of(int32_t instance) const
{
    for (int slot = 0; slot < k_max_pads; ++slot) {
        if (m_pads[static_cast<std::size_t>(slot)].instance == instance) {
            return slot;
        }
    }
    return -1;
}

int GamepadInput::connected_count() const
{
    int count = 0;
    for (const Pad& pad : m_pads) {
        count += pad.instance >= 0 ? 1 : 0;
    }
    return count;
}

void GamepadInput::set_held(Pad& pad, Input input, bool pressed)
{
    bool& held = pad.held[static_cast<std::size_t>(input)];
    if (held != pressed) {
        held = pressed;
        m_presses.set(input, pressed);
    }
}

void GamepadInput::button_changed(int pad, int button, bool pressed)
{
    if (pad < 0 || pad >= k_max_pads) {
        return;
    }
    if (const std::optional<Input> input = input_for_button(pad, button)) {
        set_held(m_pads[static_cast<std::size_t>(pad)], *input, pressed);
    }
}

void GamepadInput::axis_changed(int pad, int axis, int16_t value)
{
    if (pad < 0 || pad >= k_max_pads) {
        return;
    }
    InputManager& inputs = m_presses.inputs();
    const bool p1 = pad == 0;
    const float v = stick_value(value);
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
    case SDL_CONTROLLER_AXIS_LEFTY: {
        const bool x = axis == SDL_CONTROLLER_AXIS_LEFTX;
        if (inputs.profile() == Profile::VirtuaFighter) {
            stick_directions(pad, axis, v);
        } else {
            inputs.set_axis(p1 ? (x ? Axis::Stick1X : Axis::Stick1Y) : (x ? Axis::Stick2X : Axis::Stick2Y), v);
        }
        break;
    }
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        if (p1) inputs.set_axis(Axis::Pedal1, v);
        break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        if (p1) inputs.set_axis(Axis::Pedal2, v);
        break;
    default:
        break;
    }
}

void GamepadInput::stick_directions(int pad, int axis, float value)
{
    Pad& p = m_pads[static_cast<std::size_t>(pad)];
    const bool p1 = pad == 0;
    // [0] the negative direction (up / left), [1] the positive one.
    const bool x = axis == SDL_CONTROLLER_AXIS_LEFTX;
    const std::size_t base = x ? 2 : 0; // stick_direction: up, down, left, right
    const Input negative = x ? (p1 ? Input::P1Left : Input::P2Left) : (p1 ? Input::P1Up : Input::P2Up);
    const Input positive = x ? (p1 ? Input::P1Right : Input::P2Right) : (p1 ? Input::P1Down : Input::P2Down);
    const std::array<Input, 2> directions = {negative, positive};
    for (std::size_t i = 0; i < 2; ++i) {
        const float toward = i == 0 ? -value : value;
        bool& state = p.stick_direction[base + i];
        const bool next = state ? toward > k_direction_release : toward > k_direction_press;
        if (next != state) {
            state = next;
            // Kept apart from the d-pad's own press of the same direction.
            m_presses.set(directions[i], next);
        }
    }
}

void GamepadInput::release_pad(int pad)
{
    if (pad < 0 || pad >= k_max_pads) {
        return;
    }
    Pad& p = m_pads[static_cast<std::size_t>(pad)];
    for (std::size_t i = 0; i < p.held.size(); ++i) {
        set_held(p, static_cast<Input>(i), false);
    }
    for (int axis : {SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY}) {
        stick_directions(pad, axis, 0.0f);
    }
    InputManager& inputs = m_presses.inputs();
    if (pad == 0) {
        inputs.set_axis(Axis::Stick1X, 0);
        inputs.set_axis(Axis::Stick1Y, 0);
        inputs.set_axis(Axis::Pedal1, 0);
        inputs.set_axis(Axis::Pedal2, 0);
    } else {
        inputs.set_axis(Axis::Stick2X, 0);
        inputs.set_axis(Axis::Stick2Y, 0);
    }
}

} // namespace model1

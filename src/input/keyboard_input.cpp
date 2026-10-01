#include "input/keyboard_input.hpp"

#include <SDL.h>

#include <optional>

namespace model1 {

namespace {

using Input = InputManager::Input;

std::optional<Input> input_for_scancode(int scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_UP:    case SDL_SCANCODE_W: return Input::P1Up;
    case SDL_SCANCODE_DOWN:  case SDL_SCANCODE_S: return Input::P1Down;
    case SDL_SCANCODE_LEFT:  case SDL_SCANCODE_A: return Input::P1Left;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: return Input::P1Right;
    case SDL_SCANCODE_J:     case SDL_SCANCODE_Z: return Input::P1Button1;
    case SDL_SCANCODE_K:     case SDL_SCANCODE_X: return Input::P1Button2;
    case SDL_SCANCODE_L:     case SDL_SCANCODE_C: return Input::P1Button3;
    case SDL_SCANCODE_1:  return Input::Start1;
    case SDL_SCANCODE_2:  return Input::Start2;
    case SDL_SCANCODE_5:  return Input::Coin1;
    case SDL_SCANCODE_6:  return Input::Coin2;
    case SDL_SCANCODE_F2: return Input::Test;
    case SDL_SCANCODE_9:  return Input::Service;
    default:              return std::nullopt;
    }
}

} // namespace

void KeyboardInput::process_sdl_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_KEYDOWN:
        if (event.key.repeat == 0) { // ignore auto-repeat
            key_changed(event.key.keysym.scancode, true);
        }
        break;
    case SDL_KEYUP:
        key_changed(event.key.keysym.scancode, false);
        break;
    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            release_all();
        }
        break;
    default:
        break;
    }
}

void KeyboardInput::key_changed(int scancode, bool pressed)
{
    const std::optional<Input> input = input_for_scancode(scancode);
    if (!input) {
        return;
    }
    uint8_t& held = m_held_keys[static_cast<std::size_t>(*input)];
    if (pressed) {
        ++held;
        if (held == 1) {
            m_inputs.set_input(*input, true);
        }
    } else if (held > 0) {
        --held;
        if (held == 0) {
            m_inputs.set_input(*input, false);
        }
    }
}

void KeyboardInput::release_all()
{
    for (std::size_t i = 0; i < m_held_keys.size(); ++i) {
        if (m_held_keys[i] != 0) {
            m_held_keys[i] = 0;
            m_inputs.set_input(static_cast<Input>(i), false);
        }
    }
}

} // namespace model1

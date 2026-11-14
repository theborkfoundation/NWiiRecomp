#include "mouse_keyboard_source.h"
#include "runtime/config.h"
#include <SDL.h>
#include <cstdlib>
#include <string>

namespace nwii::runtime::input {

void MouseKeyboardSource::update(GameCubePadState pads[4], WiimoteState motes[4]) {
    int mx, my;
    uint32_t mouse_state = SDL_GetMouseState(&mx, &my);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    motes[0].err = 0; 
    int w = nwii::runtime::Config::get().window_width;
    int h = nwii::runtime::Config::get().window_height;
    if (w == 0) w = 640;
    if (h == 0) h = 480;
    
    motes[0].ir_x = (float)mx / (float)w;
    motes[0].ir_y = (float)my / (float)h;

    // Detect horizontal Wiimote layout (e.g. Rayman Origins SOJE41, New Super Mario Bros SMNE01)
    const auto& game_id = nwii::runtime::Config::get().game_id;
    bool is_horizontal = (game_id.rfind("SOJ", 0) == 0) ||
                         (game_id.rfind("SMN", 0) == 0) ||
                         (std::getenv("NWII_HORIZONTAL") != nullptr);

    if (keys) {
        // Wiimote 2 & A (Accept / Jump):
        // Space, Enter, K, 2, X, or Left Click
        if ((mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT)) ||
            keys[SDL_SCANCODE_SPACE]    ||
            keys[SDL_SCANCODE_RETURN]   ||
            keys[SDL_SCANCODE_KP_ENTER] ||
            keys[SDL_SCANCODE_K]        ||
            keys[SDL_SCANCODE_2]        ||
            keys[SDL_SCANCODE_X]) {
            motes[0].buttons |= 0x0100; // WPAD_BUTTON_2 (Jump / Select)
            motes[0].buttons |= 0x0800; // WPAD_BUTTON_A (Accept)
        }

        // Wiimote 1 & B (Attack / Run / Back):
        // J, 1, Z, Left Shift, or Right Click
        if ((mouse_state & SDL_BUTTON(SDL_BUTTON_RIGHT)) ||
            keys[SDL_SCANCODE_J]      ||
            keys[SDL_SCANCODE_1]      ||
            keys[SDL_SCANCODE_Z]      ||
            keys[SDL_SCANCODE_LSHIFT]) {
            motes[0].buttons |= 0x0200; // WPAD_BUTTON_1 (Attack / Dash)
            motes[0].buttons |= 0x0400; // WPAD_BUTTON_B (Back / Bubble)
        }

        // Wiimote B (Bubble):
        if (keys[SDL_SCANCODE_C]) {
            motes[0].buttons |= 0x0400; // WPAD_BUTTON_B
        }

        // Wiimote Plus (Pause / Menu):
        if (keys[SDL_SCANCODE_ESCAPE] ||
            keys[SDL_SCANCODE_P]      ||
            keys[SDL_SCANCODE_EQUALS]) {
            motes[0].buttons |= 0x0010; // WPAD_BUTTON_PLUS
        }

        // Wiimote Minus (Select / Stats):
        if (keys[SDL_SCANCODE_MINUS]     ||
            keys[SDL_SCANCODE_TAB]       ||
            keys[SDL_SCANCODE_BACKSPACE]) {
            motes[0].buttons |= 0x1000; // WPAD_BUTTON_MINUS
        }

        // Directional controls (Arrow keys & WASD):
        bool k_left  = keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A];
        bool k_right = keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D];
        bool k_up    = keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W];
        bool k_down  = keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S];

        // Wiimote orientation: a horizontal remote has its D-pad rotated 90
        // degrees, so the physical direction that means "left" reports UP.
        // Per-game default is horizontal for SOJ/SMN titles; NWII_REMOTE=
        // horizontal|vertical overrides it.
        bool horiz = is_horizontal;
        if (const char *r = std::getenv("NWII_REMOTE")) {
            horiz = (std::string(r) == "horizontal");
        }
        if (horiz) {
            if (k_left)  motes[0].buttons |= 0x0008; // report UP    -> action Left
            if (k_right) motes[0].buttons |= 0x0004; // report DOWN  -> action Right
            if (k_up)    motes[0].buttons |= 0x0002; // report RIGHT -> action Up
            if (k_down)  motes[0].buttons |= 0x0001; // report LEFT  -> action Down
        } else {
            if (k_left)  motes[0].buttons |= 0x0001; // WPAD_BUTTON_LEFT
            if (k_right) motes[0].buttons |= 0x0002; // WPAD_BUTTON_RIGHT
            if (k_up)    motes[0].buttons |= 0x0008; // WPAD_BUTTON_UP
            if (k_down)  motes[0].buttons |= 0x0004; // WPAD_BUTTON_DOWN
        }

        // Classic Controller extension (used when NWII_INPUT=classic):
        {
            ClassicPadState &cl = motes[0].classic;
            cl = {};
            cl.connected = true;
            if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RETURN] ||
                keys[SDL_SCANCODE_KP_ENTER] || keys[SDL_SCANCODE_K] ||
                keys[SDL_SCANCODE_2] || keys[SDL_SCANCODE_X] ||
                (mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT)))
                cl.buttons |= 0x0010;  // A
            if (keys[SDL_SCANCODE_J] || keys[SDL_SCANCODE_1] ||
                keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_LSHIFT] ||
                (mouse_state & SDL_BUTTON(SDL_BUTTON_RIGHT)))
                cl.buttons |= 0x0040;  // B
            if (keys[SDL_SCANCODE_U]) cl.buttons |= 0x0008;  // X
            if (keys[SDL_SCANCODE_I]) cl.buttons |= 0x0020;  // Y
            if (keys[SDL_SCANCODE_Q]) cl.buttons |= 0x2000;  // L
            if (keys[SDL_SCANCODE_E]) cl.buttons |= 0x0200;  // R
            if (keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_P] ||
                keys[SDL_SCANCODE_EQUALS])
                cl.buttons |= 0x0400;  // +
            if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_TAB] ||
                keys[SDL_SCANCODE_BACKSPACE])
                cl.buttons |= 0x1000;  // -
            if (k_up)    cl.buttons |= 0x0001;
            if (k_left)  cl.buttons |= 0x0002;
            if (k_down)  cl.buttons |= 0x4000;
            if (k_right) cl.buttons |= 0x8000;
            cl.lx = k_right ? 1.0f : (k_left ? -1.0f : 0.0f);
            cl.ly = k_up ? 1.0f : (k_down ? -1.0f : 0.0f);
        }

        // GameCube Pad fallback mapping:
        pads[0].err = 0;
        if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_K])  pads[0].buttons |= 0x0100; // A
        if (keys[SDL_SCANCODE_J] || keys[SDL_SCANCODE_X])      pads[0].buttons |= 0x0200; // B
        if (keys[SDL_SCANCODE_RETURN])                         pads[0].buttons |= 0x1000; // START
        if (keys[SDL_SCANCODE_UP])    pads[0].buttons |= 0x0008;
        if (keys[SDL_SCANCODE_DOWN])  pads[0].buttons |= 0x0004;
        if (keys[SDL_SCANCODE_LEFT])  pads[0].buttons |= 0x0001;
        if (keys[SDL_SCANCODE_RIGHT]) pads[0].buttons |= 0x0002;
        if (keys[SDL_SCANCODE_W])     pads[0].stick_y = 127;
        if (keys[SDL_SCANCODE_S])     pads[0].stick_y = -128;
        if (keys[SDL_SCANCODE_D])     pads[0].stick_x = 127;
        if (keys[SDL_SCANCODE_A])     pads[0].stick_x = -128;
    }
}

} 

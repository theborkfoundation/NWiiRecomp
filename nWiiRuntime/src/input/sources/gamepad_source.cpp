#include "gamepad_source.h"
#include "runtime/config.h"
#include <SDL.h>
#include <cmath>
#include <cstdlib>
#include <string>

namespace nwii::runtime::input {

void GamepadSource::update(GameCubePadState pads[4], WiimoteState motes[4]) {
    auto& cfg = nwii::runtime::Config::get();
    int mode = cfg.input_mode;
    bool want_pointer = (mode == 3 || mode == 4);

    bool is_horizontal = (cfg.game_id.rfind("SOJ", 0) == 0) ||
                         (cfg.game_id.rfind("SMN", 0) == 0) ||
                         (std::getenv("NWII_HORIZONTAL") != nullptr);

    for (int i = 0; i < 4; ++i) {
        if (!SDL_IsGameController(i))
            continue;
            
        SDL_GameController* controller = SDL_GameControllerOpen(i);
        if (!controller) continue;

        pads[i].err = 0;  
        motes[i].err = 0; 

        if (want_pointer) {
            float dx = 0.0f, dy = 0.0f;
            
            if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO)) {
                SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, SDL_TRUE);
                float data[3];
                if (SDL_GameControllerGetSensorData(controller, SDL_SENSOR_GYRO, data, 3) == 0) {
                    dx = data[0] * cfg.gyro_sensitivity;
                    dy = data[1] * cfg.gyro_sensitivity;
                }
            } else {
                int rx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
                int ry = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
                
                if (std::abs(rx) > 8000) dx = (rx / 32767.0f);
                if (std::abs(ry) > 8000) dy = (ry / 32767.0f);
            }
            float speed = (mode == 4)
                ? 0.10f * cfg.gyro_sensitivity
                : 0.05f * cfg.pointer_speed;
            motes[i].ir_x += dx * speed;
            motes[i].ir_y += dy * speed;
            if (motes[i].ir_x < 0.0f) motes[i].ir_x = 0.0f;
            if (motes[i].ir_x > 1.0f) motes[i].ir_x = 1.0f;
            if (motes[i].ir_y < 0.0f) motes[i].ir_y = 0.0f;
            if (motes[i].ir_y > 1.0f) motes[i].ir_y = 1.0f;

            motes[i].accel_x = dx;
            motes[i].accel_y = dy;
            motes[i].accel_z = 1.0f;
        }

        // Official RVL_SDK Wiimote Buttons:
        // A (South) -> Button 2 (Jump) & Button A (Select)
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A)) {
            motes[i].buttons |= 0x0100; // WPAD_BUTTON_2
            motes[i].buttons |= 0x0800; // WPAD_BUTTON_A
        }
        // X (West) -> Button 1 (Attack / Dash)
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X)) {
            motes[i].buttons |= 0x0200; // WPAD_BUTTON_1
        }
        // B (East) -> Button B (Back / Bubble)
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B)) {
            motes[i].buttons |= 0x0400; // WPAD_BUTTON_B
        }
        // Y (North) -> Button 1 (Attack)
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y)) {
            motes[i].buttons |= 0x0200; // WPAD_BUTTON_1
        }

        // Shoulders / Triggers:
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) {
            motes[i].buttons |= 0x0200; // WPAD_BUTTON_1 (Sprint / Dash)
        }
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384) {
            motes[i].buttons |= 0x0400; // WPAD_BUTTON_B (Bubble / Back)
        }

        // System Buttons:
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START)) {
            motes[i].buttons |= 0x0010; // WPAD_BUTTON_PLUS
        }
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK)) {
            motes[i].buttons |= 0x1000; // WPAD_BUTTON_MINUS
        }
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_GUIDE)) {
            motes[i].buttons |= 0x8000; // WPAD_BUTTON_HOME
        }

        // D-Pad and Left Analog Stick:
        bool d_up    = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
        bool d_down  = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        bool d_left  = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        bool d_right = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

        int lx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (lx < -16000) d_left = true;
        if (lx >  16000) d_right = true;
        if (ly < -16000) d_up = true;
        if (ly >  16000) d_down = true;

        bool horiz = is_horizontal;
        if (const char *r = std::getenv("NWII_REMOTE")) {
            horiz = (std::string(r) == "horizontal");
        }
        if (horiz) {
            if (d_left)  motes[i].buttons |= 0x0008; // report UP    -> action Left
            if (d_right) motes[i].buttons |= 0x0004; // report DOWN  -> action Right
            if (d_up)    motes[i].buttons |= 0x0002; // report RIGHT -> action Up
            if (d_down)  motes[i].buttons |= 0x0001; // report LEFT  -> action Down
        } else {
            if (d_left)  motes[i].buttons |= 0x0001; // WPAD_BUTTON_LEFT
            if (d_right) motes[i].buttons |= 0x0002; // WPAD_BUTTON_RIGHT
            if (d_up)    motes[i].buttons |= 0x0008; // WPAD_BUTTON_UP
            if (d_down)  motes[i].buttons |= 0x0004; // WPAD_BUTTON_DOWN
        }

        // Classic Controller extension (WPAD_CL_* bits):
        {
            ClassicPadState &cl = motes[i].classic;
            cl = {};
            cl.connected = true;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A)) cl.buttons |= 0x0010; // A
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B)) cl.buttons |= 0x0040; // B
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X)) cl.buttons |= 0x0008; // X
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y)) cl.buttons |= 0x0020; // Y
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) cl.buttons |= 0x2000;  // L
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) cl.buttons |= 0x0200; // R
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START)) cl.buttons |= 0x0400;  // +
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK)) cl.buttons |= 0x1000;   // -
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_GUIDE)) cl.buttons |= 0x0800;  // Home
            if (d_up)    cl.buttons |= 0x0001;
            if (d_left)  cl.buttons |= 0x0002;
            if (d_down)  cl.buttons |= 0x4000;
            if (d_right) cl.buttons |= 0x8000;

            const int clt = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            const int crt = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            if (clt > 8000) cl.buttons |= 0x0080;  // ZL
            if (crt > 8000) cl.buttons |= 0x0004;  // ZR
            cl.lt = clt / 32767.0f;
            cl.rt = crt / 32767.0f;

            const int rx2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
            const int ry2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
            cl.lx = lx / 32767.0f;
            cl.ly = -ly / 32767.0f;
            cl.rx = rx2 / 32767.0f;
            cl.ry = -ry2 / 32767.0f;
        }

        // GameCube pad state:
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A)) pads[i].buttons |= 0x0100; 
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B)) pads[i].buttons |= 0x0200; 
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START)) pads[i].buttons |= 0x1000;  
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X)) pads[i].buttons |= 0x0400; 
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y)) pads[i].buttons |= 0x0800; 
        if (d_up)    pads[i].buttons |= 0x0008;
        if (d_down)  pads[i].buttons |= 0x0004;
        if (d_left)  pads[i].buttons |= 0x0001;
        if (d_right) pads[i].buttons |= 0x0002;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) pads[i].buttons |= 0x0040;  
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pads[i].buttons |= 0x0020; 
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) pads[i].buttons |= 0x0010; 

        if (std::abs(lx) > 8000) pads[i].stick_x = (int8_t)((lx / 32767.0f) * 127.0f);
        if (std::abs(ly) > 8000) pads[i].stick_y = (int8_t)((-ly / 32767.0f) * 127.0f); 
        
        if (!want_pointer) {
            int rx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
            int ry = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
            if (std::abs(rx) > 8000) pads[i].substick_x = (int8_t)((rx / 32767.0f) * 127.0f);
            if (std::abs(ry) > 8000) pads[i].substick_y = (int8_t)((-ry / 32767.0f) * 127.0f);
        }
        
        pads[i].trigger_l = (uint8_t)((SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f) * 255.0f);
        pads[i].trigger_r = (uint8_t)((SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f) * 255.0f);
    }
}

} 

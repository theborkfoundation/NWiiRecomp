#pragma once
#include <cstdint>

namespace nwii::runtime::input {

struct GameCubePadState {
    uint16_t buttons;
    int8_t stick_x, stick_y;
    int8_t substick_x, substick_y;
    uint8_t trigger_l, trigger_r;
    uint8_t analog_a, analog_b;
    int8_t err;
};

// Classic Controller extension state (WPAD_CL_* button bits).
struct ClassicPadState {
    uint16_t buttons;
    float lx, ly;      // left stick, -1..1
    float rx, ry;      // right stick, -1..1
    float lt, rt;      // analog triggers, 0..1
    bool connected;
};

struct WiimoteState {
    uint32_t buttons;
    float ir_x, ir_y; 
    float accel_x, accel_y, accel_z;
    float gyro_pitch, gyro_yaw, gyro_roll;
    int8_t err;
    ClassicPadState classic;
};

class IInputSource {
public:
    virtual ~IInputSource() = default;

    
    
    virtual void update(GameCubePadState pads[4], WiimoteState motes[4]) = 0;
};

} 

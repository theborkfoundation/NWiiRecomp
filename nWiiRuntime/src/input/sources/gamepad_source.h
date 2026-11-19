#pragma once
#include "input/input_source.h"
#include <array>

struct _SDL_GameController;

namespace nwii::runtime::input {

class GamepadSource : public IInputSource {
public:
    GamepadSource() = default;
    ~GamepadSource() override;

    void update(GameCubePadState pads[4], WiimoteState motes[4]) override;

private:
    std::array<_SDL_GameController *, 4> controllers_{};
};

} 

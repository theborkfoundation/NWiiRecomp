#include "input/sources/gamepad_source.h"
#include <SDL.h>
#include <cstdlib>

static void require(bool value) {
  if (!value) std::abort();
}

int main() {
  using namespace nwii::runtime::input;
  require(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
  const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                              SDL_CONTROLLER_AXIS_MAX,
                                              SDL_CONTROLLER_BUTTON_MAX, 0);
  require(index >= 0 && SDL_IsGameController(index));
  const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
  GameCubePadState pads[4]{};
  WiimoteState motes[4]{};
  {
    GamepadSource source;
    for (int i = 0; i < 20; ++i) source.update(pads, motes);
    require(SDL_GameControllerFromInstanceID(id) != nullptr);
  }
  // All updates share one reference, released when the source is destroyed.
  require(SDL_GameControllerFromInstanceID(id) == nullptr);
  require(SDL_JoystickDetachVirtual(index) == 0);
  SDL_Quit();
}

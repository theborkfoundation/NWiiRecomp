#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace nwii {
namespace runtime {

enum class Platform {
    Wii,
    GameCube,
    WiiU
};

enum class Backend {
    OpenGL,
    Metal
};

struct GameProfile {

    uint32_t callback_stack_top = 0;

    std::vector<std::string> disable_modules;

    
    std::string rpl_path;

    

    uint32_t ext_interrupt_dispatch = 0;

    

    uint32_t sched_lock_offset = 0;

    // The Bink decoder in this engine writes its YUV planes as linear rows and
    // the title samples them as GX textures. Decode I8 linearly for this game.
    bool linear_i8 = false;
    // Its chroma planes are 128-centred and the TEV setup has no bias, so the
    // half-range offset has to be applied when the plane is sampled.
    bool chroma_bias_i8 = false;
};

class Config {
public:
    static Config& get() {
        static Config instance;
        return instance;
    }

    void load(const std::string& filepath);

    
    void load_game_profile();

    Platform platform = Platform::Wii;
    Backend backend = Backend::OpenGL;
    bool enable_vsync = true;
    int window_width = 640;
    int window_height = 480;
    std::string game_dir;

    
    
    std::string game_id = "RSZK";

    

    

    
    int input_mode = 6;

    float gyro_sensitivity = 1.0f;

    float pointer_speed = 1.0f;

    int phone_port = 4313;

    GameProfile game_profile;

private:
    Config() = default;
};

} 
} 

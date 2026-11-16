#pragma once
#include "runtime/gx_state.h"
#include <string>

namespace nwii::runtime::gx {

enum class ShaderDialect {
    DesktopGL, // GLSL 3.30 core
    OpenGLES,  // GLSL ES 3.00
};

struct GeneratedShader {
    std::string vertex_source;
    std::string fragment_source;
};

GeneratedShader GenerateTEVShader(
    const GXState& state, uint8_t prim_type,
    ShaderDialect dialect = ShaderDialect::DesktopGL);

} 

// Shader dialect selection: desktop output stays GLSL 3.30, the ES profile
// emits GLSL ES 3.00 with explicit precision qualifiers.
#include "runtime/gx/tev_shader_gen.h"
#include "runtime/gx_state.h"

#include <cstdio>
#include <string>

using nwii::runtime::gx::GenerateTEVShader;
using nwii::runtime::gx::GXState;
using nwii::runtime::gx::ShaderDialect;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static bool starts_with(const std::string &text, const char *prefix) {
  return text.rfind(prefix, 0) == 0;
}

int main() {
  GXState state{};
  state.numTevStages = 1;
  state.tevStages[0].texMap = 0;
  state.tevStages[0].texCoord = 0;
  state.tevStages[0].colorOp = 0;
  state.tevStages[0].alphaOp = 0;
  state.texStages[0].format = 0;

  const auto gl = GenerateTEVShader(state, 0x80, ShaderDialect::DesktopGL);
  CHECK(starts_with(gl.vertex_source, "#version 330 core\n"));
  CHECK(starts_with(gl.fragment_source, "#version 330 core\n"));
  CHECK(gl.fragment_source.find("precision highp float") == std::string::npos);
  CHECK(gl.fragment_source.find("#version 300 es") == std::string::npos);

  const auto es = GenerateTEVShader(state, 0x80, ShaderDialect::OpenGLES);
  CHECK(starts_with(es.vertex_source, "#version 300 es\n"));
  CHECK(starts_with(es.fragment_source, "#version 300 es\n"));
  CHECK(es.vertex_source.find("precision highp float;") != std::string::npos);
  CHECK(es.fragment_source.find("precision highp float;") != std::string::npos);
  CHECK(es.fragment_source.find("precision highp int;") != std::string::npos);
  CHECK(es.fragment_source.find("precision highp sampler2D;") !=
        std::string::npos);
  CHECK(es.fragment_source.find("texture(uTex0, vTex0)") != std::string::npos);
  CHECK(es.fragment_source.find("FragColor = tevReg[0]") != std::string::npos);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("shader_dialect: all checks passed\n");
  return 0;
}

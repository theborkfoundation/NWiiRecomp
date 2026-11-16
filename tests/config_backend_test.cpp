// Backend selection: the documented "OpenGLES" spelling is accepted
// case-insensitively, OpenGL remains the default, and Metal keeps its existing
// fallback.
#include "runtime/config.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using nwii::runtime::Backend;
using nwii::runtime::Config;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void load_backend(const fs::path &path, const char *value) {
  std::ofstream out(path);
  out << "[graphics]\nbackend = \"" << value << "\"\n";
}

int main() {
  const fs::path root = fs::temp_directory_path() / "nwii_config_backend_test";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path cfg = root / "config.toml";

  auto &config = Config::get();

  for (const char *value : {"OpenGLES", "opengles", "OpenGLEs"}) {
    load_backend(cfg, value);
    config.backend = Backend::OpenGL;
    config.load(cfg.string());
    CHECK(config.backend == Backend::OpenGLES);
  }

  load_backend(cfg, "OpenGL");
  config.backend = Backend::OpenGLES;
  config.load(cfg.string());
  CHECK(config.backend == Backend::OpenGL);

  // Metal is still routed to the desktop GL fallback until it exists.
  load_backend(cfg, "Metal");
  config.backend = Backend::OpenGLES;
  config.load(cfg.string());
  CHECK(config.backend == Backend::OpenGL);

  load_backend(cfg, "something-else");
  config.backend = Backend::OpenGLES;
  config.load(cfg.string());
  CHECK(config.backend == Backend::OpenGL);

  fs::remove_all(root);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("config_backend: all checks passed\n");
  return 0;
}

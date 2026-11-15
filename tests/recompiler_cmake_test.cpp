// Generated projects must default to an optimized single-config build while
// leaving multi-config generators and explicit build types alone.
#include "analyzer/analyzer.h"
#include "loader/loader.h"
#include "recompiler/recompiler.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "nwii_recompiler_cmake_test";
  const fs::path runtime = root / "runtime";
  const fs::path output = root / "export";
  fs::remove_all(root);
  fs::create_directories(runtime);
  {
    std::ofstream dummy(runtime / "dummy.cpp");
    dummy << "// placeholder runtime source\n";
  }

  nwii::loader::Executable image;
  nwii::analyzer::Analyzer analyzer(image);
  nwii::recomp::RecompilerConfig config;
  config.project_name = "TestProject";
  config.output_dir = output.string();
  config.runtime_source_dir = runtime.string();
  nwii::recomp::Recompiler compiler(analyzer, nullptr, config);

  if (!compiler.generate_cmake_project(0)) {
    std::cerr << "CMake project generation failed\n";
    fs::remove_all(root);
    return 1;
  }

  std::ifstream in(output / "CMakeLists.txt");
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  bool ok = true;
  auto require = [&](bool condition, const char *what) {
    if (!condition) {
      std::cerr << "missing: " << what << "\n";
      ok = false;
    }
  };
  require(text.find("project(TestProject LANGUAGES C CXX)") != std::string::npos,
          "project()");
  require(text.find("if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)") !=
              std::string::npos,
          "single-config guard");
  require(text.find("set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" "
                    "FORCE)") != std::string::npos,
          "Release default");
  require(text.find("set(CMAKE_CXX_EXTENSIONS ON)") != std::string::npos,
          "GNU extensions for the resume dispatch");
  require(fs::exists(output / "main_output.cpp"), "main_output.cpp");

  fs::remove_all(root);
  if (!ok)
    return 1;
  std::printf("recompiler_cmake: all checks passed\n");
  return 0;
}

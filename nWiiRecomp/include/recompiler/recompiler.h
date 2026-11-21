#pragma once
#include "analyzer/analyzer.h"
#include "recompiler/symbols.h"
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace nwii {
namespace recomp {

struct RecompilerConfig {
    std::string project_name = "RecompiledGame";
    std::string input_game_dir = "";
    std::string output_dir = "export";
    std::string runtime_source_dir = "../nWiiRuntime";
    std::string symbols_csv = "";

    // Legacy single-file output was removed; kept so existing configs still
    // parse. Split output is always used.
    bool split_output = true;
    int instructions_per_file = 20000;

    

    std::map<uint32_t, std::string> hle_hooks;

    // Optional hooks: called as `if (hook(ctx)) { ctx.pc = ctx.lr; return; }`
    // before the guest body runs, so a native fast path can take over while the
    // original code stays as the fallback. Unlike hle_hooks, this does not
    // remove the function from the dispatch range table, so it is safe when the
    // native handler can decline a call.
    std::map<uint32_t, std::string> optional_hooks;

    // Explicit per-title C++ replacements; labels and instruction accounting
    // are retained so interrupted functions can still resume at patched sites.
    std::map<uint32_t, std::string> instruction_overrides;

    

    struct RelAot { std::string path; uint32_t module_base; uint32_t bss_base; };
    std::vector<RelAot> rel_aot;
};

class Recompiler {
public:
    Recompiler(const analyzer::Analyzer& analyzer, const SymbolTable* symbols = nullptr, const RecompilerConfig& config = RecompilerConfig());

    std::vector<std::string> generate_cpp(uint32_t entry_point);

    bool generate_cmake_project(uint32_t entry_point);

    std::string generate_function_cpp(const analyzer::Function& func);

private:
    void emit_function(std::ostream& out, const analyzer::Function& func,
                       const std::string& func_name,
                       std::set<std::string>* cross_refs = nullptr);
    void emit_instruction(std::ostream& out, const analyzer::Instruction& inst,
                          const analyzer::Function& func,
                          const std::unordered_set<uint32_t>& instruction_addresses,
                          std::set<std::string>* cross_refs);

    const analyzer::Analyzer& analyzer_;
    const SymbolTable* symbols_;
    RecompilerConfig config_;
};

} 
} 

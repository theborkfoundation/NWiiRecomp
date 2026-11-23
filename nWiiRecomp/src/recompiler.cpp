#include "recompiler/recompiler.h"
#include "ppc/instruction.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <algorithm>

namespace nwii {
namespace recomp {

Recompiler::Recompiler(const analyzer::Analyzer &analyzer,
                       const SymbolTable *symbols,
                       const RecompilerConfig &config)
    : analyzer_(analyzer), symbols_(symbols), config_(config) {}

static const std::unordered_set<std::string> &hle_handlers() {
  // Native handlers that actually exist in nWiiRuntime. Binding is by symbol
  // name, so any game whose symbols the analyzer can name gets the whole seam
  // with no per-game configuration; [hle_hooks] in the TOML stays available for
  // overrides and for games where a symbol is missed.
  //
  // Deliberately conservative: only handlers that have been run and checked in
  // a real game are listed. Binding DVDReadAsyncPrio automatically loaded the
  // game's DLL as zeros and gave a black screen, so disc, audio, IOS and the
  // remaining VI entry points stay out until each is verified on its own. They
  // can still be forced per game through [hle_hooks].
  //
  // GXLoadTexMtxImm and GXCallDisplayList are deliberately absent: both are
  // implemented but broke rendering in Mario Party 7, so they stay opt-in via
  // the TOML until that is understood.
  static const std::unordered_set<std::string> s = {
      // State setters must execute the guest SDK: they also update GXData's
      // cached registers/dirty flags, which later guest setters read. Emitting
      // only a native FIFO write loses those changes (e.g. TEV stage count).
      "GXLoadNrmMtxImm",
      "GXLoadPosMtxImm",
      "OSAllocFromHeap",
      "OSCreateHeap",
      "OSFreeToHeap",
      "OSInitAlloc",
      "OSReport",
      "PADInit",
      "PADRead",
      "VIWaitForRetrace",
  };
  return s;
}

// Symbols out of the signature database can carry the library they came from,
// e.g. "GXSetCullMode\tgx.a GXGeometry.o" -- match on the bare name.
static std::string bare_symbol(const std::string &name) {
  size_t cut = name.find_first_of(" \t");
  if (cut == std::string::npos) {
    // By this point the name has usually been sanitised into an identifier,
    // so the separator that followed the function name is now "__".
    cut = name.find("__", 1);
  }
  std::string out = cut == std::string::npos ? name : name.substr(0, cut);
  // Symbols also arrive carrying their own address, e.g. GXSetZMode_800C0CC0.
  if (out.size() > 9) {
    const size_t us = out.size() - 9;
    if (out[us] == '_' &&
        out.find_first_not_of("0123456789ABCDEFabcdef", us + 1) ==
            std::string::npos)
      out.resize(us);
  }
  return out;
}

static bool is_hle_function(const std::string &name) {
  return hle_handlers().count(bare_symbol(name)) != 0;
}

static std::string configured_hook(const analyzer::Function &func,
                                   const RecompilerConfig &config) {
  auto it = config.hle_hooks.find(func.start_address);
  if (it != config.hle_hooks.end())
    return it->second;
  return {};
}

// Hook that may decline a call at run time; the guest body stays as fallback.
static std::string configured_optional_hook(const analyzer::Function &func,
                                            const RecompilerConfig &config) {
  auto it = config.optional_hooks.find(func.start_address);
  return it == config.optional_hooks.end() ? std::string() : it->second;
}

// Handler selected by the analyzer (instruction signature) or the automatic
// name allowlist. These functions are structural HLE: excluded from the guest
// range table and dispatched through the exact-address switch.
static std::string auto_hle_handler(const analyzer::Function &func,
                                    const std::string &base_name) {
  if (!func.hle_hook_name.empty())
    return func.hle_hook_name;
  if (is_hle_function(base_name))
    return base_name;
  return {};
}

// Explicit [hle_hooks] configuration takes precedence over the automatic
// handler, so an override is never silently shadowed.
static std::string effective_handler(const analyzer::Function &func,
                                     const std::string &base_name,
                                     const RecompilerConfig &config) {
  if (std::string hook = configured_hook(func, config); !hook.empty())
    return hook;
  return auto_hle_handler(func, base_name);
}


// Eight-digit uppercase hex, matching the loc_<address> label format.
static std::string hex8(uint32_t value) {
  std::stringstream ss;
  ss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
  return ss.str();
}

std::vector<std::string> Recompiler::generate_cpp(uint32_t entry_point) {
  std::vector<std::string> generated_files;

  std::cout << "[Recompiler] Starting C++ generation. Total functions to emit: "
            << analyzer_.get_functions().size() << "\n";

  auto emit_headers = [](std::ostream &out) {
    out << "#include \"runtime/cpu_context.h\"\n";
    out << "#include \"runtime/config.h\"\n";
    out << "#include <stdint.h>\n";
    out << "#include <bit>\n";
    out << "#include <thread>\n";
    out << "#include <chrono>\n";

    out << "#include <cmath>\n";
    out << "#include <iostream>\n";
    out << "#include <cstdlib>\n\n";
    out << "using namespace nwii::runtime;\n\n";
    // Resume dispatch tables store label addresses as offsets from
    // loc_resume_body. Taking a label's address is a GNU extension supported by
    // GCC and Clang; every supported host compiler provides it. The offsets are
    // signed because the compiler may lay the base block out after some of the
    // instruction blocks.
    out << "#define NWII_RESUME_LOC(A) (int32_t)((const char*)&&loc_##A - "
           "(const char*)&&loc_resume_body)\n";
    out << "#define NWII_RESUME_DEFAULT "
           "(int32_t)((const char*)&&loc_resume_default - "
           "(const char*)&&loc_resume_body)\n\n";
  };

  // Split output is the only supported mode: the legacy single-file path
  // emitted a dispatcher with one case per guest instruction and did not
  // scale. `split_output` remains in the config for compatibility.
  {
    
    std::string header_path = config_.output_dir + "/functions.h";
    std::ofstream hout(header_path);
    hout << "#pragma once\n";
    hout << "#include \"runtime/cpu_context.h\"\n\n";
    hout << "extern \"C\" {\n";
    hout << "void OSReport(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Open(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_OpenAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Close(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Read(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctl(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctlv(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlvAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosFree(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigure(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigurePan(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VISetBlack(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIGetNextField(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIWaitForRetrace(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSDisableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSEnableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSRestoreInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetTime(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSTicksToMilliseconds(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoBase(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoPtrs(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXGetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCopyClear(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXBegin(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXEnd(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxDesc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxAttrFmt(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadPosMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXCallDisplayList(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadNrmMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadTexMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCurrentMtx(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Write(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Seek(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_CloseAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_ReadAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_WriteAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_SeekAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXAcquireVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXFreeVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceState(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceMix(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceAdpcm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceSrc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceOffsets(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDOpen(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVD_Callback(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadAsyncPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDClose(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDGetDriveStatus(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void KPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInitAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSCreateHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSAllocFromHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSFreeToHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetCurrentThread(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSleepThread(nwii::runtime::CPUContext& ctx);\n";
    hout << "}\n";

    std::set<std::string> emitted_names;
    std::vector<std::string> all_func_names;
    // The symbol name before any uniquifying suffix. The seam matches on this;
    // the wrapper itself must never carry it, or it would collide with the
    // native handler's own symbol at link time.
    std::vector<std::string> all_base_names;

    hout << "extern \"C\" {\n";
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name;
      if (symbols_ && symbols_->has_symbol(start_addr)) {
        func_name = symbols_->get_symbol(start_addr);
      } else {
        std::stringstream ss;
        ss << "func_" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << start_addr;
        func_name = ss.str();
      }
      all_base_names.push_back(bare_symbol(func_name));
      if (emitted_names.count(func_name)) {
        std::stringstream ss;
        ss << func_name << "_" << std::hex << std::uppercase << start_addr;
        func_name = ss.str();
      }
      emitted_names.insert(func_name);
      all_func_names.push_back(func_name);

      hout << "void " << func_name << "(nwii::runtime::CPUContext& ctx);\n";
    }
    hout << "}\n";
    hout.close();

    int file_idx = 0;
    int func_idx = 0;
    int count_in_current_file = 0;
    int functions_in_current_file = 0;
    std::ostringstream unit_body;
    std::set<std::string> unit_declarations;

    // Each split unit declares only the functions it defines or calls. The
    // complete guest declaration list (functions.h) is 1.5 MB and every unit
    // used to parse it; only main_output.cpp still needs all of it.
    auto flush_unit = [&]() {
      std::stringstream ss;
      ss << "output_" << file_idx++ << ".cpp";
      std::string fname = ss.str();
      generated_files.push_back(fname);
      std::ofstream out(config_.output_dir + "/" + fname);
      emit_headers(out);
      // Definitions in this unit must see a C-linkage declaration first, and
      // cross-unit calls are declared here instead of through functions.h.
      out << "extern \"C\" {\n";
      for (const auto &name : unit_declarations)
        out << "void " << name << "(CPUContext&);\n";
      out << "}\n\n";
      out << unit_body.str();
      out.close();
      std::cout << "[Recompiler] Generated " << fname << "\n";
      unit_body.str("");
      unit_body.clear();
      unit_declarations.clear();
      functions_in_current_file = 0;
    };

    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      const std::string &base_name = all_base_names[func_idx++];
      const bool structural_hle = !auto_hle_handler(func, base_name).empty();
      unit_declarations.insert(func_name);
      if (structural_hle) {
        const std::string hle_handler =
            effective_handler(func, base_name, config_);
        // Wrapper with a distinct name. When the generated name already
        // equals the handler name no wrapper can be emitted; those functions
        // are addressed by their runtime symbol directly.
        if (hle_handler != func_name) {
          unit_body
              << "void " << func_name << "(nwii::runtime::CPUContext& ctx) {\n";
          if (!configured_hook(func, config_).empty()) {
            // Configured hooks are ordinary C++ functions.
            unit_body << "    extern void " << hle_handler
                      << "(nwii::runtime::CPUContext&);\n";
          } else {
            // Runtime HLE handlers have C linkage; declare them at file scope
            // (block-scope extern "C" is not accepted by Clang).
            unit_declarations.insert(hle_handler);
          }
          unit_body << "    " << hle_handler << "(ctx);\n";
          unit_body << "    ctx.pc = ctx.lr;\n";
          unit_body << "}\n\n";
        }
      } else {
        emit_function(unit_body, func, func_name, &unit_declarations);
      }

      ++functions_in_current_file;
      count_in_current_file += func.instructions.size();
      if (count_in_current_file >= config_.instructions_per_file) {
        flush_unit();
        count_in_current_file = 0;
      }
    }
    if (functions_in_current_file > 0)
      flush_unit();

    std::ofstream out;

    std::string main_name = "main_output.cpp";
    generated_files.push_back(main_name);
    out.open(config_.output_dir + "/" + main_name);
    emit_headers(out);
    // The dispatcher used to be a switch with one case per guest instruction,
    // which was unbuildable at -O3. It is now a small table lookup, so let it
    // be optimized like everything else.
    out << "#include \"functions.h\"\n\n";

    std::string entry_func;
    if (symbols_ && symbols_->has_symbol(entry_point)) {
      entry_func = symbols_->get_symbol(entry_point);
    } else {
      std::stringstream ss;
      ss << "func_" << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << entry_point;
      entry_func = ss.str();
    }

    out << "// --- Function Bounds Table ---\n";
    out << "struct FuncBound { uint32_t start; uint32_t end; "
           "void(*func)(CPUContext&); };\n";
    out << "static const FuncBound g_func_bounds[] = {\n";

    // The old dispatch switch listed every branch-link return address,
    // backward-branch source and jump-table target explicitly. Fold those into
    // each function's range so one binary search can replace the switch.
    std::map<uint32_t, uint32_t> cover_end;
    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      ++func_idx;
      bool is_hle =
          !auto_hle_handler(func, all_base_names[func_idx - 1]).empty();
      if (is_hle)
        continue;
      uint32_t hi = func.end_address;
      std::unordered_set<uint32_t> function_addresses;
      function_addresses.reserve(func.instructions.size());
      for (const auto &inst : func.instructions)
        function_addresses.insert(inst.address);
      for (const auto &inst : func.instructions) {
        ppc::Instruction ppc_inst(inst.opcode);
        if (ppc_inst.is_branch_link() || ppc_inst.opcode() == 17) {
          uint32_t ret = inst.address + 4;
          if (ret > hi)
            hi = ret;
        }
        if (ppc_inst.opcode() == 16 || ppc_inst.opcode() == 18) {
          uint32_t t = ppc_inst.branch_target(inst.address);
          bool is_local = function_addresses.count(t) != 0;
          if (t <= inst.address && is_local && inst.address + 4 > hi)
            hi = inst.address + 4;
        }
      }
      for (uint32_t jt : func.jump_table_targets)
        if (jt + 4 > hi)
          hi = jt + 4;
      cover_end[start_addr] = hi;
    }

    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle =
          !auto_hle_handler(func, all_base_names[func_idx]).empty();
      ++func_idx;
      if (!is_hle) {
        out << "    {0x" << std::hex << std::uppercase << start_addr << ", 0x"
            << cover_end[start_addr] << ", " << func_name << "},\n";
      }
    }
    out << "    {0, 0, nullptr}\n";
    out << "};\n\n";

    // O(1) dispatch for cross-function calls. The default branch used to
    // binary-search g_func_bounds on every call: 15 dependent loads through a
    // ~350 KB table, measured at ~146 ns per lookup on an A53. A flat pointer
    // table indexed by (target - base) >> 2 is ~21 ns even at 12.7 MB, because
    // the search's dependent loads miss on every step while this is one load.
    // The table is filled at startup from g_func_bounds, so it can live in
    // .bss instead of costing 13 MB in the object file.
    {
      uint32_t limit = 0;
      for (const auto &kv : cover_end)
        if (kv.first >= 0x80000000u && kv.second > limit)
          limit = kv.second;
      limit = (limit + 0xFFFFu) & ~0xFFFFu;  // round up, keeps the array sane
      out << "// --- Dense Dispatch Table ---\n";
      out << "static constexpr uint32_t kDispatchBase = 0x80000000u;\n";
      out << "static constexpr uint32_t kDispatchLimit = 0x" << std::hex
          << std::uppercase << limit << "u;\n";
      out << "static constexpr size_t kDispatchSlots =\n"
             "    (kDispatchLimit - kDispatchBase) >> 2;\n";
      out << "static void (*s_dispatch_table[kDispatchSlots])(CPUContext&);\n\n";
      // One-off fill from the bounds table. Later (inner) functions overwrite
      // the entries of any enclosing range, matching what the binary search
      // resolves: it picks the last range whose start <= target.
      out << "static void nwii_fill_dispatch_table() {\n";
      out << "  for (const FuncBound &b : g_func_bounds) {\n";
      out << "    if (!b.func || b.start < kDispatchBase || b.start >= kDispatchLimit)\n";
      out << "      continue;\n";
      out << "    uint32_t end = b.end < kDispatchLimit ? b.end : kDispatchLimit;\n";
      out << "    for (uint32_t a = b.start; a < end; a += 4)\n";
      out << "      s_dispatch_table[(a - kDispatchBase) >> 2] = b.func;\n";
      out << "  }\n";
      out << "}\n\n";
    }

    out << "// --- Entry Point Wrapper ---\n";
    out << "extern \"C\" void run_game(nwii::runtime::CPUContext& ctx) {\n";
    out << "    static const bool s_dispatch_ready = [] {\n";
    out << "        nwii_fill_dispatch_table();\n";
    out << "        return true;\n";
    out << "    }();\n";
    out << "    (void)s_dispatch_ready;\n";
    out << "    static uint32_t pc_history[10] = {0};\n";
    out << "    static int pc_history_idx = 0;\n";
    out << "    if (ctx.pc == 0) ctx.pc = 0x" << std::hex << std::uppercase
        << entry_point << std::dec << ";\n";
    out << "    while (ctx.is_running) {\n"
           "      pc_history[pc_history_idx] = ctx.pc;\n"
           "      pc_history_idx = (pc_history_idx + 1) % 10;\n"
           "      if (ctx.pc == 0) {\n"
           "          static bool warned_zero = false;\n"
           "          if (!warned_zero) {\n"
           "              warned_zero = true;\n"
           "              std::cout << \"[run_game] PC=0 (branch to NULL), last PCs:\";\n"
           "              for (int i = 0; i < 10; ++i)\n"
           "                  std::cout << \" 0x\" << std::hex << pc_history[(pc_history_idx + 9 - i) % 10];\n"
           "              std::cout << \" lr=0x\" << ctx.lr << std::dec << std::endl;\n"
           "          }\n"
           "          if ((ctx.msr & 0x8000) == 0) ctx.msr |= 0x8000; // Force "
           "EE=1 in idle\n"
           "          process_pending_callbacks(ctx);\n"
           "          if (ctx.pc == 0) {\n"
           "              "
           "std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "          }\n"
           "          continue;\n"
           "      }\n";
    out << "      if (_setjmp(ctx.exception_jmp_buf) == 0) {\n";
    out << "        process_pending_callbacks(ctx);\n";
    out << "        if (ctx.pc == 0xFFFFFFFC) {\n";
    out << "            if (!ctx.backup_stack.empty()) {\n";
    out << "                auto& bk = ctx.backup_stack.top();\n";
    out << "                ctx.gpr = bk.gpr; ctx.fpr = bk.fpr; ctx.ps1 = "
           "bk.ps1;\n";
    out << "                ctx.cr = bk.cr; ctx.lr = bk.lr; ctx.ctr = "
           "bk.ctr;\n";
    out << "                ctx.xer = bk.xer; ctx.pc = bk.pc;\n"
           "                ctx.srr0 = bk.srr0; ctx.srr1 = bk.srr1;\n"
           "                ctx.msr = bk.msr; ctx.fpscr = bk.fpscr;\n"
           "                ctx.gqr = bk.gqr; ctx.sprg = bk.sprg;\n";
    out << "                ctx.backup_stack.pop();\n";
    out << "            }\n";
    out << "            ctx.callback_depth--;\n";
    out << "            if (ctx.callback_depth <= 0) {\n";
    out << "                ctx.in_callback = false;\n";
    out << "                ctx.callback_depth = 0;\n";
    out << "            }\n";
    out << "            continue;\n";
    out << "        }\n";
    out << "        if ((ctx.pc & 0xF0000000) == 0xC0000000) ctx.pc = (ctx.pc "
           "& 0x0FFFFFFF) | 0x80000000;\n";
    out << "        else if ((ctx.pc & 0xF0000000) == 0xD0000000) ctx.pc = "
           "(ctx.pc & 0x0FFFFFFF) | 0x90000000;\n";
    out << "        else if (ctx.pc < 0x80000000) ctx.pc |= 0x80000000;\n";
    out << "        uint32_t target = ctx.pc;\n";
    out << "        switch (target) {\n";
    // HLE functions dispatch by exact address (their bodies live in the
    // runtime); everything else goes through the range table. Keep this list
    // tiny: the generated dispatcher is compiled at -O0, so a switch with tens
    // of thousands of cases is far slower than the O(log n) lookup below.
    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle =
          !auto_hle_handler(func, all_base_names[func_idx]).empty();
      ++func_idx;
      if (!is_hle)
        continue;
      // Match the historic dispatch target: the analyzer-provided handler
      // when there is one, otherwise the generated wrapper. A configured hook
      // always goes through the wrapper so the override is honoured.
      std::string dispatch_target = func_name;
      if (configured_hook(func, config_).empty() &&
          !func.hle_hook_name.empty())
        dispatch_target = func.hle_hook_name;
      out << "            case 0x" << std::hex << std::uppercase << start_addr
          << std::dec << ": {\n";
      out << "                " << dispatch_target
          << "(ctx); ctx.pc = ctx.lr; break;\n";
      out << "            }\n";
    }
    out << "            default: {\n";
    out << "                // O(1) for anything inside the recompiled image. The\n";
    out << "                // bounds search below stays for low-memory code\n";
    out << "                // (trampolines, VIDisc) and out-of-range targets.\n";
    out << "                if (target >= kDispatchBase && target < kDispatchLimit) {\n";
    out << "                    void (*fn)(CPUContext&) =\n";
    out << "                        s_dispatch_table[(target - kDispatchBase) >> 2];\n";
    out << "                    if (__builtin_expect(fn != nullptr, 1)) {\n";
    out << "                        ctx.pc = target;\n";
    out << "                        fn(ctx);\n";
    out << "                        break;\n";
    out << "                    }\n";
    out << "                }\n";
    out << "                // g_func_bounds is sorted by start (sentinel last).\n";
    out << "                size_t lo = 0, hi = sizeof(g_func_bounds) / "
           "sizeof(g_func_bounds[0]) - 1;\n";
    out << "                while (lo < hi) {\n";
    out << "                    size_t mid = (lo + hi) / 2;\n";
    out << "                    if (g_func_bounds[mid].start <= target) lo = mid + 1;\n";
    out << "                    else hi = mid;\n";
    out << "                }\n";
    out << "                const FuncBound *fb = nullptr;\n";
    out << "                if (lo > 0 && target < g_func_bounds[lo - 1].end)\n";
    out << "                    fb = &g_func_bounds[lo - 1];\n";
    out << "                if (fb) {\n";
    out << "                    ctx.pc = target;\n";
    out << "                    fb->func(ctx);\n";
    out << "                } else {\n";
    out << "                    // Not in any recompiled function: runtime-generated\n";
    out << "                    // code (copied to low memory, trampolines). Interpret.\n";
    out << "                    nwii::runtime::interpret_step(ctx);\n";
    out << "                }\n";
    out << "            }\n";
    out << "        }\n";
    out << "      } else {\n";
    out << "          continue;\n";
    out << "      }\n";
    out << "    }\n";
    out << "    std::cout << \"[DEBUG] PC History before 0x0:\\n\";\n";
    out << "    for (int i=0; i<10; ++i) {\n";
    out << "        std::cout << \"  0x\" << std::hex << "
           "pc_history[(pc_history_idx + i) % 10] << \"\\n\";\n";
    out << "    }\n";
    out << "}\n";
    out.close();
    std::cout << "[Recompiler] Generated " << main_name << "\n";
  }

  return generated_files;
}

bool Recompiler::generate_cmake_project(uint32_t entry_point) {
  if (!std::filesystem::exists(config_.output_dir)) {
    std::filesystem::create_directories(config_.output_dir);
  }

  std::vector<std::string> generated_files = generate_cpp(entry_point);
  if (generated_files.empty()) {
    return false;
  }

  // Copy the runtime sources, and only the sources.
  //
  // A plain recursive copy takes the source tree's own `build` directory with
  // it — CMake caches, object files, fetched dependencies and their `.git`
  // packs. Those are read-only inside git object stores, so the copy dies on
  // the first pack file and the whole export fails. It also happens to be the
  // last step, so everything generated up to that point was silently thrown
  // away, including the SDK symbol report.
  std::string runtime_dest = config_.output_dir + "/nWiiRuntime";
  try {
    namespace fs = std::filesystem;
    const fs::path src = config_.runtime_source_dir;
    const auto opts = fs::copy_options::overwrite_existing;

    auto skip = [](const fs::path &rel) {
      for (const auto &part : rel)
      {
        const std::string s = part.string();
        if (s == "build" || s == ".git" || s == ".cache" || s == ".DS_Store")
          return true;
      }
      return false;
    };

    fs::create_directories(runtime_dest);
    for (auto it = fs::recursive_directory_iterator(src);
         it != fs::recursive_directory_iterator(); ++it)
    {
      const fs::path rel = fs::relative(it->path(), src);
      if (skip(rel))
      {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      const fs::path dest = fs::path(runtime_dest) / rel;
      if (it->is_directory())
        fs::create_directories(dest);
      else if (it->is_regular_file())
        fs::copy_file(it->path(), dest, opts);
    }
  } catch (const std::exception &e) {
    std::cerr << "Failed to copy runtime from " << config_.runtime_source_dir
              << ": " << e.what() << "\n";
    return false;
  }

  std::string cmake_path = config_.output_dir + "/CMakeLists.txt";
  std::ofstream out(cmake_path);
  if (!out.is_open())
    return false;

  out << "cmake_minimum_required(VERSION 3.20)\n";

  
  out << "project(" << config_.project_name << " LANGUAGES C CXX)\n\n";
  // Generated code carries per-instruction bookkeeping and large dispatch
  // structures, so an unoptimized build is dramatically slower. Default to
  // Release only for single-config generators and only when the user has not
  // chosen a build type; multi-config generators select the config at build
  // time and must keep CMAKE_BUILD_TYPE empty.
  out << "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n";
  out << "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)\n";
  out << "endif()\n\n";
  out << "set(CMAKE_POLICY_VERSION_MINIMUM 3.5)\n";
  out << "set(CMAKE_CXX_STANDARD 20)\n";
  out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
  // Translated functions resume at interior PCs through a table of label
  // addresses, which is a GCC/Clang extension.
  out << "set(CMAKE_CXX_EXTENSIONS ON)\n\n";

  
  
  out << "include(FetchContent)\n";
  out << "FetchContent_Declare(\n";
  out << "    tomlplusplus\n";
  out << "    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git\n";
  out << "    GIT_TAG        v3.4.0\n";
  out << ")\n";
  out << "FetchContent_MakeAvailable(tomlplusplus)\n\n";

  // Fetch SDL2 2.28.5 by default, or use the target's SDL2 through pkg-config
  // with -DNWII_SYSTEM_SDL2=ON (the Linux ARM64 cross-build path).
  out << "# SDL2: fetched by default, or the target's system SDL2 with\n";
  out << "# -DNWII_SYSTEM_SDL2=ON.\n";
  out << "include(\"${CMAKE_CURRENT_SOURCE_DIR}/nWiiRuntime/cmake/sdl.cmake\")\n\n";

  out << "add_subdirectory(nWiiRuntime)\n\n";

  out << "add_executable(" << config_.project_name
      << " nWiiRuntime/src/core/main.cpp";
  for (const auto &file : generated_files) {
    out << " " << file;
  }
  out << ")\n";

  out << "target_include_directories(" << config_.project_name
      << " PRIVATE nWiiRuntime/src nWiiRuntime/include)\n";

  
  out << "target_link_libraries(" << config_.project_name
      << " PRIVATE nwiiruntime)\n";

  return true;
}

std::string Recompiler::generate_function_cpp(const analyzer::Function &func) {
  std::stringstream ss;

  // Standalone bodies (tests) do not come with the unit preamble, so provide
  // the resume-dispatch macros here as well.
  ss << "#ifndef NWII_RESUME_LOC\n";
  ss << "#define NWII_RESUME_LOC(A) (int32_t)((const char*)&&loc_##A - "
        "(const char*)&&loc_resume_body)\n";
  ss << "#define NWII_RESUME_DEFAULT "
        "(int32_t)((const char*)&&loc_resume_default - "
        "(const char*)&&loc_resume_body)\n";
  ss << "#endif\n\n";

  std::string func_name;
  if (symbols_ && symbols_->has_symbol(func.start_address)) {
    func_name = symbols_->get_symbol(func.start_address);
  } else {
    std::stringstream ss_name;
    ss_name << "func_" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(8) << func.start_address;
    func_name = ss_name.str();
  }

  emit_function(ss, func, func_name);
  return ss.str();
}

void Recompiler::emit_function(std::ostream &out,
                               const analyzer::Function &func,
                               const std::string &func_name,
                               std::set<std::string> *cross_refs) {
  const std::string optional_hook = configured_optional_hook(func, config_);
  if (!optional_hook.empty()) {
    // Emit the declaration inside the hook's own namespace. A qualified
    // `extern bool ns::f(...)` at file scope is only legal as a redeclaration,
    // and a block-scope one would land in the global namespace instead.
    const auto pos = optional_hook.rfind("::");
    if (pos == std::string::npos) {
      out << "extern bool " << optional_hook
          << "(nwii::runtime::CPUContext&);\n";
    } else {
      out << "namespace " << optional_hook.substr(0, pos) << " {\n";
      out << "bool " << optional_hook.substr(pos + 2)
          << "(nwii::runtime::CPUContext&);\n";
      out << "}\n";
    }
  }
  out << "void " << func_name << "(CPUContext& ctx) {\n";

  if (!optional_hook.empty()) {
    // Guard on the entry PC: generated functions also carry interior resume
    // labels, and an interrupt can resume mid-function, which must not
    // re-enter the hook.
    out << "    if (ctx.pc == 0x" << std::hex << std::uppercase
        << func.start_address << std::dec << " && " << optional_hook
        << "(ctx)) { ctx.pc = ctx.lr; return; }\n";
  }

  // Explicit [hle_hooks] configuration replaces the guest body. Non-auto-HLE
  // functions stay in the dispatcher range table (like before), so callers
  // that jump into the function still land on the hook wrapper.
  if (std::string hook = configured_hook(func, config_); !hook.empty()) {
    out << "    extern void " << hook << "(CPUContext&);\n";
    out << "    " << hook << "(ctx);\n";
    out << "    ctx.pc = ctx.lr; return;\n";
    out << "}\n\n";
    return;
  }

  
  

  

  
  
  // Cache maintenance is meaningless without a guest cache model, and the
  // dcbf/dcbst loops can spin for enormous or bogus ranges (the streaming
  // code passes buffer pointers that wrap). Compile them to a return.
  if (func_name.rfind("DCFlushRange", 0) == 0 ||
      func_name.rfind("DCInvalidateRange", 0) == 0 ||
      func_name.rfind("DCStoreRange", 0) == 0) {
    out << "    // cache maintenance: no-op without a guest cache\n";
    out << "    ctx.pc = ctx.lr; return;\n";
    out << "}\n\n";
    return;
  }

  bool all_zero = !func.instructions.empty();
  for (const auto &inst : func.instructions) {
    if (inst.opcode != 0) {
      all_zero = false;
      break;
    }
  }
  bool os_low_mem = (func.start_address & 0x3FFFFFFF) < 0x3100;
  if (all_zero || os_low_mem) {
    out << "    nwii::runtime::interpret_step(ctx);\n";
    out << "}\n\n";
    return;
  }

  
  out << "    if (nwii::runtime::g_trace_calls && ctx.pc == 0x" << std::hex
      << std::uppercase << func.start_address << std::dec
      << ") nwii::runtime::trace_call(0x" << std::hex << std::uppercase
      << func.start_address << std::dec << ", ctx);\n";

  std::unordered_set<uint32_t> valid_addrs;
  valid_addrs.reserve(func.instructions.size());
  uint32_t last_addr = func.start_address;
  for (const auto &inst : func.instructions) {
    valid_addrs.insert(inst.address);
    if (inst.address > last_addr)
      last_addr = inst.address;
  }

  // Accumulate the guest instruction count in a local instead of doing a
  // read-modify-write on ctx.inst_count at every instruction. That is a
  // load/add/store through the context pointer at all ~1.8M emitted sites, and
  // because the member is reachable from every guest store it can never be
  // kept in a register. The count is only *read* at the poll points (backward
  // branches) and by the NWII_INSTTB timebase, so a local keeps the same
  // observable value wherever it is observed, and the destructor covers every
  // early return.
  out << "    struct NwiiInstCount {\n";
  out << "        CPUContext& c; uint64_t n = 0;\n";
  out << "        void flush() { if (n) { c.inst_count += n; n = 0; } }\n";
  out << "        ~NwiiInstCount() { flush(); }\n";
  out << "    } nwii_ic{ctx};\n";

  // Guest code can re-enter a function at any instruction: callbacks and
  // interrupts save the current PC, indirect branches (jump tables) and
  // restored thread contexts can all name an interior address.
  //
  // For a dense address range the resume dispatch is one table slot per
  // instruction. A slot holds the signed offset from loc_resume_body to its
  // label, which is a compile-time constant, so the table needs no relocations
  // and costs far less to compile than a switch with one case per instruction.
  // A sparse function (a handful of instructions across a large address span)
  // keeps the switch: its table would be huge and its case count tiny.
  if (valid_addrs.size() > 1) {
    const uint32_t start = func.start_address;
    const size_t span_slots = (last_addr - start) / 4 + 1;
    if (span_slots <= valid_addrs.size() * 2 + 8) {
      out << "    if (ctx.pc != 0x" << hex8(start) << "u) {\n";
      out << "        static const int32_t resume_offset[] = {\n";
      out << "            NWII_RESUME_DEFAULT,\n";
      for (uint32_t addr = start + 4; addr <= last_addr; addr += 4) {
        if (valid_addrs.count(addr))
          out << "            NWII_RESUME_LOC(" << hex8(addr) << "),\n";
        else
          out << "            NWII_RESUME_DEFAULT,\n";
      }
      out << "        };\n";
      out << "        const uint32_t delta = ctx.pc - 0x" << hex8(start)
          << "u;\n";
      out << "        if (delta < sizeof(resume_offset) && (delta & 3u) == 0)\n";
      out << "            goto *(void*)((const char*)&&loc_resume_body + "
             "resume_offset[delta >> 2]);\n";
      out << "        if (ctx.pc == 0) return; // idle: no runnable thread\n";
      out << "        nwii::runtime::interpret_step(ctx);\n";
      out << "        return;\n";
      out << "    }\n";
      out << "    goto loc_resume_body;\n";
      out << "loc_resume_default:\n";
      out << "    nwii::runtime::interpret_step(ctx);\n";
      out << "    return;\n";
      out << "loc_resume_body: ;\n";
    } else {
      out << "    if (ctx.pc != 0x" << std::hex << std::uppercase << start
          << std::dec << ") {\n";
      out << "        switch (ctx.pc) {\n";
      for (uint32_t addr : valid_addrs) {
        if (addr != start) {
          out << "            case 0x" << std::hex << std::uppercase << addr
              << std::dec << ": goto loc_" << hex8(addr) << ";\n";
        }
      }
      out << "            case 0x0: return; // idle: no runnable thread\n";
      out << "            default: nwii::runtime::interpret_step(ctx); return;\n";
      out << "        }\n";
      out << "    }\n";
    }
  }
  for (const auto &inst : func.instructions) {

    

    
    emit_instruction(out, inst, func, valid_addrs, cross_refs);
  }

  out << "}\n\n";
}

void Recompiler::emit_instruction(
    std::ostream &out, const analyzer::Instruction &inst,
    const analyzer::Function &func,
    const std::unordered_set<uint32_t> &instruction_addresses,
    std::set<std::string> *cross_refs) {
  ppc::Instruction ppc_inst(inst.opcode);

  out << "loc_" << hex8(inst.address) << ": ;\n";
  out << "    ++nwii_ic.n;\n";
  out << "    // 0x" << std::hex << std::setfill('0') << std::setw(8)
      << inst.address << ": ";
  out << std::setfill('0') << std::setw(8) << inst.opcode << std::dec << "\n";

  if (auto patch = config_.instruction_overrides.find(inst.address);
      patch != config_.instruction_overrides.end()) {
    out << "    " << patch->second << "\n";
    return;
  }

  if (ppc_inst.opcode() == 14) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();

    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = " << simm << "; // li r" << rD << ", "
          << simm << "\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] + " << simm
          << "; // addi r" << rD << ", r" << rA << ", " << simm << "\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 444) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    if (rS == rB) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "]; // mr r" << rA
          << ", r" << rS << "\n";
    } else {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]; // or r" << rA << ", r" << rS << ", r" << rB << "\n";
    }

    
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 339) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.gpr[" << rD << "] = ctx.lr; // mflr r" << rD << "\n";
    } else if (spr == 9) {
      out << "    ctx.gpr[" << rD << "] = ctx.ctr; // mfctr r" << rD << "\n";
    } else if (spr == 26) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr0; // mfsrr0 r" << rD << "\n";
    } else if (spr == 27) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr1; // mfsrr1 r" << rD << "\n";
    } else if (spr == 22) {
      out << "    ctx.gpr[" << rD << "] = ctx.read_dec(); // mfdec r" << rD << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gpr[" << rD << "] = ctx.gqr[" << (spr - 912)
          << "]; // mfgqr" << (spr - 912) << " r" << rD << "\n";
    } else if (spr == 1) {
      out << "    ctx.gpr[" << rD << "] = ctx.xer; // mfxer r" << rD << "\n";
    } else if (spr >= 920 && spr <= 924) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID " << spr << "\n";
    } else if (spr == 1008) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID0 (stub)\n";
    } else if (spr == 287) {
      out << "    ctx.gpr[" << rD
          << "] = (nwii::runtime::Config::get().platform == "
             "nwii::runtime::Platform::GameCube) ? 0x00083214 : 0x00087102; // "
             "mfspr PVR\n";
    } else if (spr == 1017) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr L2CR\n";
    } else {
      out << "    std::cerr << \"[WARN] mfspr r\" << " << rD
          << " << \" spr=\" << " << spr << " << \" (stub=0)\\n\";\n";
      out << "    ctx.gpr[" << rD << "] = 0; // unknown spr stub\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 467) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.lr = ctx.gpr[" << rS << "]; // mtlr r" << rS << "\n";
    } else if (spr == 9) {
      out << "    ctx.ctr = ctx.gpr[" << rS << "]; // mtctr r" << rS << "\n";
    } else if (spr == 26) {
      out << "    ctx.srr0 = ctx.gpr[" << rS << "]; // mtsrr0 r" << rS << "\n";
    } else if (spr == 27) {
      out << "    ctx.srr1 = ctx.gpr[" << rS << "]; // mtsrr1 r" << rS << "\n";
    } else if (spr == 22) {
      out << "    ctx.write_dec(ctx.gpr[" << rS << "]); // mtdec r" << rS << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gqr[" << (spr - 912) << "] = ctx.gpr[" << rS
          << "]; // mtgqr" << (spr - 912) << " r" << rS << "\n";
    } else if (spr == 1) {
      out << "    ctx.xer = ctx.gpr[" << rS << "]; // mtxer r" << rS << "\n";
    } else {
      out << "    // unhandled mtspr " << spr
          << "\n"; 
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 0) {
    
    uint32_t crD = ppc_inst.rd() >> 2; 
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();

    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << rA
        << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << rA
        << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << rA
        << "] == (int32_t)ctx.gpr[" << rB << "]);\n";
  } else if (ppc_inst.opcode() == 32) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(" << simm
          << "); // lwz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
          << "] + " << simm << "); // lwz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 33) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
        << "]); // lwzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 34) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(" << simm
          << "); // lbz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
          << "] + " << simm << "); // lbz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 35) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
        << "]); // lbzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 36) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write32(" << simm << ", ctx.gpr[" << rS
          << "]); // stw r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + " << simm
          << ", ctx.gpr[" << rS << "]); // stw r" << rS << ", " << simm << "(r"
          << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 37) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << simm << ";\n";
    out << "        ctx.mmu.write32(ea, ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stwu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 38) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write8(" << simm << ", (uint8_t)ctx.gpr[" << rS
          << "]); // stb r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << simm
          << ", (uint8_t)ctx.gpr[" << rS << "]); // stb r" << rS << ", " << simm
          << "(r" << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 39) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << simm << ";\n";
    out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stbu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 16) {
    
    uint32_t bo = ppc_inst.bo();
    uint32_t bi = ppc_inst.bi();
    uint32_t cr_idx = bi / 4;
    uint32_t cr_bit = bi % 4;

    std::string cond_field;
    if (cr_bit == 0)
      cond_field = "lt";
    else if (cr_bit == 1)
      cond_field = "gt";
    else if (cr_bit == 2)
      cond_field = "eq";
    else
      cond_field = "so";

    int16_t bd = ppc_inst.simm() & 0xFFFC; 
    int32_t target = bd;                   
    if ((inst.opcode & 2) == 2) {          
      target = bd;                         
      out << "    // Absolute branches not fully supported in simple string "
             "emit\n";
    } else {
      target = inst.address + bd;
    }

    std::string target_lbl = "loc_" + hex8(target);

    bool is_local = instruction_addresses.count(target) != 0;

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = 0x" << std::hex << std::uppercase << inst.address
          << std::dec << ";\n";
      out << "    nwii_ic.flush();\n";
      out << "    if (process_pending_callbacks(ctx)) return;\n";
      out << "    if (nwii::runtime::trace_spin() && (ctx.inst_count % 10000000) == 0) { std::cout << "
             "\"Spinning at PC: 0x\" << std::hex << 0x"
          << std::uppercase << std::hex << inst.address << std::dec
          << " << \" LR: 0x\" << std::hex << ctx.lr << std::dec << std::endl; "
             "}\n";
    }

    std::string ext_name;
    if (!is_local) {
      if (symbols_ && symbols_->has_symbol(target)) {
        ext_name = symbols_->get_symbol(target);
        if (ext_name.find("loc_") == 0)
          ext_name.replace(0, 4, "func_");
      } else {
        std::stringstream ss_ext;
        ss_ext << "func_" << std::hex << std::uppercase << std::setfill('0')
               << std::setw(8) << target;
        ext_name = ss_ext.str();
      }
    }

    bool bit0 = (bo & 0x10) != 0;
    bool bit1 = (bo & 0x08) != 0;
    bool bit2 = (bo & 0x04) != 0;
    bool bit3 = (bo & 0x02) != 0;

    bool dec_ctr = !bit2;
    bool test_ctr = !bit2;
    bool test_cr = !bit0;

    std::vector<std::string> conditions;
    if (test_ctr) {
      if (bit3)
        conditions.push_back("ctx.ctr == 0");
      else
        conditions.push_back("ctx.ctr != 0");
    }
    if (test_cr) {
      if (bit1)
        conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
      else
        conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
    }

    std::string cond_expr;
    if (conditions.empty())
      cond_expr = "true";
    else if (conditions.size() == 1)
      cond_expr = conditions[0];
    else
      cond_expr = conditions[0] + " && " + conditions[1];

    if (dec_ctr)
      out << "    ctx.ctr--;\n";

    if (ppc_inst.lk())
      out << "    ctx.lr = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << "; // save return address\n";

    if (is_local) {
      if (cond_expr == "true")
        out << "    goto " << target_lbl << ";\n";
      else
        out << "    if (" << cond_expr << ") goto " << target_lbl << ";\n";
    } else {
      bool is_mapped = true;
      if (!symbols_ || !symbols_->has_symbol(target)) {
        if (analyzer_.get_functions().find(target) ==
            analyzer_.get_functions().end()) {
          is_mapped = false;
        }
      }
      if (is_mapped) {
        if (cross_refs)
          cross_refs->insert(ext_name);
        if (cond_expr == "true") {
          out << "    nwii_ic.flush();\n";
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; " << ext_name << "(ctx); return;\n";
        } else {
          out << "    if (" << cond_expr << ") { nwii_ic.flush(); ctx.pc = 0x"
              << std::hex << std::uppercase << target << std::dec << "; "
              << ext_name << "(ctx); return; }\n";
        }
      } else {
        if (cond_expr == "true") {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return;\n";
        } else {
          out << "    if (" << cond_expr << ") { ctx.pc = 0x" << std::hex
              << target << std::dec << "; return; }\n";
        }
      }
    }
  } else if (ppc_inst.opcode() == 18) {
    
    uint32_t target = ppc_inst.branch_target(inst.address);
    std::string target_name;

    bool is_local = instruction_addresses.count(target) != 0;

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = 0x" << std::hex << std::uppercase << inst.address
          << std::dec << ";\n";
      out << "    nwii_ic.flush();\n";
      out << "    if (process_pending_callbacks(ctx)) return;\n";
      out << "    if (nwii::runtime::trace_spin() && (ctx.inst_count % 10000000) == 0) { std::cout << "
             "\"Spinning at PC: 0x\" << std::hex << 0x"
          << std::uppercase << std::hex << inst.address << std::dec
          << " << \" LR: 0x\" << std::hex << ctx.lr << std::dec << std::endl; "
             "}\n";
    }

    if (symbols_ && symbols_->has_symbol(target)) {
      target_name = symbols_->get_symbol(target);
      if (target_name.find("loc_") == 0)
        target_name.replace(0, 4, "func_");
    } else {
      std::stringstream ss_name;
      ss_name << "func_" << std::hex << std::uppercase << std::setfill('0')
              << std::setw(8) << target;
      target_name = ss_name.str();
    }

    if (ppc_inst.is_branch_link()) {
      out << "    ctx.lr = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << "; // save return address\n";
      if (is_local) {
        out << "    goto loc_" << hex8(target) << ";\n";
      } else {
        bool is_mapped = true;
        if (!symbols_ || !symbols_->has_symbol(target)) {
          if (analyzer_.get_functions().find(target) ==
              analyzer_.get_functions().end()) {
            is_mapped = false;
          }
        }
        if (is_mapped) {
          if (cross_refs)
            cross_refs->insert(target_name);
          out << "    nwii_ic.flush();\n";
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; ";
          out << target_name << "(ctx);\n";
          // Some HLE handlers (e.g. OSReport) are addressed by their handler
          // name directly and do not set ctx.pc. Treat an unchanged pc as a
          // normal return so the dispatcher does not invoke the handler again.
          out << "    if (ctx.pc == 0x" << std::hex << std::uppercase << target
              << std::dec << ") ctx.pc = 0x" << std::hex << std::uppercase
              << (inst.address + 4) << std::dec << ";\n";
          out << "    if (ctx.pc != 0x" << std::hex << std::uppercase
              << (inst.address + 4) << std::dec << ") return;\n";
        } else {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return; // bl to unmapped target\n";
        }
      }
    } else {
      if (is_local) {
        out << "    goto loc_" << hex8(target) << ";\n";
      } else {
        bool is_mapped = true;
        if (!symbols_ || !symbols_->has_symbol(target)) {
          if (analyzer_.get_functions().find(target) ==
              analyzer_.get_functions().end()) {
            is_mapped = false;
          }
        }
        if (is_mapped) {
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; return;\n";
        } else {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return;\n";
        }
      }
    }
  } else if (ppc_inst.opcode() == 19) {
    uint32_t xo = ppc_inst.extended_opcode();
    if (xo == 150) { 
      out << "    // isync\n";
    } else if (xo == 50) { 
        out << "    {\n";
        out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
        out << "        ctx.msr = ctx.srr1; // rfi: restore MSR from SRR1\n";
        out << "        ctx.pc = ctx.srr0;  // rfi: jump to SRR0\n";
        out << "        ctx.dispatch_saved_ctx = 0; // exit ISR\n";
        out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
        out << "            nwii_ic.flush();\n";
        out << "            if (process_pending_callbacks(ctx)) return;\n";
        out << "        }\n";
        out << "    }\n";
        out << "    return;\n";
    } else if (xo == 16 || xo == 528) { 
      uint32_t bo = ppc_inst.bo();
      uint32_t bi = ppc_inst.bi();
      uint32_t cr_idx = bi / 4;
      uint32_t cr_bit = bi % 4;

      std::string cond_field;
      if (cr_bit == 0)
        cond_field = "lt";
      else if (cr_bit == 1)
        cond_field = "gt";
      else if (cr_bit == 2)
        cond_field = "eq";
      else
        cond_field = "so";

      std::string target_reg = (xo == 16) ? "ctx.lr" : "ctx.ctr";

      bool bit0 = (bo & 0x10) != 0;
      bool bit1 = (bo & 0x08) != 0;
      bool bit2 = (bo & 0x04) != 0;
      bool bit3 = (bo & 0x02) != 0;

      bool dec_ctr = !bit2;
      bool test_ctr = !bit2;
      bool test_cr = !bit0;

      std::vector<std::string> conditions;
      if (test_ctr) {
        if (bit3)
          conditions.push_back("ctx.ctr == 0");
        else
          conditions.push_back("ctx.ctr != 0");
      }
      if (test_cr) {
        if (bit1)
          conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
        else
          conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
      }

      std::string cond_expr;
      if (conditions.empty())
        cond_expr = "true";
      else if (conditions.size() == 1)
        cond_expr = conditions[0];
      else
        cond_expr = conditions[0] + " && " + conditions[1];

      if (dec_ctr)
        out << "    ctx.ctr--;\n";

      // PowerPC clears the low two bits of an indirect branch target, and the
      // link register must be captured before bclrl overwrites it.
      const bool lr_target = target_reg == "ctx.lr";
      const std::string target_expr =
          std::string("(") + target_reg + " & ~3u)";
      if (ppc_inst.lk() && lr_target) {
        out << "    {\n";
        out << "        const uint32_t target = " << target_expr << ";\n";
        out << "        ctx.lr = 0x" << std::hex << std::uppercase
            << (inst.address + 4) << std::dec << "; // save return address\n";
        if (cond_expr == "true") {
          out << "        ctx.pc = target; return;\n";
        } else {
          out << "        if (" << cond_expr
              << ") { ctx.pc = target; return; }\n";
        }
        out << "    }\n";
      } else {
        if (ppc_inst.lk()) {
          out << "    ctx.lr = 0x" << std::hex << std::uppercase
              << (inst.address + 4) << std::dec << "; // save return address\n";
        }

        bool null_guard = ppc_inst.lk() && !lr_target;
        if (cond_expr == "true") {
          if (null_guard)
            out << "    if (" << target_expr << ") { ctx.pc = " << target_expr
                << "; return; } else nwii::runtime::note_null_call(0x"
                << std::hex << std::uppercase << inst.address << std::dec
                << ", ctx.lr);\n";
          else
            out << "    ctx.pc = " << target_expr << "; return;\n";
        } else {
          if (null_guard)
            out << "    if ((" << cond_expr << ") && " << target_expr
                << ") { ctx.pc = " << target_expr << "; return; }\n";
          else
            out << "    if (" << cond_expr << ") { ctx.pc = " << target_expr
                << "; return; }\n";
        }
      }
    } else if (xo == 257 || xo == 129 || xo == 289 || xo == 225 || xo == 33 ||
               xo == 449 || xo == 417 || xo == 193) {
      uint32_t crbD = ppc_inst.rd();
      uint32_t crbA = ppc_inst.ra();
      uint32_t crbB = ppc_inst.rb();

      auto get_cr_bit_str = [](uint32_t bit_idx) {
        std::string field = "ctx.cr[" + std::to_string(bit_idx / 4) + "]";
        uint32_t bit = bit_idx % 4;
        if (bit == 0)
          return field + ".lt";
        if (bit == 1)
          return field + ".gt";
        if (bit == 2)
          return field + ".eq";
        return field + ".so";
      };

      std::string op;
      if (xo == 257)
        op = get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB); 
      else if (xo == 129)
        op = get_cr_bit_str(crbA) + " & !" + get_cr_bit_str(crbB); 
      else if (xo == 289)
        op = get_cr_bit_str(crbA) + " == " + get_cr_bit_str(crbB); 
      else if (xo == 225)
        op = "!(" + get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 33)
        op = "!(" + get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 449)
        op = get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB); 
      else if (xo == 417)
        op = get_cr_bit_str(crbA) + " | !" + get_cr_bit_str(crbB); 
      else if (xo == 193)
        op = get_cr_bit_str(crbA) + " ^ " + get_cr_bit_str(crbB); 

      out << "    " << get_cr_bit_str(crbD) << " = " << op << ";\n";
    } else if (xo == 0) {
      uint32_t crfD = ppc_inst.rd() >> 2;
      uint32_t crfS = ppc_inst.ra() >> 2;
      out << "    ctx.cr[" << crfD << "] = ctx.cr[" << crfS << "]; // mcrf\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED OPCODE 19 XO \" << " << xo
          << " << \" at 0x" << std::hex << inst.address << std::dec
          << "\\n\"; std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 48) {
    
    uint32_t fD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();

    
    
    if (rA == 0) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD
          << "] = ctx.mmu.read_f32(" << simm << "); // lfs\n";
    } else {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD
          << "] = ctx.mmu.read_f32(ctx.gpr[" << rA << "] + " << simm
          << "); // lfs\n";
    }
  } else if (ppc_inst.opcode() == 52) {
    
    uint32_t fS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write_f32(" << simm << ", (float)ctx.fpr[" << fS
          << "]); // stfs f" << fS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write_f32(ctx.gpr[" << rA << "] + " << simm
          << ", (float)ctx.fpr[" << fS << "]); // stfs f" << fS << ", " << simm
          << "(r" << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 59) {
    uint32_t xo = (ppc_inst.value() >> 1) & 0x1F;
    uint32_t fD = ppc_inst.rd();
    uint32_t fA = ppc_inst.ra();
    uint32_t fB = ppc_inst.rb();
    uint32_t fC = ppc_inst.rc();

    
    if (xo == 21) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] + ctx.fpr[" << fB << "]); // fadds\n";
    } else if (xo == 20) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] - ctx.fpr[" << fB << "]); // fsubs\n";
    } else if (xo == 25) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] * ctx.fpr[" << fC << "]); // fmuls\n";
    } else if (xo == 18) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] / ctx.fpr[" << fB << "]); // fdivs\n";
    } else if (xo == 22) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)std::sqrt(ctx.fpr[" << fB
          << "]); // fsqrts\n";
    } else if (xo == 24) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)(1.0f / ctx.fpr[" << fB
          << "]); // fres\n";
    } else if (xo == 29) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], ctx.fpr[" << fB << "]); // fmadds\n";
    } else if (xo == 28) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], -ctx.fpr[" << fB << "]); // fmsubs\n";
    } else if (xo == 31) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)(-std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], ctx.fpr[" << fB << "])); // fnmadds\n";
    } else if (xo == 30) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (float)(-std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], -ctx.fpr[" << fB << "])); // fnmsubs\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 59 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
    if (ppc_inst.value() & 1)
      out << "    ctx.update_cr1(); // record form: CR1 from FPSCR\n";
  } else if (ppc_inst.opcode() == 63) {
    uint32_t xo = (ppc_inst.value() >> 1) & 0x3FF;
    uint32_t fD = ppc_inst.rd();
    uint32_t fB = ppc_inst.rb();
    uint32_t fA = ppc_inst.ra();
    if (xo == 72) {
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fB << "]; // fmr\n";
    } else if (xo == 40) { 
      out << "    ctx.fpr[" << fD << "] = -ctx.fpr[" << fB << "]; // fneg\n";
    } else if (xo == 264) { 
      out << "    ctx.fpr[" << fD << "] = std::fabs(ctx.fpr[" << fB
          << "]); // fabs\n";
    } else if (xo == 136) { 
      out << "    ctx.fpr[" << fD << "] = -std::fabs(ctx.fpr[" << fB
          << "]); // fnabs\n";
    } else if (xo == 12) { 
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD << "] = (double)(float)ctx.fpr[" << fB
          << "]; // frsp\n";
    } else if (xo == 15) { 
      out << "    {\n";
      out << "        uint32_t i_val = (uint32_t)nwii::runtime::guest_fctiw("
          << "ctx.fpr[" << fB << "], ctx.fpscr, true);\n";
      out << "        uint64_t val = i_val;\n";
      out << "        std::memcpy(&ctx.fpr[" << fD << "], &val, 8);\n";
      out << "    } // fctiwz\n";
    } else if (xo == 14) { 
      out << "    {\n";
      out << "        uint32_t i_val = (uint32_t)nwii::runtime::guest_fctiw("
          << "ctx.fpr[" << fB << "], ctx.fpscr, false);\n";
      out << "        uint64_t val = i_val;\n";
      out << "        std::memcpy(&ctx.fpr[" << fD << "], &val, 8);\n";
      out << "    } // fctiw\n";
    } else if (xo == 583) { 
      out << "    { uint64_t v = ctx.mffs_bits(); std::memcpy(&ctx.fpr["
          << fD << "], &v, 8); } // mffs\n";
    } else if (xo == 711) { 
      uint32_t fm = (ppc_inst.value() >> 17) & 0xFF;
      out << "    { uint64_t v; std::memcpy(&v, &ctx.fpr[" << fB
          << "], 8); ctx.mtfsf(" << fm << ", (uint32_t)v); } // mtfsf\n";
    } else if ((xo & 0x1F) == 26) { 
      out << "    ctx.fpr[" << fD << "] = 1.0 / std::sqrt(ctx.fpr[" << fB
          << "]); // frsqrte\n";
    } else if ((xo & 0x1F) == 24) { 
      out << "    ctx.fpr[" << fD << "] = 1.0 / ctx.fpr[" << fB
          << "]; // fre\n";
    } else if ((xo & 0x1F) == 18) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] / ctx.fpr["
          << fB << "]; // fdiv\n";
    } else if ((xo & 0x1F) == 20) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] - ctx.fpr["
          << fB << "]; // fsub\n";
    } else if ((xo & 0x1F) == 21) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] + ctx.fpr["
          << fB << "]; // fadd\n";
    } else if ((xo & 0x1F) == 25) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] * ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "]; // fmul\n";
    } else if ((xo & 0x1F) == 29) { 
      out << "    ctx.fpr[" << fD << "] = std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F) << "], ctx.fpr["
          << fB << "]); // fmadd\n";
    } else if ((xo & 0x1F) == 28) { 
      out << "    ctx.fpr[" << fD << "] = std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F) << "], -ctx.fpr["
          << fB << "]); // fmsub\n";
    } else if ((xo & 0x1F) == 31) { 
      out << "    ctx.fpr[" << fD << "] = -std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F) << "], ctx.fpr["
          << fB << "]); // fnmadd\n";
    } else if ((xo & 0x1F) == 30) { 
      out << "    ctx.fpr[" << fD << "] = -std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F) << "], -ctx.fpr["
          << fB << "]); // fnmsub\n";
    } else if ((xo & 0x1F) == 23) { 
      out << "    ctx.fpr[" << fD << "] = (ctx.fpr[" << fA
          << "] >= 0.0) ? ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F)
          << "] : ctx.fpr[" << fB << "]; // fsel\n";
    } else if (xo == 38 ||
               xo == 70) { 
      uint32_t bit = (ppc_inst.value() >> 21) & 0x1F;
      out << "    ctx.set_fpscr_bit(" << bit << ", "
          << (xo == 38 ? "true" : "false") << "); // mtfsb"
          << (xo == 38 ? "1" : "0") << "\n";
    } else if (xo == 64) { 
      uint32_t crfd = (ppc_inst.value() >> 23) & 0x7;
      uint32_t crfs = (ppc_inst.value() >> 18) & 0x7;
      out << "    ctx.mcrfs(" << crfd << ", " << crfs << "); // mcrfs\n";
    } else if (xo == 134) { 
      uint32_t field = (ppc_inst.value() >> 23) & 0x7;
      uint32_t imm = (ppc_inst.value() >> 12) & 0xF;
      out << "    ctx.mtfsfi(" << field << ", " << imm << "); // mtfsfi\n";
    } else if (xo == 0 || xo == 32) { 
      uint32_t crfD = fD >> 2;
      out << "    {\n";
      out << "        bool un = std::isnan(ctx.fpr[" << fA
          << "]) || std::isnan(ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].lt = !un && (ctx.fpr[" << fA
          << "] < ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].gt = !un && (ctx.fpr[" << fA
          << "] > ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].eq = !un && (ctx.fpr[" << fA
          << "] == ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].so = un;\n";
      out << "    }\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 63 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
    if (ppc_inst.value() & 1)
      out << "    ctx.update_cr1(); // record form: CR1 from FPSCR\n";
  } else if (ppc_inst.opcode() == 20) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rA << "] & ~0x"
        << std::hex << std::uppercase << mask << std::dec
        << ") | (std::rotl(ctx.gpr[" << rS << "], " << sh << ") & 0x"
        << std::hex << std::uppercase << mask << std::dec << "); // rlwimi r"
        << rA << ", r" << rS << ", " << sh << ", " << mb << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    }
  } else if (ppc_inst.opcode() == 21) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = std::rotl(ctx.gpr[" << rS << "], " << sh
        << ") & 0x" << std::hex << std::uppercase << mask << std::dec
        << "; // rlwinm r" << rA << ", r" << rS << ", " << sh << ", " << mb
        << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    }
  } else if (ppc_inst.opcode() == 31) {
    uint32_t xo = ppc_inst.extended_opcode();
    // XO-form arithmetic encodes OE as bit 9 of the extended opcode. Fold
    // each OE variant onto its base operation and remember the flag so a
    // single XER.OV/SO update can be emitted after the result.
    bool oe = false;
    switch (xo) {
    case 522: xo = 10; oe = true; break;   // addco
    case 520: xo = 8; oe = true; break;    // subfco
    case 650: xo = 138; oe = true; break;  // addeo
    case 648: xo = 136; oe = true; break;  // subfeo
    case 714: xo = 202; oe = true; break;  // addzeo
    case 712: xo = 200; oe = true; break;  // subfzeo
    case 746: xo = 234; oe = true; break;  // addmeo
    case 744: xo = 232; oe = true; break;  // subfmeo
    case 552: xo = 40; oe = true; break;   // subfo
    case 747: xo = 235; oe = true; break;  // mullwo
    case 778: xo = 266; oe = true; break;  // addo
    case 616: xo = 104; oe = true; break;  // nego
    case 1019: xo = 491; oe = true; break; // divwo
    default: break;
    }
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    // In non-update indexed memory instructions RA=0 means a zero base,
    // regardless of the value stored in GPR0 (which may also be the destination).
    const std::string indexed_ea =
        (rA ? "ctx.gpr[" + std::to_string(rA) + "] + " : "") +
        "ctx.gpr[" + std::to_string(rB) + "]";

    uint32_t rS = ppc_inst.rs();

    if (xo == 598 || xo == 854 || xo == 982 || xo == 54 || xo == 86 ||
        xo == 278 || xo == 246 || xo == 470) {
      
      out << "    // sync/cache instruction\n";
    } else if (xo == 0) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = ((int32_t)ctx.gpr[" << rA
          << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = ((int32_t)ctx.gpr[" << rA
          << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 32) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = (ctx.gpr[" << rA << "] < ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = (ctx.gpr[" << rA << "] > ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 11) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(((uint64_t)ctx.gpr[" << rA
          << "] * (uint64_t)ctx.gpr[" << rB << "]) >> 32); // mulhwu\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 75) { 
      out << "    ctx.gpr[" << rD
          << "] = (uint32_t)(((int64_t)(int32_t)ctx.gpr[" << rA
          << "] * (int64_t)(int32_t)ctx.gpr[" << rB << "]) >> 32); // mulhw\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 87) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rB
            << "]); // lbzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lbzx\n";
    } else if (xo == 215) {
      if (rA == 0)
        out << "    ctx.mmu.write8(ctx.gpr[" << rB << "], (uint8_t)ctx.gpr["
            << rS << "]); // stbx\n";
      else
        out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint8_t)ctx.gpr[" << rS << "]); // stbx\n";
    } else if (xo == 279) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rB
            << "]); // lhzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lhzx\n";
    } else if (xo == 371) { 
      uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
      uint32_t spr_top    = (ppc_inst.value() >> 11) & 0x1F;
      uint32_t spr = (spr_top << 5) | spr_bottom;
      if (spr == 269) { 
        out << "    nwii_ic.flush();\n";
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() >> 32); // mftbu\n";
      } else { 
        out << "    nwii_ic.flush();\n";
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() & 0xFFFFFFFF); // mftb\n";
      }

      
      
    } else if (xo == 407) {
      if (rA == 0)
        out << "    ctx.mmu.write16(ctx.gpr[" << rB << "], (uint16_t)ctx.gpr["
            << rS << "]); // sthx\n";
      else
        out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint16_t)ctx.gpr[" << rS << "]); // sthx\n";
    } else if (xo == 23) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rB
            << "]); // lwzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lwzx\n";
    } else if (xo == 151) {
      if (rA == 0)
        out << "    ctx.mmu.write32(ctx.gpr[" << rB << "], ctx.gpr[" << rS
            << "]); // stwx\n";
      else
        out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], ctx.gpr[" << rS << "]); // stwx\n";
    } else if (xo == 40) {
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr["
          << rB << "];\n";
      out << "        ctx.gpr[" << rD << "] = b - a; // subf\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)b - "
               "(int64_t)(int32_t)a;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // subfo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 235) {
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr["
          << rB << "];\n";
      out << "        ctx.gpr[" << rD << "] = a * b; // mullw\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)a * "
               "(int64_t)(int32_t)b;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // mullwo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 491) {
      out << "    if (ctx.gpr[" << rB << "] != 0) {\n";
      out << "      const int32_t a = (int32_t)ctx.gpr[" << rA
          << "], b = (int32_t)ctx.gpr[" << rB << "];\n";
      out << "      const bool ov = (b != 0 && a == INT32_MIN && b == -1);\n";
      out << "      ctx.gpr[" << rD
          << "] = (uint32_t)((a == INT32_MIN && b == -1) ? INT32_MIN : a / b); "
             "// divw (PPC wraps the overflow case)\n";
      if (oe) {
        out << "      ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "      if (ov) ctx.xer |= 0x80000000u; // divwo\n";
      }
      out << "    }\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 459) {
      out << "    if (ctx.gpr[" << rB << "] != 0) ctx.gpr[" << rD
          << "] = ctx.gpr[" << rA << "] / ctx.gpr[" << rB << "]; // divwu\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 954) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int8_t)ctx.gpr["
          << rS << "]; // extsb\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 922) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int16_t)ctx.gpr["
          << rS << "]; // extsh\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 28) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]; // and\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 60) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ~ctx.gpr["
          << rB << "]; // andc\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 316) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]; // xor\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 24) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] << (ctx.gpr[" << rB
          << "] & 0x1F)); // slw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 536) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] >> (ctx.gpr[" << rB
          << "] & 0x1F)); // srw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 792) {
      out << "    { int32_t src = (int32_t)ctx.gpr[" << rS
          << "]; uint32_t sh = ctx.gpr[" << rB << "] & 63;\n";
      out << "      ctx.gpr[" << rA << "] = (uint32_t)(src >> (sh >= 32 ? 31 : sh));\n";
      out << "      bool ca = src < 0 && (sh >= 32 || "
             "(sh != 0 && ((uint32_t)src & ((1u << sh) - 1)) != 0));\n";
      out << "      ctx.xer = (ctx.xer & ~0x20000000u) | ((uint32_t)ca << 29); } // sraw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 26) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS
          << "] == 0 ? 32 : std::countl_zero(ctx.gpr[" << rS
          << "]); // cntlzw\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
        out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 10) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr["
          << rB << "];\n";
      out << "        const uint64_t sum = (uint64_t)a + (uint64_t)b;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)a + "
               "(int64_t)(int32_t)b;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // addco\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 8) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr["
          << rB << "];\n";
      out << "        const uint64_t sum = (uint64_t)(~a) + (uint64_t)b + 1;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)b - "
               "(int64_t)(int32_t)a;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // subfco\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 104) { 
      // Unsigned negation gives the wrapped 32-bit result without host UB for
      // INT32_MIN. nego additionally reports the overflow in XER.
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "];\n";
      out << "        ctx.gpr[" << rD << "] = 0u - a; // neg\n";
      if (oe) {
        out << "        const bool ov = (a == 0x80000000u);\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // nego\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 266) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr["
          << rB << "];\n";
      out << "        const uint32_t r = a + b;\n";
      out << "        ctx.gpr[" << rD << "] = r; // add\n";
      if (oe) {
        out << "        const bool ov = ((~(a ^ b) & (a ^ r)) & 0x80000000u) != 0;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // addo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 138) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr["
          << rB << "];\n";
      out << "        const uint32_t ca = (ctx.xer >> 29) & 1;\n";
      out << "        const uint64_t sum = (uint64_t)a + (uint64_t)b + "
             "(uint64_t)ca;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)a + "
               "(int64_t)(int32_t)b + (int64_t)ca;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // addeo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 136) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr["
          << rB << "];\n";
      out << "        const uint32_t ca = (ctx.xer >> 29) & 1;\n";
      out << "        const uint64_t sum = (uint64_t)(~a) + (uint64_t)b + "
             "(uint64_t)ca;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)b - "
               "(int64_t)(int32_t)a - 1 + (int64_t)ca;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // subfeo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 202) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "];\n";
      out << "        const uint32_t ca = (ctx.xer >> 29) & 1;\n";
      out << "        const uint64_t sum = (uint64_t)a + (uint64_t)ca;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)a + "
               "(int64_t)ca;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // addzeo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 200) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "];\n";
      out << "        const uint32_t ca = (ctx.xer >> 29) & 1;\n";
      out << "        const uint64_t sum = (uint64_t)(~a) + (uint64_t)ca;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = -(int64_t)(int32_t)a - 1 + "
               "(int64_t)ca;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // subfzeo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 234) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "];\n";
      out << "        const uint32_t ca = (ctx.xer >> 29) & 1;\n";
      out << "        const uint64_t sum = (uint64_t)a + 0xFFFFFFFFULL + "
             "(uint64_t)ca;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = (int64_t)(int32_t)a - 1 + "
               "(int64_t)ca;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // addmeo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 232) { 
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "];\n";
      out << "        const uint32_t ca = (ctx.xer >> 29) & 1;\n";
      out << "        const uint64_t sum = (uint64_t)(~a) + 0xFFFFFFFFULL + "
             "(uint64_t)ca;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)sum;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | "
             "((uint32_t)((sum >> 32) & 1) << 29); // Set CA bit\n";
      if (oe) {
        out << "        const int64_t exact = -(int64_t)(int32_t)a - 2 + "
               "(int64_t)ca;\n";
        out << "        const bool ov = exact > INT32_MAX || exact < INT32_MIN;\n";
        out << "        ctx.xer = (ctx.xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);\n";
        out << "        if (ov) ctx.xer |= 0x80000000u; // subfmeo\n";
      }
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
        out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
      out << "    }\n";
    } else if (xo == 83) { 
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.msr;\n";
    } else if (xo == 146) { 

      

      
      out << "    {\n";
      out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
      out << "        ctx.msr = ctx.gpr[" << ppc_inst.rs() << "];\n";
      out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
      out << "            ctx.pc = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << ";\n";
      out << "            nwii_ic.flush();\n";
      out << "            if (process_pending_callbacks(ctx)) return;\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 124) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]); // nor\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 476) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]); // nand\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 284) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]); // eqv\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 412) { 
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ~ctx.gpr["
          << rB << "]; // orc\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 20) { 
      // RA=0 means a zero base; GPR0 is not an operand.
      out << "    { uint32_t ea = " << indexed_ea
          << "; ctx.gpr[" << rD << "] = ctx.mmu.read32(ea); "
          << "ctx.mmu.set_reservation(ea); } // lwarx\n";
    } else if (xo == 150) { 
      out << "    {\n";
      out << "      const uint32_t ea = " << indexed_ea << ";\n";
      out << "      const bool ok = ctx.mmu.reservation_matches(ea);\n";
      out << "      if (ok) ctx.mmu.write32(ea, ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "      ctx.mmu.clear_reservation();\n";
      out << "      ctx.cr[0].lt = false; ctx.cr[0].gt = false; "
             "ctx.cr[0].eq = ok; ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      out << "    } // stwcx.\n";
    } else if (xo == 119) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read8(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lbzux\n";
    } else if (xo == 311) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhzux\n";
    } else if (xo == 343) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(int32_t)(int16_t)"
          << "ctx.mmu.read16(" << indexed_ea << "); // lhax\n";
    } else if (xo == 375) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhaux\n";
    } else if (xo == 247) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stbux\n";
    } else if (xo == 439) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write16(ea, (uint16_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // sthux\n";
    } else if (xo == 792) { 

      out << "    {\n";
      out << "        int32_t sv = (int32_t)ctx.gpr[" << rA << "];\n";
      out << "        uint32_t sh = ctx.gpr[" << rB << "] & 0x3F;\n";
      out << "        uint32_t ca;\n";
      out << "        if (sh >= 32) { ctx.gpr[" << rD
          << "] = (sv < 0) ? 0xFFFFFFFF : 0; ca = (sv < 0) ? 1 : 0; }\n";
      out << "        else { ctx.gpr[" << rD << "] = (uint32_t)(sv >> sh);\n";
      out << "            ca = (sv < 0 && (uint32_t)(sv & ((sh==0)?0:((1u<<sh)-1))) != 0) ? 1 : 0; }\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);\n";
      if (ppc_inst.value() & 1)
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      out << "    } // sraw\n";
    } else if (xo == 824) { 

      

      uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
      out << "    {\n";
      out << "        int32_t sv = (int32_t)ctx.gpr[" << rD << "];\n";
      out << "        ctx.gpr[" << rA << "] = (uint32_t)(sv >> " << sh << ");\n";
      out << "        uint32_t ca = (sv < 0 && (uint32_t)(sv & "
          << (sh == 0 ? 0u : ((1u << sh) - 1)) << "u) != 0) ? 1 : 0;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);\n";
      if (ppc_inst.value() & 1)
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      out << "    } // srawi\n";
    } else if (xo == 922) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(int32_t)(int16_t)ctx.gpr["
          << rA << "]; // extsh\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 954) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(int32_t)(int8_t)ctx.gpr["
          << rA << "]; // extsb\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
            out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
      }
    } else if (xo == 535) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    ctx.fpr[" << frD2 << "] = ctx.ps1[" << frD2
          << "] = (double)ctx.mmu.read_f32(" << indexed_ea << "); // lfsx (both PS lanes)\n";
    } else if (xo == 567) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.fpr[" << frD2 << "] = ctx.ps1[" << frD2
          << "] = (double)ctx.mmu.read_f32(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lfsux (both PS lanes)\n";
    } else if (xo == 599) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(" << indexed_ea << "); // lfdx\n";
    } else if (xo == 631) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lfdux\n";
    } else if (xo == 663) { 
      out << "    ctx.mmu.write_f32(" << indexed_ea
          << ", (float)ctx.fpr[" << ppc_inst.rs() << "]); // stfsx\n";
    } else if (xo == 695) { 
      out << "    ctx.mmu.write_f32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], (float)ctx.fpr[" << ppc_inst.rs() << "]); // stfsux\n";
      out << "    ctx.gpr[" << rA << "] += ctx.gpr[" << rB << "];\n";
    } else if (xo == 727) { 
      out << "    ctx.mmu.write_f64(" << indexed_ea
          << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfdx\n";
    } else if (xo == 759) { 
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], ctx.fpr[" << ppc_inst.rs() << "]); // stfdux\n";
      out << "    ctx.gpr[" << rA << "] += ctx.gpr[" << rB << "];\n";
    } else if (xo == 983) { 
      out << "    ctx.mmu.write32(" << indexed_ea
          << ", (uint32_t)std::bit_cast<uint64_t>(ctx.fpr[" << ppc_inst.rs()
          << "])); // stfiwx: store the low word of the FPR bits\n";
    } else if (xo == 790) { 
      out << "    { uint16_t v = ctx.mmu.read16(" << indexed_ea << "); ctx.gpr[" << rD
          << "] = ((v&0xFF)<<8)|((v>>8)&0xFF); } // lhbrx\n";
    } else if (xo == 534) { 
      out << "    { uint32_t v = ctx.mmu.read32(" << indexed_ea << "); ctx.gpr[" << rD
          << "] = __builtin_bswap32(v); } // lwbrx\n";
    } else if (xo == 662) { 
      out << "    ctx.mmu.write32(" << indexed_ea
          << ", __builtin_bswap32(ctx.gpr[" << ppc_inst.rs()
          << "])); // stwbrx\n";
    } else if (xo == 1014) { 
      out << "    ctx.clear_cache_line(" << indexed_ea << "); // dcbz\n";
    } else if (xo == 55) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read32(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lwzux\n";
    } else if (xo == 183) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write32(ea, ctx.gpr[" << ppc_inst.rs() << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stwux\n";
    } else if (xo == 918) { 
      out << "    ctx.mmu.write16(" << indexed_ea
          << ", ((ctx.gpr[" << ppc_inst.rs() << "] & 0xFF) << 8) | ((ctx.gpr["
          << ppc_inst.rs() << "] >> 8) & 0xFF)); // sthbrx\n";
    } else if (xo == 595 || xo == 210) { 
      out << "    // mfsr/mtsr stub\n";
    } else if (xo == 19) { 
      out << "    {\n";
      out << "        uint32_t cr_val = 0;\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            cr_val |= (ctx.cr[i].lt ? 8 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].gt ? 4 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].eq ? 2 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].so ? 1 : 0) << (28 - i * 4);\n";
      out << "        }\n";
      out << "        ctx.gpr[" << rD << "] = cr_val;\n";
      out << "    }\n";
    } else if (xo == 144) { 
      uint32_t crm = (ppc_inst.value() >> 12) & 0xFF;
      out << "    {\n";
      out << "        uint32_t cr_val = ctx.gpr[" << ppc_inst.rs() << "];\n";
      out << "        uint32_t mask = 0x" << std::hex << crm << std::dec
          << ";\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            if (mask & (1 << (7 - i))) {\n";
      out << "                uint32_t field = (cr_val >> (28 - i * 4)) & "
             "0xF;\n";
      out << "                ctx.cr[i].lt = (field & 8) != 0;\n";
      out << "                ctx.cr[i].gt = (field & 4) != 0;\n";
      out << "                ctx.cr[i].eq = (field & 2) != 0;\n";
      out << "                ctx.cr[i].so = (field & 1) != 0;\n";
      out << "            }\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 566) { 
      out << "    // tlbsync (no-op)\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 31 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 24) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << ppc_inst.uimm() << "; // ori\n";
  } else if (ppc_inst.opcode() == 25) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << (ppc_inst.uimm() << 16) << "; // oris\n";
  } else if (ppc_inst.opcode() == 26) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << ppc_inst.uimm() << "; // xori\n";
  } else if (ppc_inst.opcode() == 27) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << (ppc_inst.uimm() << 16) << "; // xoris\n";
  } else if (ppc_inst.opcode() == 28) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << ppc_inst.rs() << "] & "
        << ppc_inst.uimm() << "; // andi.\n";
    out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
    out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
    out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
    out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 29) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] & " << (ppc_inst.uimm() << 16) << "; // andis.\n";
    out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << ppc_inst.ra() << "] < 0);\n";
    out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << ppc_inst.ra() << "] > 0);\n";
    out << "    ctx.cr[0].eq = (ctx.gpr[" << ppc_inst.ra() << "] == 0);\n";
    out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 7) { 
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = (uint32_t)((int32_t)ctx.gpr["
        << ppc_inst.ra() << "] * (uint32_t)(int32_t)" << ppc_inst.simm()
        << "); // mulli (modulo 32, no signed overflow)\n";
  } else if (ppc_inst.opcode() == 8) {
    const uint32_t rd = ppc_inst.rd(), ra = ppc_inst.ra();
    out << "    { uint64_t sum = (uint64_t)(~ctx.gpr[" << ra
        << "]) + " << (uint32_t)(int32_t)ppc_inst.simm() << "ull + 1;\n";
    out << "      ctx.gpr[" << rd << "] = (uint32_t)sum;\n";
    out << "      ctx.xer = (ctx.xer & ~0x20000000u) | "
           "((uint32_t)(sum >> 32) << 29); } // subfic\n";
  } else if (ppc_inst.opcode() == 12) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int32_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA << "] + " << (uint32_t)simm
        << "ull;\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 13) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int32_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA << "] + " << (uint32_t)simm
        << "ull;\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
    out << "        ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
    out << "        ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
    out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 15) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = " << (ppc_inst.simm() << 16) << "; // lis\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.gpr[" << rA << "] + "
          << (ppc_inst.simm() << 16) << "; // addis\n";
  } else if (ppc_inst.opcode() == 17) { 
    out << "    // sc (System Call)\n";
    out << "    nwii::runtime::handle_syscall(ctx);\n";
  } else if (ppc_inst.opcode() == 4) { 
    uint32_t xo = (inst.opcode >> 1) & 0x1F;
    uint32_t xo_10 = (inst.opcode >> 1) & 0x3FF;
    uint32_t frD = ppc_inst.rd();
    uint32_t frA = ppc_inst.ra();
    uint32_t frB = ppc_inst.rb();
    uint32_t frC = ppc_inst.rc();

    // Indexed quantized loads/stores use a six-bit XO; W and I are
    // separate fields, not part of the arithmetic paired-single opcode.
    const uint32_t xo_6 = (inst.opcode >> 1) & 0x3F;
    if (xo_6 == 6 || xo_6 == 7 || xo_6 == 38 || xo_6 == 39) {
      const uint32_t ra = ppc_inst.ra(), rb = ppc_inst.rb();
      const uint32_t w = (inst.opcode >> 10) & 1;
      const uint32_t i = (inst.opcode >> 7) & 7;
      const bool update = (xo_6 & 32) != 0;
      const bool store = (xo_6 & 1) != 0;
      out << "    { uint32_t ea = ";
      if (ra != 0 || update) out << "ctx.gpr[" << ra << "] + ";
      out << "ctx.gpr[" << rb << "]; ctx.psq_"
          << (store ? "store" : "load") << "(" << frD << ", ea, "
          << w << ", " << i << ");";
      if (update) out << " ctx.gpr[" << ra << "] = ea;";
      out << " } // psq_" << (store ? "st" : "l")
          << (update ? "ux" : "x") << "\n";
    } else if (xo == 21) {
      out << "    ctx.ps_add(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 20) {
      out << "    ctx.ps_sub(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 25) {
      out << "    ctx.ps_mul(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 29) {
      out << "    ctx.ps_madd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 28) {
      out << "    ctx.ps_msub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 31) {
      out << "    ctx.ps_nmadd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 30) {
      out << "    ctx.ps_nmsub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 18) {
      out << "    ctx.ps_div(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 10) {
      out << "    ctx.ps_sum0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 11) {
      out << "    ctx.ps_sum1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 12) {
      out << "    ctx.ps_muls0(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 13) {
      out << "    ctx.ps_muls1(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 14) {
      out << "    ctx.ps_madds0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 15) {
      out << "    ctx.ps_madds1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 23) {
      out << "    ctx.ps_sel(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo_10 == 72) {
      out << "    ctx.ps_mr(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 40) {
      out << "    ctx.ps_neg(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 264) {
      out << "    ctx.ps_abs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 136) {
      out << "    ctx.ps_nabs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 528) {
      out << "    ctx.ps_merge00(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 560) {
      out << "    ctx.ps_merge01(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 592) {
      out << "    ctx.ps_merge10(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 624) {
      out << "    ctx.ps_merge11(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 0) {
      out << "    ctx.ps_cmpu0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 32) {
      out << "    ctx.ps_cmpo0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 64) {
      out << "    ctx.ps_cmpu1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 96) {
      out << "    ctx.ps_cmpo1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 1014) {
      out << "    ctx.clear_cache_line("
          << (frA ? "ctx.gpr[" + std::to_string(frA) + "]" : "0u")
          << " + ctx.gpr[" << frB << "]); // dcbz_l\n";
    } else if (xo == 26) {
      out << "    ctx.fpr[" << frD << "] = 1.0 / std::sqrt(ctx.fpr[" << frB
          << "]); ctx.ps1[" << frD << "] = 1.0 / std::sqrt(ctx.ps1[" << frB
          << "]); // ps_rsqrte\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 4 (ps_*) XO \" << " << xo
          << " << \" XO_10 \" << " << xo_10
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
      out << "    ctx.fpr[" << frD << "] = 0.0;\n";
      out << "    ctx.ps1[" << frD << "] = 0.0;\n";
    }
  } else if (ppc_inst.opcode() == 10) { 
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = (ctx.gpr[" << ppc_inst.ra() << "] < "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = (ctx.gpr[" << ppc_inst.ra() << "] > "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = (ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.uimm() << ");\n";
  } else if (ppc_inst.opcode() == 11) { 
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] < " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] > " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.simm() << ");\n";
  } else if (ppc_inst.opcode() == 34) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8("
          << ppc_inst.simm() << "); // lbz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lbz\n";
  } else if (ppc_inst.opcode() == 38) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write8(" << ppc_inst.simm() << ", (uint8_t)ctx.gpr["
          << ppc_inst.rs() << "]); // stb\n";
    else
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", (uint8_t)ctx.gpr[" << ppc_inst.rs() << "]); // stb\n";
  } else if (ppc_inst.opcode() == 40) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16("
          << ppc_inst.simm() << "); // lhz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lhz\n";
  } else if (ppc_inst.opcode() == 41) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
        << rA << "]); // lhzu\n";
  } else if (ppc_inst.opcode() == 42) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16("
          << ppc_inst.simm() << "); // lha\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
          << "] + " << ppc_inst.simm() << "); // lha\n";
  } else if (ppc_inst.opcode() == 43) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd()
        << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
        << "]); // lhau\n";
  } else if (ppc_inst.opcode() == 44) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write16(" << ppc_inst.simm() << ", (uint16_t)ctx.gpr["
          << ppc_inst.rs() << "]); // sth\n";
    else
      out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", (uint16_t)ctx.gpr[" << ppc_inst.rs() << "]); // sth\n";
  } else if (ppc_inst.opcode() == 45) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.mmu.write16(ctx.gpr[" << rA << "], (uint16_t)ctx.gpr["
        << ppc_inst.rs() << "]); // sthu\n";
  } else if (ppc_inst.opcode() == 46) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rD << "; i <= 31; ++i) {\n";
    out << "            ctx.gpr[i] = ctx.mmu.read32(addr);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 47) { 
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rS << "; i <= 31; ++i) {\n";
    out << "            ctx.mmu.write32(addr, ctx.gpr[i]);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 50) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64("
          << ppc_inst.simm() << "); // lfd\n";
    else
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lfd\n";
  } else if (ppc_inst.opcode() == 51) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
        << rA << "] + " << ppc_inst.simm() << "); // lfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 54) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write_f64(" << ppc_inst.simm() << ", ctx.fpr["
          << ppc_inst.rs() << "]); // stfd\n";
    else
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfd\n";
  } else if (ppc_inst.opcode() == 55) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
        << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 56) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_load(" << frD << ", " << d << ", " << W << ", " << I
          << "); // psq_l\n";
    else
      out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_l\n";
  } else if (ppc_inst.opcode() == 57) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_lu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else if (ppc_inst.opcode() == 60) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_store(" << frS << ", " << d << ", " << W << ", " << I
          << "); // psq_st\n";
    else
      out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_st\n";
  } else if (ppc_inst.opcode() == 61) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_stu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else {
    out << "    nwii::runtime::micro_interpret(ctx, 0x" << std::hex
        << inst.opcode << ", 0x" << inst.address << std::dec << ");\n";
    out << "    if (ctx.pc != 0x" << std::hex << (inst.address + 4) << std::dec
        << ") return;\n";
  }
}

} 
} 

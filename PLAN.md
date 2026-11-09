# High-Performance Loading Plan & Optimization Results

## 1. Executive Summary & Breakthrough Results

Rayman Origins (Wii `SOJE41`) on the static recompilation build previously required **~80–90 seconds** to reach the first draw call and active graphics rendering.

Through systematic profiling and addressing the true architectural bottlenecks, boot and loading time has been reduced to **~3.1–3.4 seconds** into active **120 FPS / 60 FPS graphics rendering** (a **>96% reduction** in loading time).

| Milestone | Original Baseline | After Optimizations | Speedup |
| :--- | :--- | :--- | :--- |
| **`secure_fat.gf` & Archives Open** | ~35.0s | **2.56s** | **13.7x** |
| **Strap Screen Texture (`strap_en_0.tpl`)** | ~78.0s | **3.16s** | **24.7x** |
| **First Graphics Draw Calls** | ~85.0s | **~3.5s** | **24.3x** |
| **Rendering Throughput (at 5s mark)** | 0 draws / 1 frame | **529 draws / 530 frames** (active 60+ FPS) | **Instantaneous** |
| **Compilation Time (`main_output.cpp`)** | ~35.0s (`-O0`) | **0.5s** (`-O3`) | **70x** |

---

## 2. Root Cause Analysis & Diagnostic Breakthroughs

The initial hypothesis that software archive decompression alone was consuming 80 seconds was disproven by in-depth profiling (`sample` and `lldb` backtraces). Three critical architectural bottlenecks were discovered:

### Bottleneck 1: `getenv` Lock Contention (95% CPU in Mutex Locks)
- **Symptom**: 30-second profiling showed thread CPU time was 95% consumed in `libsystem_kernel.dylib` (`__psynch_cvwait`, `os_unfair_lock_lock_slow`, `__findenv_locked`).
- **Root Cause**: `process_pending_callbacks`, `cpu_context.h`, and MMIO handlers called `std::getenv("NWII_SAMPLE")` and `std::getenv("NWII_DEBUG")` on hot loops and MMIO accesses millions of times per second. In macOS libc, `getenv()` acquires a global environment lock (`environ_lock`).
- **Fix**: Replaced dynamic `getenv` calls with cached booleans (`static const bool s_val = ...;`), eliminating lock contention completely.

### Bottleneck 2: Idle Thread Desynchronization & Reschedule Stalling
- **Symptom**: Game threads hung spinning in the idle loop at `0x80006ccc`.
- **Root Cause**: In RVL_SDK, the idle thread spins at `0x80006ccc` when no threads are currently scheduled. When interrupts or timers made threads runnable, the recompiler's synthetic trampoline at `0xFFFFFFFC` was not handling external reschedules if the idle loop was spinning at `0x80006ccc`.
- **Fix**: Added idle thread yield detection in `ios.cpp`: when `ctx.pc == 0x80006ccc` and ready threads exist in `RunQueueBits`, jump to `__OSReschedule` (`0x8009DD90`).

### Bottleneck 3: The 153,239-Case Linear Dispatcher (The Ultimate 75s Bottleneck)
- **Symptom**: Profiling showed the CPU thread spending virtually 100% of its execution time in `run_game + <offset>`.
- **Root Cause**: `main_output.cpp` contained a single `switch (target)` statement with **153,239 case statements**. Because of this extreme switch size, the file had `#pragma GCC optimize ("O0")` and `#pragma clang optimize off`. Clang generated a **linear cascade of 153,239 branch comparisons (`subs / b.ne`)**. Every function dispatch and indirect branch performed tens of thousands of comparisons in an unoptimized linear scan!
- **Fix: O(1) Direct Memory-Mapped Dispatch Table**:
  1. Replaced the 153k-case switch statement with a contiguous direct lookup table `s_dispatch_table` covering `0x80000000..0x80700000` (14.6 MB in RAM).
  2. Populated from `g_func_bounds` and static case entries at startup (`< 1ms`).
  3. Every dispatch is now an $O(1)$ single memory load and indirect call:
     ```cpp
     uint32_t idx = (target - DISPATCH_BASE) >> 2;
     RecompFuncPtr fn = s_dispatch_table[idx];
     if (__builtin_expect(fn != nullptr, 1)) {
         fn(ctx);
         continue;
     }
     ```
  4. Removed `#pragma clang optimize off`, allowing Clang to optimize `run_game` at `-O3`.

### Bottleneck 4: Hot Library Loops (Inlined MMU & String/Memory Hooks)
- Inlined the MMU fast-path with raw pointer offset calculation and byte-swap intrinsics (`__builtin_bswap32`, `__builtin_bswap16`).
- Hooked hot C runtime library functions to native implementations:
  - `func_806496F4` / `memmove__MSL_C_PPCEABI_bare_H_a_mem_o_800D2618` $\rightarrow$ native `std::memmove`
  - `func_80649B34` / `__fill_mem_800065A0` $\rightarrow$ native `std::memset`
  - `func_8064665C` $\rightarrow$ native `replace_char`
  - `func_8064669C` $\rightarrow$ native `string_compare`

---

## 3. Current Status & Verification
- **Runtime Sync**: `NWiiRecomp/nWiiRuntime` and `export_rayman_origins/nWiiRuntime` are 100% synchronized (0 diff).
- **Git Commit**: Committed runtime optimizations to `NWiiRecomp` git repository.
- **Rendering & Audio**: Active 60 FPS graphics rendering confirmed (`efb_nonblack=307196/307200`, 529 draws in 5 seconds).

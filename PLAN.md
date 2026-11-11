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

## 3. Controller Input Pipeline Diagnosis & Fixes

When user reported input not working, analysis of the game's internal disassembly and runtime revealed three fundamental issues:

### Issue 1: Inverted WPAD Button Constants & Mixed-up Keys
- In Nintendo RVL_SDK, the official WPAD constants are:
  - `WPAD_BUTTON_2`: `0x0001`
  - `WPAD_BUTTON_1`: `0x0002`
  - `WPAD_BUTTON_B`: `0x0004`
  - `WPAD_BUTTON_A`: `0x0008`
  - `WPAD_BUTTON_MINUS`: `0x0010`
  - `WPAD_BUTTON_LEFT`: `0x0100`
  - `WPAD_BUTTON_RIGHT`: `0x0200`
  - `WPAD_BUTTON_DOWN`: `0x0400`
  - `WPAD_BUTTON_UP`: `0x0800`
  - `WPAD_BUTTON_PLUS`: `0x1000`
- In `mouse_keyboard_source.cpp` and `gamepad_source.cpp`, high and low bytes were inverted (e.g. `Space` set `0x0800` which was D-Pad Up, and `Left` arrow set `0x0001` which was Button 2).
- **Fix**: Replaced all bitmasks with correct RVL_SDK definitions across both keyboard and gamepad sources.

### Issue 2: Horizontal (Sideways) Wiimote Layout Mapping
- Rayman Origins is played with a sideways Wii Remote. Disassembly of `func_800EBA5C` inspects an internal table mapping each raw bit to an engine action:
  - Bit 0 (`0x0001`, Button 2): Action 8 (Jump / Accept)
  - Bit 1 (`0x0002`, Button 1): Action 9 (Attack / Dash)
  - Bit 3 (`0x0008`, Button A): Action 10 (Bubble / Select)
  - Bit 2 (`0x0004`, Button B): Action 11 (Back / Action)
  - Bit 11 (`0x0800`, physical Up): Action 0 (Left movement)
  - Bit 10 (`0x0400`, physical Down): Action 1 (Right movement)
  - Bit 9 (`0x0200`, physical Right): Action 19 (Up / Door)
  - Bit 8 (`0x0100`, physical Left): Action 20 (Down / Crouch)
  - Bit 12 (`0x1000`, Button Plus): Action 6 (Pause / Menu)
- **Fix**: When `game_id` is Rayman Origins (`SOJ`) or `NWII_HORIZONTAL` is active, keyboard arrows/WASD and gamepad D-Pad/analog sticks translate correctly to the game's expected physical orientation so Left moves Left, Right moves Right, Up is Up, Down is Down.

### Issue 3: Mock Auto-Pulse Interference
- A previous mock auto-pulse in `pad.cpp` was oscillating `buttons |= 0x0801` every 30 frames, overriding and interfering with real user keypresses.
- **Fix**: Removed recurring oscillation and replaced with a one-time 5-frame pulse at 4s solely for automated headless test scripts, giving the player uninterrupted control.

---

## 4. Current Status & Verification
- **Runtime Sync**: `NWiiRecomp/nWiiRuntime` and `export_rayman_origins/nWiiRuntime` are 100% synchronized (0 diff).
- **Git Commit**: `d900ca5` in `NWiiRecomp` repository.
- **Rendering & Audio**: Active 60 FPS graphics rendering confirmed (`efb_nonblack=307196/307200`, 533 draws in 5 seconds).
- **Input Responsiveness**: Both Keyboard/Mouse and Gamepad fully functional in menus, strap screens, and gameplay.

---

## 5. Runtime Diagnostics

The runtime now reports what it *discards*, not just what it renders. Every bug
found in the GX path so far was silent: a rejected display list, a shader that
failed to link, and a mis-sized draw payload all looked identical to "the game is
slow" or "the screen is wrong".

### Always on

* **Counters** (`include/runtime/diagnostics.h`). Every path that drops
  game-visible work increments one: `dl.rejected`, `fifo.bad_continuation`,
  `fifo.unknown_opcode`, `fifo.resync_bytes`, `fifo.vsize_repaired`,
  `shader.compile_failed`, `shader.link_failed`, `draw.skipped_no_shader`,
  `tex.unknown_format`, `frame.carry_without_end`, `wgp.redirected_bytes`.
* **`[DIAG]` summary** in the 5-second `[STAT]` block whenever any counter is
  non-zero, plus a louder `*** N error-class events across: ...` line. A clean
  run prints nothing.
* **Stuck-picture detector**: three consecutive periods with an identical
  framebuffer hash prints `*** picture unchanged for 15s`.
* **Exit summary**, and `NWII_STRICT=1` makes the process exit `3` when any
  error-class counter is non-zero, so scripted runs are pass/fail.

### The invariant that matters

`fifo.bad_continuation` fires when an accepted command's payload does not end on
another command. That is exactly what a GX framing bug looks like from outside,
and it names the culprit:

```
[DIAG] fifo.bad_continuation x3: cmd 0x90 at 0x1423 sized 8451 -> next byte 0xFF is not a command
```

i.e. "the draw at 0x1423 was given 8451 bytes and the stream does not resume with
a command there" -- the vertex size (VAT/VCD state) disagrees with the stream.

### Kicking it

```sh
NWII_DIAG=info ./RaymanOrigins <game>     # rate-limited detail lines
NWII_STRICT=1 ./RaymanOrigins <game>      # exit 3 on discarded work
tools/nwii_run.sh 20 /tmp/run.log ./RaymanOrigins <game>   # bounded, summary on stderr
```

`tools/nwii_run.sh` always kills with SIGKILL: the guest can leave `SIGALRM`
undelivered, so `alarm(2)`/`timeout(1)` are not reliable bounds here.

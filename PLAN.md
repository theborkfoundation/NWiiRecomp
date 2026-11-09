# High-Performance Loading Plan: High-Level Emulation (HLE) Decompressor

## 1. Problem Diagnosis & Findings

Rayman Origins (Wii `SOJE41`) on the static recompilation build currently requires **~90 seconds** to reach the first draw (strap screen and active 60 FPS graphics).

### Profiling Results (5-Second macOS `sample` Analysis)
- **Main / Render Thread**: Spends 96.1% of time in `Cocoa_GL_SwapWindow` / `SDL_CondWait_REAL` waiting for vertical sync intervals.
- **CPU Thread**: Spends 98.8% of time actively executing inside `run_game` in recompiled PowerPC code (`func_80604B88`, `func_806449B4`, `func_806496F4`).
- **Core Utilization**: ~142% across cores; the game is not hung, locked, or waiting on disk I/O.

### Root Cause
1. **Software Archive Decompression**:
   - The game's UbiArt Framework packs assets into compressed `.ipk` archives (e.g., `bootsequence_WII.ipk` containing ~107 MB of assets).
   - In Dolphin, this code runs under an optimizing JIT with direct hardware register allocation ($r0–r31 \rightarrow X0–X31$) and block chaining, completing decompression in milliseconds.
2. **Static Recompilation Overhead for Emulated Decompression**:
   - In `NWiiRecomp`, every guest PowerPC instruction maintains emulated CPU structures (`++ctx.inst_count`, `ctx.cr[0].lt/gt/eq/so` calculation for every operation).
   - 13,682 call sites evaluate `process_pending_callbacks(ctx)` across inner loops and function boundaries.
   - Even with an inlined MMU fast-path (2 ARM64 instructions per load/store), executing billions of emulated decompression instructions in interpreted C++ takes ~90 seconds.

---

## 2. Decision: Option 1 (HLE Decompression)

We have chosen **Option 1: High-Level Emulation (HLE) of the Decompressor**.

### Rationale
- **Instant Speedup**: Native decompression (e.g., LZO1X / Deflate) running directly in C on an Apple M4 CPU processes data at >3 GB/s, decompressing 107 MB in **~20 to 50 milliseconds** (reducing loading time from 90s to <1s).
- **Surgical & Non-Destructive**: Replaces the inner decompression routine without modifying game disc assets or altering game logic.
- **Standard Industry Practice**: High-Level Emulation of compression/decompression is standard in modern static recompilation and emulation projects (e.g., *N64Recomp*, *RPCS3*, *Dolphin*).

---

## 3. Implementation Roadmap

### Phase 1: Analysis & Function Signature Discovery
- [x] Document root cause and decision in `PLAN.md`.
- [ ] Determine the exact calling convention, input buffer (`r3`/`r4`), output buffer, compressed size, and decompressed size for the decompression entrypoint (`func_80604B88` / `func_806449B4`).
- [ ] Identify the compression algorithm (LZO1X vs. Deflate/Zlib) by inspecting the `.ipk` header and magic bytes.

### Phase 2: Native Decompressor Integration
- [ ] Add native decompressor utility (e.g., embedded LZO1X or miniz / zlib) into `nWiiRuntime`.
- [ ] Implement an HLE hook for `func_80604B88`:
  1. Retrieve source buffer pointer via `ctx.mmu.get_ptr(src_addr)`.
  2. Retrieve destination buffer pointer via `ctx.mmu.get_ptr(dst_addr)`.
  3. Execute native fast decompression in C.
  4. Set return value (`ctx.gpr[3]`) and return immediately (`ctx.pc = ctx.lr;`).

### Phase 3: Runtime Synchronization & Rebuild
- [ ] Apply HLE hook to both `NWiiRecomp/nWiiRuntime` and `export_rayman_origins/nWiiRuntime`.
- [ ] Ensure `diff -rq NWiiRecomp/nWiiRuntime export_rayman_origins/nWiiRuntime` reports 0 differences.
- [ ] Rebuild `RaymanOrigins` using `cmake --build export_rayman_origins/build -j10`.

### Phase 4: Benchmarking & Verification
- [ ] Benchmark loading time to first draw (`g_stat_draws > 0`).
- [ ] Target metric: **< 2 seconds boot time** into active 60 FPS graphics rendering.
- [ ] Verify clean runtime logs and commit changes to `NWiiRecomp`.

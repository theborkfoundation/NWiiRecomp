# Recompiler regressions

Build and run without a game dump:

```sh
cmake -S . -B build -DNWII_BUILD_TESTS=ON
cmake --build build --target indexed_memory_test ipc_interrupt_test write_gather_test -j8
ctest --test-dir build --output-on-failure
```

`indexed_memory_generator` feeds synthetic PPC instructions through the actual
recompiler. CMake compiles the resulting C++ into `indexed_memory_test`, which
executes it against guest RAM. Checks cover RA=0 with a poisoned GPR0, nonzero
bases, byte-reversed loads targeting r0, byte-reversed stores, indexed scalar and
paired floating loads/stores, and storing FPR bits with stfiwx. These reproduce
the addressing error that corrupted Rayman's Bink header during byte swapping.

Carry/record tests execute subfic and addic followed by addze, cover zero and
32-bit boundary values, check addic./andis. condition flags, and verify sraw
carry when bits are shifted out (including shifts of 0, 31, 32 and 63).

Additional checks cover both startup-task completion orderings through actual
instruction overrides, scalar single FP lane duplication, paired scalar-source
aliasing, and negative quantization scales.

Build `ipc_interrupt_test` as well to run the real IPC MMIO handlers through
queued replies, latch acknowledgement, the physical register alias and interrupt
masking. This verifies Wii IPC uses PI bit 0x4000 and preserves queued replies
until the guest acknowledges them.

`write_gather_test` checks actual redirected MEM2 writes (including base zero),
32-byte burst completion, flush padding, FIFO restoration, and MMU writes at
CC008004. It also parses one-shot BP masks, checks renderer state is not seeded
by future commands, verifies konst selections affect shader keys, and reuses a
display list under two different matrices to catch stale cached snapshots.

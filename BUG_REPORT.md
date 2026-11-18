# Bug report

_Last updated: 2026-11-15._

All findings are recorded under [Resolved](#resolved-reference). The
translation-performance measurements were taken against the Rayman Origins
export with the local Apple arm64 compiler; they are not presented as universal
whole-game benchmarks.

Findings carry stable IDs (`I-NNN`); use them in commits, reviews and issue
tracking. Fixed findings keep their ID in the resolved table.

## Profiling candidates (no action yet)

Every translated instruction emits `++ctx.inst_count` (`recompiler.cpp`, around
line 1047), and local backward branches call `process_pending_callbacks` on each
traversal (`recompiler.cpp`, around line 1295). The Rayman export contains about
1.78 million count sites and 13,668 callback-check sites; these are static counts,
not execution frequencies. The callback function has a fast path but still
performs state checks before returning
(`NWiiRecomp/nWiiRuntime/src/hle/ios.cpp`, around line 903). Indirect calls and
external tail branches also return through the central dispatcher, which performs
callback work and function lookup.

Profile optimized game execution before changing these paths: collect samples and
dynamic counts by guest PC, including time in callback checks, lookup, and
instruction bookkeeping. Potential batching or inline fast paths must preserve
instruction-based timing, interrupt latency, context restoration, and guest
control flow.

## Resolved (reference)

| IDs | Round | Area | NWiiRecomp commit |
|---|---|---|---|
| I-001 – I-005 | 1 | HLE direct-call return contract, explicit hook precedence, `OSCreateHeap` bounds, Metal backend fallback, GL 3.3 / GLSL 330 consistency | `7ffe576` |
| I-006 – I-009 | 2 | `/dev/fs` seek/read bounds, NAND error propagation, `divw` undefined behaviour, title-directory derivation | `59d2d8b` |
| I-010 – I-016 | 3 | `lwarx`/`stwcx.` reservations, `lhax` RA=0, CR0.SO, `neg`/`mulli` UB, complete OE arithmetic decoding, `fctiw`/`fctiwz` saturation, fused doubles | `d178694`, `4e6e1f9` |
| I-017 | 4 | Removed the legacy non-split dispatcher; split output is the only path | `d22d0d5` |
| I-018 – I-021 | perf | Dense resume-dispatch tables, cached branch-target lookups, per-unit declarations, Release build default | `c10aa73` |
| I-022 – I-025 | 5 | OE arithmetic operand snapshots, FPSCR.RN bit selection, record-form CR1 updates, indirect-branch target masking | `494d52d` |
| I-026 – I-028 | 5 | DI output-size and RAM-span validation, disc-gap data preservation, virtual-NAND write wrap | `494d52d` |

Verification: `ctest` 4/4 (including the `translator_edge` test), Rayman Origins
regenerated and rebuilt, boot-stall soak stable, and in-game play-tested. Runtime
issues that are not source-review findings (for example the residual intermittent
boot stall) stay tracked in `PROGRESS.md`.

Translation-performance verification (2026-11-15): `ctest` 6/6, adding
`recompiler_resume` (interior resume for dense and sparse functions, compiled
optimized) and `recompiler_cmake` (generated build defaults). Rayman Origins was
regenerated (89 units, 344.6 MB of generated source) and clean-built, and a
windowed run rendered at the same pace as the previous build (248/548/848 frames
at 5/10/15 s).

- I-018: `func_80326108` (3,268 resume points) compiles in 2.82 s with 179,756
  bytes of `__TEXT`, versus 6.22 s and 216,266 bytes for the per-address switch.
  The switch-free variant (2.33 s, 76,844 bytes) is still not equivalent; the
  shipped table keeps every interior resume point.
- I-019: self-branch generation is now linear: 0.018/0.037/0.069 s for
  4,000/8,000/16,000 instructions, versus 0.072/0.256/0.982 s.
- I-020: split units no longer include the 1.56 MB, 29,448-line `functions.h`.
  A clean `-j8` Release build of the export took 227.8 s wall (1,710 s CPU)
  versus 279.8 s (2,106 s CPU); both builds selected Release explicitly, so
  I-021 does not contribute to that difference.
- I-021: generated single-config projects default to Release when no build type
  is supplied; the README documents Release builds for measurements.

Round 5 verification (2026-11-15): `ctest` 8/8, adding `translator_semantics`
(generated OE arithmetic with aliased destinations, record-form CR1, indirect
branch masking) and `device_bounds` (DI output bounds and RAM spans, disc reads
across a region gap, virtual-NAND write wrap), and extending `translator_edge`
with FPSCR.RN and CR1 coverage. Rayman Origins was regenerated and rebuilt; it
booted and rendered at the previous pace (250/550/850 frames at 5/10/15 s). One
boot attempt hit the known intermittent idle stall tracked in `PROGRESS.md`;
four retries and the 15 s run were normal.

## Reporting a finding

Keep entries unranked until triage, assign the next free `I-NNN` ID, and use this
shape:

- **Severity**: High / Medium / Low
- **Location**: file + line or function
- **Symptom**: what it does versus what it should do
- **Repro**: minimal sequence that shows it (say whether it was reproduced in a
  running game or found by reading the code)
- **Suggestion**: proposed fix or test

When an entry is fixed, keep its ID and move it to the resolved table with the
commit hash and a one-line note on how it was verified.

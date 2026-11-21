#pragma once

namespace nwii::runtime {
struct CPUContext;
// SOJE41's video-only Bink call. Returns false to run the original guest body.
// Configured per game through [optional_hooks] in the recompiler TOML, so a
// regenerated export keeps wiring it up without hand-editing generated code.
bool decode_rayman_bink_frame(CPUContext &ctx);
}

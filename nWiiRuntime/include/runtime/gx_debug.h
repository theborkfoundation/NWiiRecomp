#pragma once

namespace nwii::runtime::gxdbg {
// Live render toggles (F1..F4 in the visible loop) for diagnosing draws.
extern bool no_alpha_test;  // skip the alpha-test discard
extern bool alpha_view;     // paint texture alpha as greyscale
extern bool no_tex;         // paint draws solid white
extern bool no_blend;       // disable blending
extern bool skip_blend;    // do not draw blended primitives
extern bool only_blend;    // draw only blended primitives
extern int solo_count;     // if non-zero, draw only primitives with this vertex count
extern unsigned skip_tex_addr;  // do not draw primitives using this tex0 base
extern bool no_cull;       // disable backface culling
extern bool no_depth;      // disable depth testing
} // namespace nwii::runtime::gxdbg

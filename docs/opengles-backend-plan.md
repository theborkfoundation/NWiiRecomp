# OpenGL ES 3.0 rendering option

Status: implementation complete for the shared GL/ES renderer and validated on
macOS/ANGLE, 2026-11-16. Profile selection, context validation, ES shader
output, the GL/ES call and format audit, EFB presentation and the EFB readback
replacement are implemented.

Validated on macOS arm64 (Apple M4) with a local ANGLE 2.1 distribution over
Metal (`libEGL.dylib`/`libGLESv2.dylib`, arm64, ad-hoc signed; ANGLE reports
"git hash: unknown hash"): ES 3.0 context and limits, TEV/XFB shader
compile+link, EFB rendering, EFB texture readback, XFB presentation, and
pixel-identical early frames versus desktop GL. Desktop GL 3.3 regression and a
standalone export build (including `-DNWII_ENABLE_GLES=OFF` refusal) also pass.

Still unperformed: Linux/EGL Mesa and other ES drivers, resizing under ES, a
full title playthrough with fixed checkpoints, and exactly time-aligned
comparison of late, time-varying frames. No platform other than the tested
macOS + ANGLE configuration is claimed.

## Goal and approach

Add a selectable OpenGL ES 3.0 rendering option to the runtime and generated standalone projects, preserving the existing desktop OpenGL 3.3 default and headless behavior. Reuse the existing GL renderer, GX command processing, texture decoding and SDL presentation path with a runtime-selected GL/GLSL profile. Match the current desktop renderer's behavior first; this work does not promise full GX accuracy or higher performance.

Initial acceptance requires a working ES 3.0 implementation on at least one available desktop test platform and an unchanged desktop-GL build. Record the actual operating system, GPU/driver, ES implementation and capabilities tested. Linux/native GLES or a functioning desktop ES implementation are suitable first targets. macOS via ANGLE/Metal is a separate platform qualification: do not claim it works until the exact SDL2 2.28.5 configuration, context creation, ANGLE distribution, and real rendering are tested. Android/iOS packaging is outside this change unless explicitly requested. No silent fallback when GLES is explicitly selected; give an actionable error.

ES is a rendering API/profile, not a promise of native drivers: ANGLE can translate ES 3.0 to another API. Packaging and performance vary by platform. See [SDL context attributes](https://wiki.libsdl.org/SDL2/SDL_GLattr), [SDL ES driver selection](https://wiki.libsdl.org/SDL2/SDL_HINT_OPENGL_ES_DRIVER), and [ANGLE platform support](https://chromium.googlesource.com/angle/angle).

## Current integration points

- `nWiiRuntime/include/runtime/config.h` defines `Backend::OpenGL` and `Metal`; `src/core/config.cpp` defaults to OpenGL and falls back for Metal. `src/hle/gx/renderer_factory.cpp` returns the GL renderer for the normal path. Add a distinct selectable GLES profile without accidentally routing it to the unfinished Metal stub.
- `src/core/main.cpp` requests a 3.3 core context, loads desktop GLAD, and owns the EFB framebuffer, XFB upload/presentation, screenshots, swap and context cleanup. Both GL profiles need this code. `GX_SetWindow` is called, while `gx.cpp` initializes the renderer with `nullptr`; avoid assuming `IRenderer` currently owns the window or presentation. Headless skips window and rendering.
- `src/hle/gx/renderer_gl.cpp` renders ordered `GXCommand`s and generates/compiles TEV shaders. `src/hle/gx/tev_shader_gen.cpp` outputs desktop GLSL 330; `main.cpp` has separate GLSL 150 XFB shaders. `src/hle/texture_cache_gl.cpp` handles GL texture creation and contains a desktop-only `glGetTexImage` in the optional EFB texture-dump path. The remaining GL calls and formats still need a complete GLES audit.
- `nWiiRuntime/CMakeLists.txt` uses a vendored or fetched desktop GL 3.3 GLAD loader. `nWiiRecomp/src/recompiler.cpp` copies runtime sources into exports and emits their top-level CMake. A clean export is a required deliverable, not just an in-tree build.

## Implementation steps

1. **Establish baseline.** Record current build/test commands and desktop GL screenshots or frame hashes for representative user-supplied scenes, including 2D, 3D, texture, blending, depth, EFB copy and XFB. Record existing shader errors and known rendering limitations. Reuse the existing diagnostic flags and avoid committing game data. Inspect the actual GL call/enum inventory, texture formats, state, draw ordering and driver limits before changing code.
2. **Select the profile.** Add `Backend::OpenGLES` and parse `[graphics] backend = "OpenGLES"` case-insensitively for the documented spelling; retain OpenGL as the default and preserve existing Metal behavior. Both profiles use the same `RendererGL`/GL-style path; pass an explicit profile to context setup and shader generation. For ES request `SDL_GL_CONTEXT_PROFILE_ES` and version 3.0 before window/context creation, without the desktop-only forward-compatible flag. Verify returned API/profile/version, required entry points and capabilities after context creation; report selected implementation, renderer, driver and GLSL version. A requested but unavailable profile must return a nonzero error with SDL/loader details, rather than falling through to GL or Metal. Do not change headless's lack of a graphics context.
3. **Function loading and build.** Choose and document a loader strategy that supports desktop GL 3.3 and GLES 3.0 in the same executable and obtains entry points from the active SDL context. Do not blindly link two generated loaders with colliding `gl*` names or assume the current desktop-only GLAD correctly identifies ES. A unified or namespaced dispatch layer is acceptable after validating both profiles. Make generated loader sources reproducible, preserve license notices, and make them available in copied runtime exports. Optional build-time disablement of GLES must leave a clear config error. Avoid a new SDK or ANGLE requirement for existing desktop GL-only builds.
4. **Profile-aware shaders.** Add a small explicit shader dialect argument to `GenerateTEVShader` and the XFB presentation shader builder. Desktop output remains equivalent; ES emits `#version 300 es` and appropriate vertex/fragment floating-point and integer precision (use high precision where needed for TEV correctness). Audit all generated constructs, uniform and sampler array indexing, stage interfaces, loop limits and fragment outputs against GLSL ES 3.00. Check shader compile *and link* failures, logging the profile and useful source/key, and never use a failed program. Cache keys and cache lifetime must include profile/context if both can ever coexist or contexts can be recreated. Keep shader permutations and state behavior unchanged otherwise.
5. **ES compatibility.** Audit all runtime GL entry points, constants and texture/framebuffer internal formats against ES 3.0, including debug paths and `main.cpp`. Replace the `NWII_TEXDUMP` EFB `glGetTexImage` with an ES-compatible temporary framebuffer and `glReadPixels`, or explicitly report unsupported diagnostic operation; if implemented, verify attachment completeness, actual image dimensions/format, pixel packing/orientation and restore read framebuffer, read buffer, texture bindings and other modified state. Check framebuffer completeness and GL errors during development. Verify depth format, readback, `glCopyTexImage2D`, framebuffer blit restrictions and default framebuffer behavior on the actual ES context; choose explicit compatible formats or an equivalent draw path where needed. Preserve EFB copy order, clear, scaling/orientation, XFB, screenshots and diagnostic behavior.
6. **Capabilities and failures.** Query relevant limits before rendering: texture units, varying and uniform limits, texture sizes, framebuffer support and any extensions actually relied on. Current generated shaders declare eight texture coordinates/stages and sizeable lighting uniform arrays; the ES 3.0 minimums alone do not prove every permutation fits. Compile/link representative permutations and report inadequate limits or shader failure clearly; avoid silently substituting a wrong program. Document hardware/driver limitations rather than lowering precision without image comparison.
7. **Documentation and export.** Document configuration, prerequisites, expected error behavior and tested platform matrix in runtime/project docs. Ensure `nWiiRecomp` exports all sources and dependency configuration required by the chosen loader, and that a fresh export can configure and build independently with GLES enabled and disabled. If ANGLE is supported, specify exact acquisition, version, libraries, runtime lookup and licenses; do not assume it is included in macOS.

## Validation and completion criteria

- Run existing CTest suite and build the normal project. Verify a clean standalone export build. Exercise default OpenGL, explicit OpenGL, GLES selected but unavailable/disabled, and headless mode; selection errors must be visible and deterministic.
- On an actual ES 3.0 context, compile and link the XFB shader plus representative generated TEV variants, including multiple textures, alpha test and lighting. Exercise empty/invalid context and shader failure paths. A source-string-only unit test is not proof of driver compatibility.
- Render repeatable fixtures for triangles, depth, blend, textures and copy-to-texture with clear; compare against desktop GL captures with documented per-fixture tolerances and orientation. Exercise `NWII_XFB`, `NWII_SCREENSHOT`, `NWII_TEXDUMP`, hidden window, resizing, vsync/pacing and orderly shutdown. Test relevant GL errors and framebuffer status. Preserve current frame skipping, FIFO and guest timing behavior.
- Run at least one representative user-supplied title through repeatable checkpoints on both profiles and report screenshots, platform and known differences. Do not commit game assets. Headless-only execution or a successful compile does not meet the graphics acceptance gate.
- Where a platform, display, ES driver, ANGLE setup, or game data is unavailable, explicitly mark that validation as unperformed; do not describe the backend or that platform as fully verified. Test macOS/ANGLE separately before advertising support there.

## Suggested delivery order

Land profile selection and loader integration first with desktop GL regression coverage; then add ES shader output and a context-backed smoke test; then close texture/readback/presentation gaps and compare gameplay captures; finally validate exports and publish the tested platform matrix. Keep changes small enough to identify whether failures arise from context creation, shaders, GL compatibility or pre-existing GX emulation.

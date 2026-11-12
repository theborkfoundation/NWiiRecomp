# Rayman Origins (SOJE41)

`rayman_origins.toml` expects a legally obtained, unpacked game at
`../games/rayman_origins`. Run the compiler from the repository root. The
configuration writes `../export_rayman_origins`.

The first two `instruction_overrides` repair an engine startup-task publication race:
the worker can finish before thread creation returns. It clears its pending
handle, but the creator then overwrites that completion with a stale handle.
Boot waits forever and never starts its configured resource preloads. Publish
pending before starting the worker, and preserve a completion that arrives
before handle publication. The patch does not skip the worker, videos or loads.
The third override captures a display list when the game submits it, before
its RAM buffer can be reused. These addresses apply specifically to SOJE41.

`instruction_overrides` maps hexadecimal instruction addresses to trusted C++
statements replacing that instruction. Generated labels and instruction counts
remain intact for dispatcher resumptions. Like HLE hooks, these statements are
compiled as code; use only configuration you trust.

Run the game from its build directory so shaders resolve:

```sh
cd ../export_rayman_origins/build
./RaymanOrigins ../../games/rayman_origins
```

Regression checks are documented in `tests/README.md`. The current build has been run through the remote instructions to an interactive
menu using the normal loading path. Some background geometry and menu text still
have rendering artifacts; gameplay has not been validated. No video skip or
forced loading flags are needed.

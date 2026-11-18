# Keyboard / mouse / gamepad controls (NWiiRecomp)

Two input modes are available. The default (no env) is the **horizontal
Wiimote** layout used by Rayman Origins (`SOJE41`). Set
`NWII_INPUT=classic` to report a **Classic Controller** instead.
`NWII_REMOTE=horizontal|vertical` overrides the Wiimote orientation (SOJ/SMN
games default to horizontal).

## Default: horizontal Wiimote

| Action | Keys / mouse |
|---|---|
| Menus / accept / jump (`2` + `A`) | `Space`, `Enter`, `K`, `2`, `X`, **left click** |
| Attack / run / back (`1` + `B`) | `J`, `1`, `Z`, `Left Shift`, **right click** |
| Bubble (`B`) | `C` |
| Pause / menu (`+`) | `Esc`, `P`, `=` |
| Select / stats (`-`) | `-`, `Tab`, `Backspace` |
| Left | `←` or `A` |
| Right | `→` or `D` |
| Up | `↑` or `W` |
| Down | `↓` or `S` |

The mouse also drives the Wiimote pointer (IR), so left-click works like
`2`/`A` for UI elements.

## Classic Controller (`NWII_INPUT=classic`)

Keyboard / mouse:

| Classic button | Keys / mouse |
|---|---|
| `A` | `Space`, `Enter`, `K`, `2`, `X`, left click |
| `B` | `J`, `1`, `Z`, `Left Shift`, right click |
| `X` | `U` |
| `Y` | `I` |
| `L` | `Q` |
| `R` | `E` |
| `ZL` / `ZR` | gamepad triggers only |
| D-pad | arrow keys |
| Left stick | `W` / `A` / `S` / `D` |
| `+` | `Esc`, `P`, `=` |
| `-` | `-`, `Tab`, `Backspace` |

Xbox-style gamepad (SDL game controller):

| Classic | Gamepad |
|---|---|
| `A` / `B` / `X` / `Y` | A / B / X / Y |
| `L` / `R` | LB / RB |
| `ZL` / `ZR` | LT / RT (analog value also reported) |
| Left / right stick | left / right analog stick |
| D-pad | D-pad |
| `+` / `-` / Home | Start / Back / Guide |

## Default-mode gamepad (Xbox-style)

A → `2`+`A`, X/Y → `1`, B → `B`; LB/LT → `1`, RB/RT → `B`;
Start → `+`, Back → `-`, Guide → Home; D-pad or left stick → movement;
the right stick moves the IR pointer.

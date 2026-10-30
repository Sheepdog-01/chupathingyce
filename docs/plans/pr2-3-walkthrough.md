# Walk-through: in-game touch controls and the TOUCH page

Stacked branches on the fork, each on the one before:

| Branch | Commit | What |
| --- | --- | --- |
| `touch-menus-desktop` | `9d45be8`, `4489e51` | PR 1, plus a server stub it was missing |
| `touch-scroll-direction` | `33b9256`, `b6a2e9a` | `input.touch_scroll`, row-sized steps |
| `touch-overlay` | `21bd429` | PR 2: the controls in a game |
| `touch-settings` | `6768284` | PR 3: Controls Setup's TOUCHSCREEN page and the layout editor |

Build `touch-settings` to test everything.

## PR 2: the controls in a game

**The seam already existed.** Android's Java overlay hands the game a
7-value pad state and a look delta through four `host_touch_*` functions,
and `touch_input.c` turns those into an `XINPUT_GAMEPAD`. That half of
`touch_input.c` was Android-only; now it calls tiny wrappers
(`controls_read` and friends) that pick Android's host functions or the
new `touch_overlay.c`. The game code doesn't change.

**`touch_overlay.c` is Android's controls in C.**
- Same 18 controls, same places, same sizes. Places live on Android's
  960x540 grid, stretched over the picture; sizes in Android's logical
  pixels. So a layout means the same on both platforms.
- Same finger rules: a finger owns the control it went down on; a button
  finger that moves 10 logical pixels also turns the view (fire and aim
  with one thumb); a finger away from the controls turns the view; the
  stick catches a finger 1.28x beyond its ring, with a 12% dead zone; a
  press shorter than a frame is held 60 ms so the game sees it.
- **Mapping to the screen:** `halo_ui_point_to_window` (the menus' own
  mapping) gives where the 640x480 layout sits in the window; the overlay
  inverts it for fingers, so touches and drawing agree at any scale.

**When they show** (`shown_now`): in a game, no menu or cinematic up, and
`input.touch_controls` is "on", or "auto" (the desktop default) while the
touchscreen was the last device used. `xinput_sdl.c` gained
`last_input_touch`: a finger sets it, a key, a mouse move or click, or a
controller clears it. So a laptop with a touchscreen never shows them
until touched, and your bare Surface shows them on the first touch in a
game.

**Drawing** goes through `ui_overlay.c` (the code-drawn screens' drawer),
called from `render_ui_widgets`, which runs every frame in play.
- `input.touch_theme` "xbox": A/B/X/Y, both triggers, both bumpers, Start
  and Back use ChupathingyCE's own controller icons (`port/assets/prompts`).
  Crouch, Zoom and the D-pad have no icon, so they get labels.
- "simple": labelled circles, as Android draws them.
- The stick: a dark ring with an inner guide ring, and a light thumb
  "island" lit from above with faint grip rings, which follows the finger.
- `input.touch_size`, `input.touch_opacity` (labels stay solid down to the
  default 60%), `input.touch_sensitivity`.

**Builds that needed care:**
- The dedicated server compiles `touch_input.c` and `touch_overlay.c` but
  not `sdl_platform.c` or `xinput_sdl.c`, so `server_platform.c` stubs the
  two functions they call. PR 1 had the same gap (`4489e51` fixes it).
- On Android, `touch_overlay.c` compiles to empty stubs.

## PR 3: the TOUCHSCREEN page and the editor

**The page.** Controls Setup's DEVICE gained TOUCHSCREEN
(`tools/port_settings.py`; both settings files regenerated, and a run of the
unmodified generator first reproduced the committed files byte for byte).
If a finger opened the screen, it starts on that page. Rows:

| Row | Setting | Where |
| --- | --- | --- |
| SHOW CONTROLS | `input.touch_controls` (AUTO, ON, OFF) | desktop, Android |
| MENU SCROLL | `input.touch_scroll` (FOLLOW, NATURAL) | desktop, Android |
| BUTTON STYLE | `input.touch_theme` (XBOX, SIMPLE) | desktop |
| SIZE | `input.touch_size` (50 to 200%) | desktop |
| OPACITY | `input.touch_opacity` (10 to 100%) | desktop |
| LOOK SENSITIVITY | `input.touch_sensitivity` (0.25 to 4) | desktop |
| EDIT LAYOUT | opens the editor | desktop |

They save with the page's ACCEPT like every other row; DEFAULTS resets the
page's settings (the layout has its own RESET in the editor).

**The editor** (`touch_overlay.c`, "port touch layout edit"):
- Dims the screen, draws every control where it is (hidden ones faint),
  and a panel in the middle with SMALLER, BIGGER, HIDE/SHOW, RESET,
  CANCEL, SAVE.
- A finger on a control chooses it (yellow ring) and drags it. Buttons act
  when released on them. One finger at a time.
- SAVE writes `input.touch_layout` (`kind,x,y,size,shown;` per control), or
  empty if it matches the usual layout. CANCEL drops the edits.
- While it's open, every finger goes to it, and `ui_widget.c` returns early
  from the menus' update (input flushed, mouse too), as the virtual keyboard
  does.

**Not covered yet:** the Xbox-style menus' Controls screen
(`controls_screen.c`, code-drawn) has no touchscreen page.

## Checked

- Every touched C file compiles without errors or warnings with the build's
  own flags (desktop, the Android variant, and the server stub file).
- Upstream's menu tests pass, including the one checking every menu setting
  exists in the config table, and the touch-gesture and controller-layout
  tests. Other test failures here are the sandbox's missing headers, and
  match the unmodified branch.
- An independent review of the diff (results folded in separately).
- Not yet run: the Windows build and your Surface.

## Test on the Surface

1. **First touch in a game:** start a level, touch the screen. The
   controls appear. The stick moves you; swiping the right side turns the
   view; Fire shoots.
2. **Fire and aim with one thumb:** hold Fire and slide: it keeps firing
   while turning.
3. **A tap:** a quick tap on Jump jumps.
4. **Pause:** the Pause button opens the pause menu; the controls hide and
   taps work the menu. Resume: they come back.
5. **Settings > Controls Setup:** it opens on TOUCHSCREEN. Change BUTTON
   STYLE to SIMPLE, SIZE to 125%, ACCEPT, back in game: they changed.
6. **EDIT LAYOUT:** drag Jump somewhere, choose it, BIGGER twice, choose
   Light and HIDE it, SAVE. In game: Jump moved and grew, Light gone.
   Reopen the editor: Light is there, faint. RESET, SAVE: back to usual.
7. **Hand-off:** with a keyboard or mouse to hand, using it hides the
   controls; touching brings them back.

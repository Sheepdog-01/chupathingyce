# Touch controls on the desktop builds (Windows first)

Status: plan, not started. Target device: a Surface Pro 9 as a bare tablet
(no Type Cover, mouse or controller). Milenko's go-ahead: 2026-10-29.

## What's already there

The Windows build compiles the Linux platform layer (`port/windows/port.json`
only swaps out the POSIX files), so `sdl_platform.c`, `xinput_sdl.c`,
`touch_input.c`, `touch_menu.c`, `ui_overlay.c` and `game/touch_game.c` are
all in `halo.exe` today. Touch is switched off by `#ifdef HALO_ANDROID` at
these points:

| Where | What the guard switches off |
| --- | --- |
| `sdl_platform.c` ~385 | `SDL_HINT_TOUCH_MOUSE_EVENTS "0"`. On Windows a finger is also a fake mouse today |
| `sdl_platform.c` ~2320 | `touch_input_cancel()` on focus loss |
| `sdl_platform.c` ~2335 | Finger events reaching `touch_input_event()` |
| `sdl_platform.c` ~2525 | The menus' pointer reading the touchscreen (desktop reads the mouse) |
| `xinput_sdl.c` ~155 | `touch_input_look()`: the swipe's yaw/pitch, kept off mouse aiming so aim assist stays |
| `xinput_sdl.c` ~1766 | `touch_input_controls()`: the overlay's virtual pad merged into port 0 |
| `xinput_sdl.c` ~1802 | Rumble to the overlay (no motor on a Surface; leave off) |
| `touch_input.c` ~19, ~188 | The `host_touch_*` imports and the overlay half of the file |

**The one missing piece is the overlay itself.** On Android it's Java
(`TouchControls.java`, `TouchLayout.java`) handing a 7-value pad state and a
look delta over JNI (`port/android/host/host_touch.c`). `touch_input_controls()`
and `touch_input_look()` already turn those into an `XINPUT_GAMEPAD`, so a C
overlay that provides the same four `host_touch_*` functions slots in with
no game changes.

`ui_overlay.c` (rounded rects, outlines, text, the controller's button
icons, drawn at Present over the picture, on both the GL and bgfx backends)
is where the C overlay draws. Its layout is 480 lines high; a wider window
reaches past 0 and 640 on x (3:2 at 1920 lines gives about −40..680).

## Design

1. **Finger events on every SDL build.** Drop the Android guards around
   finger routing and the touch-to-mouse hint. With the hint off, the mouse
   stays the mouse and a finger is only a finger.
2. **Touch as an input device.** `xinput_sdl.c` tracks the last device used
   for button prompts. Add touch to it: a finger makes touch "last used";
   a key, mouse move or pad press takes it back. The controls show while
   touch is last used, so a desktop with a touchscreen never shows them by
   accident, and a bare tablet shows them on the first touch.
3. **Menus.** While a menu is up and touch was last used, the menus' pointer
   reads `touch_menu` (tap to select, drag to scroll) instead of the mouse.
   This is Android's path, already written.
4. **`touch_overlay.c`** (new, `port/linux/src`, desktop only):
   - The defaults table from `TouchLayout.java` (960×540 reference, same
     radii), scaled by `min(w/960, h/540)`.
   - Per-finger tracking by `SDL_FingerID`: a finger is owned by the control
     it landed on (stick, button) or by look. A finger on a button can drag
     to look, as on Android.
   - Provides `host_touch_read`, `host_touch_look_read`, `host_touch_scene`
     and a no-op `host_touch_rumble`, with the same 60 ms tap hold so a quick
     tap still reaches the game's 30 Hz read.
   - Draws each frame through `ui_overlay_*`, buttons labelled with the
     controller icons the prompts already use.
5. **Guard.** Replace `#ifdef HALO_ANDROID` at the points above with one
   `HALO_TOUCH_CONTROLS` defined for Android and the SDL desktop builds, so
   the reason for the guard reads in the code.
6. **Setting.** `input.touch_controls` gets a desktop meaning: `"auto"`
   (default: shown while touch is last used), `"on"`, `"off"`.

Not in scope for now: the layout editor, gyro, Windows edge-swipe zones
(Windows 11 shows a gripper on the first edge swipe in fullscreen, so it's
a nuisance, not a blocker), the pen (it stays a mouse).

## PRs

| PR | Contents | Done when |
| --- | --- | --- |
| 1 | Steps 1–3, 5: finger routing, hint, touch as last device, menus by touch | The Surface walks the menus and starts a level by touch alone |
| 2 | Step 4, 6: the C overlay with the default layout | The Pillar of Autumn (a10) is playable by touch alone |
| 3 | Hide button, look sensitivity, layout saved to a file beside `config.toml` | Layout survives a restart |

Each PR: small enough to understand every line, built by the PR's Windows
job (`.github/workflows/build.yml`, free on a public fork), tested on the
Surface, and disclosed as written with Claude.

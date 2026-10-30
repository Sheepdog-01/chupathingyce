# PR 1 walk-through: menus by touch on the desktop builds

Branch `touch-menus-desktop` (one commit, `9d45be8`, on upstream `main` @ `7470461`).
Four files, about 110 lines. Read in this order.

## 1. The hint (`sdl_platform.c`, `platform_sdl_initialize`)

```c
SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");      /* was Android-only */
#ifndef HALO_ANDROID
SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
#endif
```

**Why:** by default SDL reports every finger twice, as a finger and as a
fake mouse. The game's mouse drives aim and fire, so on Windows today a
tap in a game shoots. With the hint off, a finger is only a finger.
The pen hint is the mirror image: SDL also reports a pen as a finger;
off on computers so a pen is only a mouse. Kept on for Android, where
a stylus should tap like a finger.

## 2. Finger events reach `touch_input.c` (`platform_pump_events`)

The four `SDL_EVENT_FINGER_*` cases lose their `#ifdef HALO_ANDROID`.
On the desktop, two lines run first:

- **Touchpads are skipped:** `SDL_GetTouchDeviceType(...) != SDL_TOUCH_DEVICE_DIRECT`.
  A MacBook or Linux touchpad also sends finger events, but its fingers
  already move the mouse. Only a touchscreen's go to the game.
- **A finger down calls `pointer_from_touch()`** (step 3).

Both are desktop-only because the 32-bit Android build reaches SDL only
through a fixed import list (`port/android/host_imports.list`), which
doesn't have `SDL_GetTouchDeviceType`. Android has no touchpads anyway.

`touch_input_cancel()` on focus loss also loses its guard, so a finger
held when you alt-tab can't click later.

## 3. Who drives the menus' pointer (`pointer_touch`)

New flag, under `input_lock` like the rest of the pointer state:

- `pointer_from_touch()`: a finger went down. Sets the flag, hides the
  arrow if a menu is up, and calls `platform_input_touch_used()` (step 5).
- `pointer_from_mouse()`: called on a mouse click or wheel turn in the
  menus. Clears the flag and shows the arrow.

**Why clicks and wheel, not motion:** opening a menu warps the mouse to
the center, which SDL reports as motion. If motion cleared the flag,
every menu open would hand the pointer back to an absent mouse.

## 4. The menus read the right device

- `platform_ui_pointer_set_active` now calls `touch_input_menu_set_active`,
  as Android's version does. That resets the touch gesture when a menu
  opens or closes, so a finger that started in play can't click in the
  menu. It also skips showing the arrow if touch was last used.
- `platform_ui_pointer_read` returns `touch_input_menu_read(pointer)` while
  the flag is set, else the mouse's pointer. The mouse's counted clicks
  are cleared either way, so a stale click can't fire when you switch back.

Nothing downstream changes: `halo_ui_pointer_update` (`d3d8_gl.c`) already
takes `pointer->touch` and maps window coordinates to the menus.

## 5. Prompts show controller buttons (`xinput_sdl.c`)

`platform_input_touch_used()` sets the last-used device to the last pad
style (Xbox by default). So after a tap, the menus' key bar reads
"B = Back" as on Android, matching PR 2's on-screen buttons. A key or
mouse use flips it back to keyboard prompts, through the existing code.

## 6. Sizes and units (`touch_input.c`)

Two things that were right on a phone and wrong on a Surface:

- **Units.** `touch_menu` works in the drawable's pixels. The menus
  want *window* units. On a phone they're the same; on a Surface at
  200% the window is 1440×960 units over 2880×1920 pixels. Without the
  fix, a tap at the screen's middle would land at the bottom-right
  corner. `touch_input_menu_read` now scales by window ÷ drawable
  (1.0 on Android).
- **dp.** Gesture sizes (12 dp tap slop, 40 dp per scroll step) need
  pixels per dp. Android passes the density. On a computer it's now the
  window's display scale × 96/160 (a scale of 1 means 96 px per inch; a
  dp is 1/160 inch). Before, it fell back to "the screen is 360 dp
  tall", a phone guess that would make scrolling on a Surface about
  three times stiffer.

## Not in this PR

The in-game controls (stick, buttons, swipe to look) stay Android-only:
the overlay half of `touch_input.c` is still behind `HALO_ANDROID`. In a
game, a tap still skips a cinematic (already shared code); otherwise
it does nothing. That's PR 2.

## Checked

- The three `.c` files compile without errors or warnings with the
  build's own flags against SDL 3.4.16's headers (a check that was
  shown to catch an injected error).
- Not yet built or run: the sandbox can't fetch the Linux sysroot, and
  can't start Actions. The Windows, Linux, macOS and Android builds run
  on your fork.

## Test on the Surface (bare tablet)

1. Main menu: tap an item to select, tap again (or the A prompt) to enter.
2. Drag a long list (Settings, the map list) to scroll; check it isn't
   stiff or twitchy.
3. Tap "B = Back" in the key bar.
4. Start a level, check a tap doesn't fire. Tap during a skippable
   cinematic: it skips.
5. Skip on a bare tablet: pausing needs PR 2's Pause button. (With a
   keyboard to hand, Esc, then tap through the pause menu.)
6. If a pen is handy: it should act as a mouse, and the arrow returns.

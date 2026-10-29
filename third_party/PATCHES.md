# Local changes to the third-party headers

Two headers here are changed from upstream. Each change is marked in the
source with `(c64-hw-sim patch)`. Re-apply them when updating either header.

## sokol/sokol_nuklear.h (mouse input handed to nuklear)

- **Button changes are queued.** Upstream kept one "down" flag and one "up"
  flag per button and cleared both when the mouse entered or left the
  window. A release lost that way left nuklear thinking the button was
  still held: windows stopped activating (that waits for the button to be
  up) and the next press was ignored. Now every change is queued, in
  order, with the mouse position at the time, and one change per button is
  handed over each frame. Entering or leaving the window drops nothing.
- **Losing the focus** (switching programs with a button held) releases
  the buttons.
- **Wheel notches are added up** within a frame, not overwritten.

## nuklear/nuklear.h

- **The wheel scrolls the window (or group, or drop-down list) under the
  mouse.** Upstream scrolled only the *active* window (the one last
  clicked), wherever the mouse was. Helper: `nk_window_under_mouse()`, just
  before `nk_panel_end()`.
- **The window under the mouse takes hover and wheel input** even when
  another window overlaps it somewhere else. Upstream kept such a window
  read only (`NK_WINDOW_ROM`) until it was clicked. Change at the end of
  the "window overlapping" block in `nk_begin_titled()`.
- **`nk_clear()`: closing the active window no longer breaks the window
  list.** Upstream set `ctx->end` (the last window) to the closed window's
  predecessor, even when the closed window wasn't last. That cut the list
  short, and emptied it if the closed window was the first. Now the
  topmost window still shown becomes active, and the list is left alone.

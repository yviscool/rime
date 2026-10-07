# @rime/picker — Screen Color Picker

Cursor-following color readout. Same proven loop as spy (poll -> tooltip ->
hotkeys), proving the screen read path: `mouse.getPos` (cursor),
`screen.pixel` (`0xRRGGBB` under it), `ui.toolTip`, `input.hotkey`,
`clipboard.write`.

## Controls

- Move the mouse: live `#RRGGBB` + `R G B @(x,y)` follows.
- F2: freeze/resume; freezing copies `#RRGGBB` to the clipboard.
- Esc: quit (destroys the tooltip, closes the hotkeys, exits 0).

## Performance notes (measured vs AHK PixelGetColor)

- One `screen.pixel` costs ~17ms on a DWM-composited desktop — at parity
  with AHK (`500ms/30` on the same box). The move gate at a 50ms poll is
  what keeps an idle desktop at zero cost, not a faster capture.

## Run

```text
bun tools/ts-build.ts
rime_js_bundle --production build/ts/picker.js
```

Needs production capabilities (`ui.create`, `windows.hook.global`,
`screen.capture`, `windows.clipboard.write`).

## Verification

- `bun test apps/picker/src/model.test.ts` — formatter units (L2).
- Desktop smoke (L5, manual): tooltip delta +1 while running, F2 copies a
  `#RRGGBB` literal to the clipboard, Esc exits 0, clipboard restored.

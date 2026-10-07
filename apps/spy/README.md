# @rime/spy — Window Spy

Cursor-following window inspector. Proves the Runtime → API → SDK → App
chain on public API only: `mouse.getPos` (cursor + window/control under it),
`ui.toolTip` (readout at cursor+16), `input.hotkey` (F2/Esc),
`clipboard.write` (frozen copy).

## Controls

- Move the mouse: live readout follows (`Title [Class]`, `pid (exe) WxH @(x,y)`, `control: ClassNN [Class]`).
- F2: freeze/resume; freezing copies the readout to the clipboard.
- Esc: quit (destroys the tooltip, closes the hotkeys, exits 0).

## Performance notes (measured, `build/probe-rime.json` method)

- `getPos` ~0ms, `toolTip` ~0ms, `Window.list` ~1ms per call on this box.
- Hotkeys are `input.hotkey` registrations: polling `getKeyState` missed
  quick taps in testing, events do not.
- Pixel/tooltip work is gated on cursor movement at a 50ms poll, so an
  idle desktop costs one `getPos` per tick.

## Run

```text
bun tools/ts-build.ts
rime_js_bundle --production build/ts/spy.js
```

Needs production capabilities (`ui.create`, `windows.hook.global`,
`windows.clipboard.write`); the demo grant is refused with
`capability_denied: ui.create`.

## Verification

- `bun test apps/spy/src/model.test.ts` — formatter units (L2).
- Desktop smoke (L5, manual): tooltip delta +1 while running, F2 copies a
  readout containing `[`/`pid` to the clipboard, Esc exits 0, clipboard
  restored. No custom windows are used (M6 five-verb GUI only); a clickable
  overlay waits on the Gui object model.

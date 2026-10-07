import { clipboard, input, mouse, runtime, screen, ui } from "@rime/sdk";
import { describePixel, hex } from "./model";

// Picker v3: a screen color picker that follows the cursor. Same shape as
// spy (poll -> tooltip -> hotkeys) so the two apps share one proven loop:
// mouse.getPos resolves the cursor, screen.pixel reads 0xRRGGBB under it,
// ui.toolTip renders at cursor+16, input.hotkey carries F2/Esc,
// clipboard.write copies the frozen hex.
//
// A single pixel read costs ~17ms on a DWM-composited desktop (measured at
// parity with AHK PixelGetColor), so the move gate is what keeps an idle
// desktop at zero cost; hotkeys stay event registrations because polling
// misses quick taps (measured, not assumed).
//
// Controls: F2 freezes/resumes (freezing copies `#RRGGBB`), Esc quits and
// destroys the tooltip. Fails the run when this main rejects.
const POLL_MS = 50;

async function main(): Promise<void> {
  let frozen = false;
  let lastHex = "";
  let lastText = "";
  let lastX = -1;
  let lastY = -1;
  let done = false;

  const freezeKey = input.hotkey("F2", () => {
    frozen = !frozen;
    if (frozen && lastHex !== "") {
      clipboard.write(lastHex).catch(() => undefined);
    }
  });
  const quitKey = input.hotkey("Esc", () => {
    done = true;
  });

  try {
    while (!done) {
      const pos = await mouse.getPos();
      const moved = pos.x !== lastX || pos.y !== lastY;
      if (!frozen && moved) {
        const color = await screen.pixel(pos.x, pos.y);
        lastHex = hex(color);
        lastText = describePixel(pos.x, pos.y, color, false);
        lastX = pos.x;
        lastY = pos.y;
        await ui.toolTip(lastText);
      } else if (frozen && moved) {
        lastX = pos.x;
        lastY = pos.y;
        await ui.toolTip(lastText);
      }
      await runtime.delay(POLL_MS, 0);
    }
  } finally {
    freezeKey.close();
    quitKey.close();
    await ui.toolTip("");
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure = String(error);
});

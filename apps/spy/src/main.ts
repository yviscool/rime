import { clipboard, input, mouse, runtime, ui } from "@rime/sdk";
import { describeProbe, type SpyProbe } from "./model";

// Spy v3: a Window-Spy readout that follows the cursor. Every call below
// crosses SDK -> native module -> Win32 service through public Runtime API
// only (no raw HWND ever enters JS): mouse.getPos resolves the cursor plus
// the window/control under it, ui.toolTip renders at cursor+16,
// input.hotkey carries F2/Esc, clipboard.write copies the frozen readout.
//
// Two measured lessons shape this loop (probe data, not guesses):
// hotkeys are event registrations (getKeyState polling misses quick taps),
// and pixel/tooltip work is skipped while the cursor sits still, so an
// idle desktop costs one getPos per 50ms.
//
// Controls: F2 freezes/resumes (freezing copies the readout to the
// clipboard), Esc quits and destroys the tooltip. The host settles
// afterwards and fails the run when this main rejects.
const POLL_MS = 50;

async function main(): Promise<void> {
  let frozen = false;
  let lastText = "";
  let lastX = -1;
  let lastY = -1;
  let done = false;

  const freezeKey = input.hotkey("F2", () => {
    frozen = !frozen;
    if (frozen && lastText !== "") {
      clipboard.write(lastText).catch(() => undefined);
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
        const probe: SpyProbe = {
          x: pos.x,
          y: pos.y,
          window: pos.window,
          control: pos.control,
        };
        lastText = describeProbe(probe, false);
        lastX = pos.x;
        lastY = pos.y;
        await ui.toolTip(lastText);
      } else if (frozen && moved) {
        // The readout stays frozen but the tip follows the cursor.
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

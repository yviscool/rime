/**
 * Production Showdown #9: System Tray Controller with Dynamic Menu & Tooltips
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Tray menus require esoteric directives (`Menu, Tray, Add, Item, Label`),
 *   global label dispatches with shared state, fragile DLL icon extraction
 *   (`Menu, Tray, Icon, shell32.dll, 42`), and lack of scoped menu lifecycles.
 * - Rime SDK: Structured `TrayController` utilizing `ui.traySetIcon()`, `ui.trayTip()`,
 *   typed hierarchical `Menu.create()` instances with closures, checked state toggles,
 *   and non-blocking notification balloons.
 */

import { Menu, input, ui, runtime } from "@rime/sdk";
import type { EventSubscription } from "@rime/sdk";

export interface TrayMenuItem {
  name: string;
  action?: () => void | Promise<void>;
  checked?: boolean;
  disabled?: boolean;
}

export class TrayController {
  private menu: Menu | null = null;
  private hotkeySub: EventSubscription | null = null;
  private currentIconPath = "*";
  private items: TrayMenuItem[] = [];

  /**
   * Sets or updates the system tray icon image.
   * Passing "" or "*" restores the default application icon.
   */
  async setIcon(iconPath = "*", iconNumber = 0): Promise<void> {
    this.currentIconPath = iconPath;
    try {
      await ui.traySetIcon(iconPath, { iconNumber });
    } catch (err) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[TrayController] Note: Tray icon update bypassed: ${err}`);
      }
    }
  }

  /**
   * Displays a balloon / toast notification anchored to the system tray.
   */
  async notify(title: string, message: string, icon: "none" | "info" | "warning" | "error" = "info"): Promise<void> {
    try {
      await ui.trayTip(message, {
        title,
        icon,
        mute: true,
      });
    } catch (err) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[TrayController] Note: Tray notification bypassed: ${err}`);
      }
    }
  }

  /**
   * Builds and attaches a dynamic Menu with hierarchical callbacks and toggles.
   */
  async buildMenu(items: TrayMenuItem[]): Promise<Menu> {
    if (this.menu) {
      try {
        await this.menu.destroy();
      } catch {}
    }

    const menu = await Menu.create();
    this.items = items;

    for (const item of items) {
      if (item.name === "-") {
        // Separator
        await menu.add();
        continue;
      }

      await menu.add(item.name, async () => {
        if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
          runtime.debug(`[TrayController] Menu item selected: "${item.name}"`);
        }
        if (item.action) {
          await item.action();
        }
      });

      if (item.checked) {
        try {
          await menu.check(item.name);
        } catch {}
      }

      if (item.disabled) {
        try {
          await menu.disable(item.name);
        } catch {}
      }
    }

    this.menu = menu;
    return menu;
  }

  /**
   * Binds a global hotkey shortcut to display tray status or toggle tray menu.
   */
  bindShortcut(chord = "ctrl+shift+t"): void {
    if (this.hotkeySub) {
      this.hotkeySub.close();
    }

    try {
      this.hotkeySub = input.hotkey(chord, () => {
        this.notify("Rime Tray Controller", `Active Items: ${this.items.length}`).catch(() => {});
      });
    } catch {}
  }

  /**
   * Releases tray resources and destroys active menus.
   */
  async dispose(): Promise<void> {
    if (this.hotkeySub) {
      try {
        this.hotkeySub.close();
      } catch {}
      this.hotkeySub = null;
    }

    if (this.menu) {
      try {
        await this.menu.destroy();
      } catch {}
      this.menu = null;
    }

    // Clear active trayTip
    try {
      await ui.trayTip("");
    } catch {}
  }
}

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 09: System Tray Controller ===");
  }

  const controller = new TrayController();

  try {
    // 1. Configure tray icon
    await controller.setIcon("*");
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[Tray] Initialized default system tray icon");
    }

    // 2. Build dynamic tray context menu
    let stealthMode = false;
    const dynamicItems: TrayMenuItem[] = [
      {
        name: "Dashboard",
        action: () => {
          runtime.debug?.("[Tray] Opening dashboard...");
        },
      },
      {
        name: "Stealth Mode",
        checked: false,
        action: () => {
          stealthMode = !stealthMode;
          runtime.debug?.(`[Tray] Toggled stealth mode: ${stealthMode}`);
        },
      },
      { name: "-" }, // Separator
      {
        name: "Exit Supervisor",
        action: () => {
          runtime.debug?.("[Tray] Exit requested");
        },
      },
    ];

    const menu = await controller.buildMenu(dynamicItems);
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Tray] Constructed dynamic menu with ${dynamicItems.length} items`);
    }

    // 3. Dispatch a balloon notification
    await controller.notify("Rime Supervisor", "Background tray services initialized and ready.", "info");

    // 4. Bind management hotkey
    controller.bindShortcut("ctrl+shift+t");
  } finally {
    await controller.dispose();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[Tray] Successfully disposed tray controller resources");
    }
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Tray Controller failed:", error);
  }
});

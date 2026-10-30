/**
 * Production Showdown #6: Audio Device & Sound Automation
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Uses `SoundSetVolume`, `SoundGetVolume`, `SoundSetMute`, and `SoundPlay`.
 *   Confusing string-convention syntax (`SoundSetMute, "-1"` for toggle),
 *   lack of async completions (SoundPlay can block script execution if wait=1),
 *   and unpredictable behavior when multiple audio endpoints are present.
 * - Rime SDK: First-class modern audio API (`sound.getVolume()`, `sound.setVolume()`,
 *   `sound.getMute()`, `sound.setMute()`, `sound.beep()`, `sound.play()`).
 *   Pure typed async Promises executed off-thread on worker slices with zero thread pinning.
 */

import { sound, runtime } from "@rime/sdk";

/**
 * Convenience accessor & mutator for master volume.
 * If value is provided (e.g. 50 or "+5" or -5), updates volume before returning current reading.
 */
export async function volume(value?: number | string): Promise<number> {
  if (value !== undefined) {
    await sound.setVolume(value);
  }
  return sound.getVolume();
}

/**
 * Convenience accessor & mutator for mute state.
 * Supports explicit boolean or "toggle".
 */
export async function mute(value?: boolean | "toggle"): Promise<boolean> {
  if (value === "toggle") {
    const current = await sound.getMute();
    await sound.setMute(!current);
  } else if (value !== undefined) {
    await sound.setMute(value);
  }
  return sound.getMute();
}

export interface AudioDeviceSnapshot {
  deviceName: string;
  volume: number;
  muted: boolean;
}

/**
 * Reads a comprehensive snapshot of the active default audio endpoint.
 */
export async function getAudioSnapshot(): Promise<AudioDeviceSnapshot> {
  let deviceName = "Default Endpoint";
  try {
    deviceName = await sound.getName();
  } catch {
    // Some headless/virtual systems may lack friendly names
  }

  const vol = await sound.getVolume();
  const isMuted = await sound.getMute();

  return {
    deviceName,
    volume: vol,
    muted: isMuted,
  };
}

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 06: Audio Switcher & Sound Automation ===");
  }

  try {
    // 1. Inspect current audio state
    const snap = await getAudioSnapshot();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Audio] Device: "${snap.deviceName}", Master Volume: ${snap.volume.toFixed(1)}%, Muted: ${snap.muted}`);
    }

    // 2. Safely test volume adjustment and restore
    const initialVol = snap.volume;
    const testDelta = initialVol > 10 ? -2 : 2;
    await volume(testDelta);
    const adjustedVol = await volume();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Audio] Adjusted volume to: ${adjustedVol.toFixed(1)}%`);
    }

    // Restore original volume
    await volume(initialVol);
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Audio] Restored volume to: ${initialVol.toFixed(1)}%`);
    }

    // 3. Play a lightweight non-blocking system beep/sound
    try {
      await sound.beep(880, 50);
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug("[Audio] System beep executed successfully");
      }
    } catch {
      // Audio hardware or service may be virtualized in CI/headless
    }
  } catch (error) {
    // If running in an environment with no audio device, log gracefully
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Audio] Note: Audio endpoint unavailable in this environment: ${error}`);
    }
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Audio Switcher failed:", error);
  }
});

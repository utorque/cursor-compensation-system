# rust_educational_no_recoil — Script Analysis

This document is a technical analysis of `script.lua`, a Logitech G-Hub / Logitech Gaming Software (LGS) Lua macro script.
**This analysis is provided for educational purposes only.**
Using this type of script in online multiplayer games violates their Terms of Service and anti-cheat policies (e.g. EAC, BattlEye).

---

## What platform does this run on?

The script uses the **Logitech Lua scripting API**, which is sandboxed inside Logitech's mouse driver software (G-Hub or LGS). It has access to a small set of driver-provided functions:

| API call | Purpose |
|---|---|
| `MoveMouseRelative(x, y)` | Moves the physical cursor by (x, y) pixels |
| `IsMouseButtonPressed(n)` | Returns `true` if mouse button `n` is held |
| `EnablePrimaryMouseButtonEvents(bool)` | Enables/disables left-click event forwarding to the OS |
| `IsModifierPressed("lctrl")` | Checks keyboard modifier state |
| `PressKey / ReleaseKey / PressAndReleaseKey` | Synthesises keyboard input |
| `PressMouseButton / ReleaseMouseButton` | Synthesises mouse button input |
| `GetRunningTime()` | Returns ms since script started (used for timing) |
| `OutputLogMessage(str)` | Writes to the driver's debug log |
| `OnEvent(event, arg)` | Entry point: called by the driver on hardware events |

---

## High-level structure

The script is split into five distinct sections.

### 1. Configuration block (lines 38–149)
Every supported weapon has a pair of variables:
- `<GUN>_<SLOT>` — the mouse button number (4 or 5) to bind this weapon to, or `nil` to leave it unbound.
- `<GUN>_<SLOT>_<ATTACHMENT>` — boolean flags controlling which in-game attachments are equipped (holosight, x8/x16 scopes, handmade sight, silencer, muzzle boost).

Two global settings are also here:
- `SENSITIVITY` — the player's in-game sensitivity (default 0.4).
- `FOV` — the player's field of view (default 78).

Supported weapons (13 total, each bindable to two independent slots = 26 possible bindings):
`AK47`, `LR300`, `MP5A4`, `Thompson`, `SMG`, `HMLMG`, `M249`, `SAR`, `M39`, `SAP`, `M92`, `Python`, `Revolver`

---

### 2. Recoil data tables (lines 158–221)
For every weapon the script stores three pre-recorded arrays:
- `<GUN>_OFFSET_X` — horizontal mouse movement to apply per bullet (in abstract units).
- `<GUN>_OFFSET_Y` — vertical mouse movement to apply per bullet (always negative = pull downward to cancel upward kick).
- `<GUN>_RPM` — the weapon's fire rate in rounds-per-minute, used to derive inter-shot timing.
- `<GUN>_BULLETS` — the length of the offset arrays (= magazine size or the number of bullets for which data exists).

The Y-offsets are large negative numbers (e.g. AK47: `-1.35` per bullet), meaning the script continuously pulls the mouse downward to counteract the weapon's upward climb.

---

### 3. Attachment multiplier calculation (lines 228–776)
This section converts the raw offsets into final pixel-delta values that account for:

- **Scope zoom** — each optic has a multiplier that scales how much real pixel movement is needed per in-game recoil unit:
  - No scope: ×1.0
  - Holosight: ×1.2 – ×1.7 (varies by weapon)
  - 8× scope: ×6.75 – ×9.75
  - 16× scope: ×13.5 – ×15.5
  - Handmade sight: ×0.8 – ×0.9
- **Barrel** — silencer and muzzle boost slightly alter timing or movement (usually ×0.9 for muzzle boost, ×1.0 otherwise).
- **Screen multiplier formula** (line 223):
  ```lua
  screenMultiplier = -0.03 * (SENSITIVITY * 3) * (FOV / 100)
  ```
  This maps the abstract recoil units to actual screen pixels for the player's specific sensitivity and FOV settings.

The resulting per-bullet X/Y pixel deltas and timing values are stored in precomputed arrays (e.g. `N1_AK47_C_X`, `N1_AK47_C_Y`, `N1_AK47_AT`, `N1_AK47_ST`) before `OnEvent` is ever called, so no floating-point work happens during firing.

---

### 4. Main event loop — `OnEvent` (lines 1256–2490)

This is the core runtime logic. The driver calls `OnEvent(event, arg)` every time a mouse button is pressed or released.

#### Toggle mechanism
When a configured side-button (4 or 5) is **pressed**, a boolean flag `kickback` is flipped (`kickback = not kickback`). This acts as an on/off toggle for the compensation on that weapon slot. A log message (`<GUN>_MACRO-ON / OFF`) is emitted to the driver log.

#### Firing loop (per weapon)
Once `kickback == true` for a given weapon, the script waits until **both** right-click (aim-down-sights, button 3) and left-click (fire, button 1) are held simultaneously. Then it:

1. Waits ~5 ms for the first bullet to register.
2. Iterates through the precomputed bullet offset arrays in a `for` loop — one iteration per bullet.
3. For each bullet:
   - Calls `Smoothing(steps, dx, dy)` (see below) to move the mouse.
   - Waits for the inter-shot delay (sleep time = `ST` value).
   - Checks if either button was released — exits immediately if so.
4. After the magazine is exhausted it enters a `repeat...until IsLeftNotPressed()` loop that keeps re-applying the last bullet's offset (holding pattern while the trigger is still held).

**Crouching modifier**: If `lctrl` is held, the script applies ADS (scoped) offsets directly without the `StandMultiplier` (1.89×). This simulates the reduced recoil while crouching or prone.

#### The `Smoothing` function (line 157)
```lua
function Smoothing(steps, dx, dy)
    x_ = 0; y_ = 0; t_ = 0
    for d = 1, steps do
        xI = round(d * dx / steps)
        yI = round(d * dy / steps)
        tI = d * steps / steps  -- always equals d
        MoveMouseRelative(round(xI - x_), round(yI - y_))
        sasd2441(tI - t_)
        x_ = xI; y_ = yI; t_ = tI
    end
end
```
This linearly interpolates the total (dx, dy) movement across `steps` sub-steps, calling `MoveMouseRelative` repeatedly. The effect is a smooth, continuous cursor drag that mimics a human hand pulling down, rather than a single instantaneous jump. The `steps` parameter comes from `AT` (attack time in ms), so more ms = more sub-steps = smoother motion.

#### Semi-auto rapidfire (`PressKey("pause")`)
For pistols and semi-automatic weapons (M92, Python, Revolver, M39, SAP, SAR), the script also sends a **Pause/Break key press** before each shot. The user is instructed (in the header comment) to bind both `Pause/Break` and `Mouse0` to Primary Attack in the game's controls menu. This causes the game to register one click per key press, effectively converting a held left-click into a rapid-fire stream of individual shots at the weapon's maximum RPM.

---

### 5. Door unlocker (lines 2491–2523)
A secondary feature unrelated to recoil. When a configured mouse button (`door_unlocker`) is pressed:
1. Presses `E` (interact key) and waits 250 ms.
2. Moves the cursor 50px diagonally (to aim at a code lock panel).
3. Clicks once, then releases `E`.
4. Types the four digits of a pre-configured `key_code` (0–9999) with 40 ms delays between keystrokes.

This automates entering a door code combination lock in the game.

---

## Code quality observations

| Issue | Location | Notes |
|---|---|---|
| **Massive code duplication** | Entire file | All 26 weapon slots share identical logic copy-pasted. A table-driven approach (array of weapon configs + one generic loop) would reduce ~2500 lines to ~200. |
| **Obfuscated helper name** | Line 155 | `sasd2441` is a sleep/busy-wait function. The name is intentionally meaningless, a minor obfuscation technique. |
| **Busy-wait sleep** | Line 155 | `repeat until GetRunningTime() > b-1` spins the CPU instead of yielding. This is the only option available in the Logitech Lua environment, which has no `sleep()`. |
| **Bug: wrong barrel reference** | Line 1039 | `N2_ST_AK47 = AK47_RPM*barrel_1_AK47_2 - N2_AT_AK47` uses `barrel_1` instead of `barrel_2`. The slot-2 muzzle boost timing is computed from the slot-1 barrel multiplier. |
| **Digit-to-string conversion** | Lines 2496–2499 | `if n == 0 then n = "0" elseif ...` repeated four times with nine conditions each. A simple `tostring(n)` or `string.format` call would replace all four blocks. |
| **Global variable pollution** | Throughout | Most variables lack `local`, making them implicit globals in Lua. In a larger script this would cause hard-to-debug state leakage between calls. |
| **`falseB` typo** | Line 1258 | `local kickback = falseB` — `falseB` is undefined (evaluates to `nil`, which is falsy, so the bug is silent). Should be `false`. |
| **Slot-2 THOMPSON muzzle boost copy-paste error** | Line 602 | `barrel_1_THOMPSON_2` is set instead of `barrel_2_THOMPSON_2` in the slot-2 attachment block. |

---

## Summary

The script is a **per-bullet, pre-computed mouse compensation macro**. It works by:
1. Pre-calculating exact pixel deltas for every shot of every weapon, adjusted for the player's sensitivity, FOV, and equipped attachments.
2. Waiting for the player to right-click (ADS) and left-click (fire) simultaneously.
3. Replaying those deltas in real time, one per shot, with precise inter-shot timing derived from each weapon's RPM.
4. Exiting immediately the moment either button is released.

The result is that the crosshair stays almost perfectly still during full-auto fire — something physically impossible for a human player without assist.

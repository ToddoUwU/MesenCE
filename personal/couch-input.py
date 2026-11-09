#!/usr/bin/env python3
"""couch-input.py SETTINGS_JSON: set up a portable Mesen folder for a one-player NES game played from the couch.

Run it with Mesen closed (Mesen rewrites settings.json when it exits), after Mesen has run once so the file holds the
full default configuration. Safe to run again: it only rewrites the entries below. A backup is kept beside the file.

  NES port 1  Mapping1 = controller Pad1 (Mesen's Xbox layout: NES A = B, NES B = A, Select = View, Start = Menu,
              d-pad, Turbo A = Y, Turbo B = X), Mapping2 = the same layout on Pad2, Mapping3 = the keyboard layout Mesen
              made by default. Moonlight can report two controllers (Sunshine makes a virtual pad for each), so either
              one plays player 1.
  Exit        View(Back) + Menu(Start) on Pad1 or Pad2 closes Mesen the normal way (battery save written), which
              returns to Quiver.

Pad key codes follow Mesen's Linux key manager: 0x1000 + 0x100 * (pad - 1) + button index (Select 10, Start 11).
"""
import json, shutil, sys, time

PAD_BASE, PAD_STRIDE, SELECT, START = 0x1000, 0x100, 10, 11


def pad_code(pad, button):
    return PAD_BASE + PAD_STRIDE * (pad - 1) + button


def on_pad(code, pad):
    lo = PAD_BASE + PAD_STRIDE * (pad - 1)
    return lo <= code < lo + PAD_STRIDE


def main(path):
    with open(path, encoding="utf-8-sig") as f:
        cfg = json.load(f)

    port = cfg["Nes"]["Port1"]
    pad1 = port["Mapping1"]
    if pad1.get("Select") != pad_code(1, SELECT) or pad1.get("A") != pad_code(1, 1):
        sys.exit(f"Mapping1 is not Mesen's Pad1 Xbox layout (A={pad1.get('A')}, Select={pad1.get('Select')}): "
                 "is this the patched build? Nothing changed.")
    keyboard = port["Mapping2"]
    if any(isinstance(v, int) and v >= PAD_BASE for v in keyboard.values()):
        keyboard = port.get("Mapping3", keyboard)  # already converted on an earlier run
    pad2 = {k: (v + PAD_STRIDE if isinstance(v, int) and on_pad(v, 1) else v) for k, v in pad1.items()}
    port["Mapping2"], port["Mapping3"] = pad2, keyboard

    shortcuts = cfg["Preferences"]["ShortcutKeys"]
    shortcuts[:] = [s for s in shortcuts if s.get("Shortcut") != "Exit"]
    shortcuts.append({
        "Shortcut": "Exit",
        "KeyCombination": {"Key1": pad_code(1, SELECT), "Key2": pad_code(1, START), "Key3": 0},
        "KeyCombination2": {"Key1": pad_code(2, SELECT), "Key2": pad_code(2, START), "Key3": 0},
    })

    backup = f"{path}.bak-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(path, backup)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    print(f"updated {path} (backup {backup})")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])

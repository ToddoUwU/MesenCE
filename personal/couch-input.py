#!/usr/bin/env python3
"""couch-input.py SETTINGS_JSON [--window X,Y,W,H]: set up a portable Mesen folder for a one-player NES game played from the couch.

Run it with Mesen closed (Mesen rewrites settings.json when it exits), after Mesen has run once so the file holds the
full default configuration. Safe to run again: it only rewrites the entries below. A backup is kept beside the file.

  NES port 1  Mapping1 = controller Pad1 (Mesen's Xbox layout: NES A = B, NES B = A, Select = View, Start = Menu,
              d-pad, Turbo A = Y, Turbo B = X), Mapping2 = the same layout on Pad2, Mapping3 = the keyboard layout Mesen
              made by default. Moonlight can report two controllers (Sunshine makes a virtual pad for each), so either
              one plays player 1.
  Exit        View(Back) + Menu(Start) on Pad1 or Pad2 closes Mesen the normal way (battery save written), which
              returns to Quiver.
  Background  AllowBackgroundInput on: Mesen otherwise ignores every controller (the Exit combo too) whenever its
              window is not focused, e.g. after a notification takes focus, which strands a couch player.

  Window      --window X,Y,W,H saves that window position (X11 coordinates), so Mesen opens, and goes fullscreen, on
              the monitor that is streamed. Without it Mesen opens at 0,0: the left monitor here (the LG, DP-2). The
              ASUS (DP-1, what Sunshine streams) starts at x=2560: --window 3200,104,1280,1232 centres it there.

Pad key codes follow Mesen's Linux key manager: 0x1000 + 0x100 * (pad - 1) + button index (Select 10, Start 11).
"""
import json, shutil, sys, time

PAD_BASE, PAD_STRIDE, SELECT, START = 0x1000, 0x100, 10, 11


def pad_code(pad, button):
    return PAD_BASE + PAD_STRIDE * (pad - 1) + button


def on_pad(code, pad):
    lo = PAD_BASE + PAD_STRIDE * (pad - 1)
    return lo <= code < lo + PAD_STRIDE


def main(path, window=None):
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

    cfg["Preferences"]["AllowBackgroundInput"] = True

    shortcuts = cfg["Preferences"]["ShortcutKeys"]
    shortcuts[:] = [s for s in shortcuts if s.get("Shortcut") != "Exit"]
    shortcuts.append({
        "Shortcut": "Exit",
        "KeyCombination": {"Key1": pad_code(1, SELECT), "Key2": pad_code(1, START), "Key3": 0},
        "KeyCombination2": {"Key1": pad_code(2, SELECT), "Key2": pad_code(2, START), "Key3": 0},
    })

    if window:
        x, y, w, h = (int(v) for v in window.split(","))
        cfg["MainWindow"].update({"WindowLocation": {"X": x, "Y": y}, "WindowSize": {"Width": w, "Height": h},
                                  "WindowIsMaximized": False})

    backup = f"{path}.bak-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(path, backup)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    print(f"updated {path} (backup {backup})")


if __name__ == "__main__":
    args = sys.argv[1:]
    window = None
    if len(args) == 3 and args[1] == "--window":
        window = args[2]
    elif len(args) != 1:
        sys.exit(__doc__)
    main(args[0], window)

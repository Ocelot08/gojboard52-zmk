"""Draw every layer of the gojboard52 keymap as a picture.

Run from the repository root:
    python scripts/draw_keymap.py

Reads  app/boards/shields/gojboard52/gojboard52.keymap
Writes build/main/keymap_layers.png

Only needs Python and the Pillow package (pip install pillow). Re-run it after you
edit the keymap to see the result before you build firmware.
"""

import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
KEYMAP = ROOT / "app/boards/shields/gojboard52/gojboard52.keymap"
OUT = ROOT / "build/main/keymap_layers.png"

# ---- how ZMK key names are shown on the keycaps ----
ALIAS = {
    **{f"N{i}": str(i) for i in range(10)},
    "SEMI": ";", "SQT": "'", "COMMA": ",", "DOT": ".", "FSLH": "/", "BSLH": "\\",
    "LBKT": "[", "RBKT": "]", "LBRC": "{", "RBRC": "}", "GRAVE": "`", "TILDE": "~",
    "EXCL": "!", "AT": "@", "HASH": "#", "DLLR": "$", "PRCNT": "%", "CARET": "^",
    "AMPS": "&", "ASTRK": "*", "LPAR": "(", "RPAR": ")", "UNDER": "_", "MINUS": "-",
    "EQUAL": "=", "PLUS": "+", "LSHFT": "Shift", "RSHFT": "Shift", "RSFT": "Shift",
    "LCTRL": "Ctrl", "RCTRL": "Ctrl", "LALT": "Alt", "RALT": "Alt", "LGUI": "Win",
    "RGUI": "Win", "RET": "Enter", "BSPC": "Bksp", "ESC": "Esc", "TAB": "Tab",
    "SPACE": "Space", "DOWN": "↓", "UP": "↑", "LEFT": "←", "RIGHT": "→",
    "C_VOL_DN": "Vol−", "C_VOL_UP": "Vol+", "C_MUTE": "Mute", "C_PREV": "Prev",
    "C_NEXT": "Next", "C_PP": "Play", "C_STOP": "Stop", "C_SLEEP_MODE": "Sleep",
    "PSCRN": "PrtSc", "PG_UP": "PgUp", "PG_DN": "PgDn", "DEL": "Del", "HOME": "Home",
    "END": "End", "KP_DIVIDE": "KP÷", "KP_MULTIPLY": "KP×", "KP_DOT": "KP.",
    "KP_ENTER": "KP Ent",
    **{f"KP_N{i}": f"KP{i}" for i in range(10)},
}
MODS = {"LC": "Ctrl", "RC": "Ctrl", "LS": "Shift", "RS": "Shift", "LA": "Alt",
        "RA": "Alt", "LG": "Win", "RG": "Win"}
LAYER_SHORT = {"SYMBOL": "Symbols", "FNNUM": "Fn / Num", "SYSTEM": "System", "DEFAULT": "Base"}

# colours per kind of key (light theme)
COLORS = {
    "key": ((250, 250, 252), (30, 30, 34)),
    "mod": ((226, 229, 236), (30, 30, 34)),
    "layer": ((255, 226, 196), (120, 60, 0)),
    "bt": ((205, 224, 255), (20, 60, 140)),
    "media": ((208, 240, 218), (16, 90, 50)),
    "trans": ((238, 238, 240), (170, 170, 175)),
    "none": ((222, 222, 226), (222, 222, 226)),
    "system": ((255, 214, 214), (140, 30, 30)),
}


def name_of(token):
    """ZMK keycode (possibly wrapped like RC(RS(ESC))) -> short readable text."""
    m = re.fullmatch(r"([A-Z]{2})\((.+)\)", token)
    if m and m.group(1) in MODS:
        return MODS[m.group(1)] + "+" + name_of(m.group(2))
    return ALIAS.get(token, token)


def describe(binding):
    """One binding like 'kp A' or 'bt BT_SEL 1' -> (label, sublabel, kind)."""
    parts = binding.split()
    if not parts:
        return "", "", "none"
    b, args = parts[0], parts[1:]
    if b == "kp" and args:
        label = name_of(args[0])
        kind = "media" if args[0].startswith("C_") else "mod" if label in (
            "Shift", "Ctrl", "Alt", "Win") else "key"
        return label, "", kind
    if b == "trans":
        return "trans", "", "trans"
    if b == "none":
        return "", "", "none"
    if b == "mo" and args:
        return "hold", LAYER_SHORT.get(args[0], args[0]), "layer"
    if b == "to" and args:
        return "to", LAYER_SHORT.get(args[0], args[0]), "layer"
    if b == "tog" and args:
        return "toggle", LAYER_SHORT.get(args[0], args[0]), "layer"
    if b == "bt":
        if args and args[0] == "BT_SEL" and len(args) > 1:
            return f"BT{int(args[1]) + 1}", "select", "bt"
        if args and args[0] == "BT_CLR":
            return "BT", "clear", "bt"
        return "BT", " ".join(args), "bt"
    if b == "bootloader":
        return "Boot", "loader", "system"
    return b, " ".join(args), "key"


def parse_layers(text):
    layers = []
    for m in re.finditer(r"(\w+)\s*\{\s*(?:label\s*=\s*\"([^\"]*)\";)?", text):
        pass
    # layer nodes look like:  name_layer { label = "..."; bindings = < ... >; ... };
    for m in re.finditer(r"(\w+_layer)\s*\{(.*?)\n\t\t\};", text, re.S):
        node, body = m.group(1), m.group(2)
        label = re.search(r'label\s*=\s*"([^"]*)"', body)
        bind = re.search(r"(?<![-\w])bindings\s*=\s*<(.*?)>;", body, re.S)
        sensor = [ln for ln in body.splitlines()
                  if "sensor-bindings" in ln and not ln.strip().startswith("//")]
        rows = []
        for line in bind.group(1).splitlines():
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            rows.append([describe(b.strip()) for b in line.split("&")[1:]])
        layers.append({
            "node": node,
            "name": (label.group(1).strip() if label else node),
            "rows": rows,
            "sensor": sensor[0].split("=", 1)[1].strip(" <>;") if sensor else None,
        })
    return layers


def font(size, bold=False):
    for name in ("segoeuib.ttf" if bold else "segoeui.ttf", "arialbd.ttf" if bold else "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def draw(layers, out):
    KW, KH, GAP, SPLIT, PAD, TITLE = 84, 74, 8, 70, 30, 60
    width = PAD * 2 + 12 * KW + 10 * GAP + SPLIT
    layer_h = TITLE + 5 * (KH + GAP) + 44
    img = Image.new("RGB", (width, PAD + len(layers) * layer_h), (255, 255, 255))
    d = ImageDraw.Draw(img)
    f_title, f_note = font(26, True), font(17)
    f_main, f_small, f_tiny = font(25, True), font(17, True), font(14)

    for li, layer in enumerate(layers):
        y0 = PAD // 2 + li * layer_h
        d.text((PAD, y0 + 6), f"Layer {li}: {layer['name']}", font=f_title, fill=(25, 25, 30))
        d.text((PAD + 360, y0 + 14), f"({layer['node']})", font=f_note, fill=(140, 140, 150))
        for r, row in enumerate(layer["rows"]):
            # the top row has 4 keys on the left half and 2 on the right half
            cols = [0, 1, 2, 3, 10, 11] if len(row) == 6 else list(range(len(row)))
            for (label, sub, kind), c in zip(row, cols):
                x = PAD + c * (KW + GAP) + (SPLIT if c >= 6 else 0)
                y = y0 + TITLE + r * (KH + GAP)
                fill, ink = COLORS[kind]
                d.rounded_rectangle([x, y, x + KW, y + KH], 10, fill=fill,
                                    outline=(185, 185, 195) if kind != "none" else None)
                if not label and not sub:
                    continue
                if len(label) > 9 and "+" in label:
                    # long shortcut such as Ctrl+Shift+Win+O: split over two lines
                    parts = label.split("+")
                    half = (len(parts) + 1) // 2
                    lines = ["+".join(parts[:half]) + "+", "+".join(parts[half:])]
                    for i, ln in enumerate(lines):
                        w = d.textlength(ln, font=f_tiny)
                        d.text((x + (KW - w) / 2, y + 16 + i * 20), ln, font=f_tiny, fill=ink)
                    continue
                size_font = f_main if len(label) <= 3 else f_small if len(label) <= 8 else f_tiny
                if kind == "trans":
                    size_font = f_tiny
                w = d.textlength(label, font=size_font)
                ty = y + (KH - 26) // 2 - (9 if sub else 0)
                d.text((x + (KW - w) / 2, ty), label, font=size_font, fill=ink)
                if sub:
                    w2 = d.textlength(sub, font=f_tiny)
                    d.text((x + (KW - w2) / 2, y + KH - 26), sub, font=f_tiny, fill=ink)
        note = ("Touch bar: " + layer["sensor"]) if layer["sensor"] else (
            "Touch bar: not overridden here (falls back to the layer below)")
        d.text((PAD, y0 + TITLE + 5 * (KH + GAP) + 4), note, font=f_note, fill=(90, 90, 100))
        d.text((PAD + 6 * (KW + GAP) - 24, y0 + 8), "left half", font=f_note, fill=(150, 150, 160))
        d.text((PAD + 6 * (KW + GAP) + SPLIT, y0 + 8), "right half", font=f_note, fill=(150, 150, 160))

    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)


if __name__ == "__main__":
    keymap = Path(sys.argv[1]) if len(sys.argv) > 1 else KEYMAP
    layers = parse_layers(keymap.read_text(encoding="utf-8"))
    draw(layers, OUT)
    print(f"{len(layers)} layers drawn -> {OUT}")

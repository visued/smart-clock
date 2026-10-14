#!/usr/bin/env python3
"""Preview 240x240 do firmware SmartClock AI — tema "PHOSPHOR CONSOLE".

Réplica fiel do layout do firmware (mesmas cores/posições) + easter egg de saldo.
Gera PNG estático ou GIF animado (--gif) mostrando os estados:
  SALDO (LED verde pulsando + cursor no medidor) / SEM SALDO (vermelho piscando)
  / SEM CHAVE (âmbar 1Hz).

Uso:
  python3 scripts/preview.py [--usage-url http://127.0.0.1:8787/usage]
                             [--ip 192.168.18.67] [--time 12:34:56]
                             [--out preview.png]
  python3 scripts/preview.py --gif --out design/animation.gif --frames 6
  python3 scripts/preview.py --demo --out design/mockup.png   # 3 estados de exemplo
  python3 scripts/preview.py --page solar --solar-url http://127.0.0.1:8787/solar
  python3 scripts/preview.py --page solar --demo --gif --out design/solar.gif
"""
import argparse
import json
import math
import os
import re
import urllib.request

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE = 240

# palette PHOSPHOR CONSOLE (RGB565 do designer -> RGB)
BG = (8, 8, 8)
SCANLINE = (0, 4, 0)
CARD = (32, 32, 32)          # 0x2104
MAX_CARDS, CARD_H, CARDS_Y, LOGO_SIZE = 4, 36, 93, 34   # = firmware
CARD_BORDER = (41, 93, 90)
DIVIDER = (49, 48, 49)
GRAY = (148, 149, 148)       # 0x94B2
BAR_EMPTY = (33, 8, 33)
PHOSPHOR = (190, 255, 107)
PHOSPHOR_DIM = (82, 117, 33)
CYAN = (0, 255, 255)
GREEN = (0, 255, 0)
GREEN_DIM = (0, 100, 0)
YELLOW = (255, 255, 0)
RED = (255, 0, 0)
RED_DIM = (160, 0, 0)
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
DS_BLUE = (77, 107, 254)
OC_AMBER = (253, 154, 0)

INVERT_OVERRIDES = {"claude": "white", "deepseek": "white", "ollama": "keep", "ollama_cloud": "keep", "zai": "white"}
BADGE_COLORS = {"claude": (217, 119, 87), "deepseek": DS_BLUE, "ollama": (255, 255, 255), "ollama_cloud": (255, 255, 255), "zai": BLACK}

# 7-segmento: segmentos a-g por digito
SEG = {
    "0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc",
    "5": "afgcd", "6": "afgedc", "7": "abc", "8": "abcdefg", "9": "abfgcd",
}


def usage_color(pct):
    if pct < 50:
        return GREEN
    if pct < 100:
        return YELLOW
    return RED


def state_of(p):
    st = p.get("state")
    if st in ("saldo", "sem_saldo", "sem_chave", "erro"):
        return st
    label = p.get("label") or ""
    pct = p.get("percent")
    if "sem chave" in label:
        return "sem_chave"
    if pct is None or pct < 0:
        return "erro"
    if pct >= 100:
        return "sem_saldo"
    return "saldo"


def load_font(size, bold=False):
    for c in (
        f"/usr/share/fonts/truetype/dejavu/DejaVuSans{'Bold' if bold else ''}.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        if os.path.exists(c):
            return ImageFont.truetype(c, size)
    return ImageFont.load_default()


def process_logo(path, size=36, pid=None):
    img = Image.open(path).convert("RGBA")
    img.thumbnail((size, size), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(img, ((size - img.width) // 2, (size - img.height) // 2), img)
    px = canvas.load()
    lum_sum = n = 0
    for yy in range(size):
        for xx in range(size):
            r, g, b, a = px[xx, yy]
            if a > 0 and r >= 225 and g >= 225 and b >= 225:
                px[xx, yy] = (0, 0, 0, 0)
            r, g, b, a = px[xx, yy]
            if a > 0:
                lum_sum += 0.299 * r + 0.587 * g + 0.114 * b
                n += 1
    invert = n > 0 and lum_sum / n < 110.0
    if pid in INVERT_OVERRIDES:
        mode = INVERT_OVERRIDES[pid]
        if mode == "white":
            for yy in range(size):
                for xx in range(size):
                    r, g, b, a = px[xx, yy]
                    if a > 0:
                        px[xx, yy] = (255, 255, 255, a)
            invert = False
        else:  # keep: cor original (ex.: llama preta sobre badge branco)
            invert = False
    if invert:
        r, g, b, a = canvas.split()
        canvas = Image.merge("RGBA", (r.point(lambda v: 255 - v),
                                     g.point(lambda v: 255 - v),
                                     b.point(lambda v: 255 - v), a))
    bg = BADGE_COLORS.get(pid, (32, 32, 32))
    out = Image.new("RGB", (size, size), bg)
    out.paste(canvas, (0, 0), canvas)
    return out


def fill_bg(d, x, y, w, h):
    """Fundo + scanlines (cada 4ª linha escura) — igual ao firmware."""
    d.rectangle((x, y, x + w - 1, y + h - 1), fill=BG)
    for yy in range(y + 3, y + h, 4):
        d.line((x, yy, x + w - 1, yy), fill=SCANLINE)


def draw_7seg(d, x, y, digit, color, h=28, w=18, th=3, ins=2):
    segs = {
        "a": (x + ins, y, w - 2 * ins, th),
        "b": (x + w - th, y + ins, th, h // 2 - ins),
        "c": (x + w - th, y + h // 2, th, h // 2 - ins),
        "d": (x + ins, y + h - th, w - 2 * ins, th),
        "e": (x, y + h // 2, th, h // 2 - ins),
        "f": (x, y + ins, th, h // 2 - ins),
        "g": (x + ins, y + h // 2 - th // 2, w - 2 * ins, th),
    }
    for s in SEG.get(str(digit), ""):
        sx, sy, sw, sh = segs[s]
        d.rectangle((sx, sy, sx + sw - 1, sy + sh - 1), fill=color)


def draw_clock(d, hhmm, x_center, y, frame, h=28, w=18, th=3):
    """Relógio 7-seg compacto — mesma geometria do firmware (SEG_P/SEG_C)."""
    colon_on = frame % 2 == 0
    digits = [int(c) for c in hhmm if c.isdigit()]
    if len(digits) != 4:
        return
    pitch, cgap = w + 4, 6
    x0 = x_center - (3 * pitch + cgap + w) // 2
    for i, dig in enumerate(digits):
        x = x0 + i * pitch + (cgap if i >= 2 else 0)
        draw_7seg(d, x + 1, y + 1, dig, PHOSPHOR_DIM, h, w, th)  # sombra
        draw_7seg(d, x, y, dig, PHOSPHOR, h, w, th)
    if colon_on:
        cx = x0 + pitch + w + (pitch - w + cgap) // 2
        for cy in (y + 9, y + 19):
            d.ellipse((cx - 2, cy - 2, cx + 2, cy + 2), fill=PHOSPHOR)


def draw_weather_icon(d, code, x, y):
    c = 0
    if len(code) >= 2:
        try:
            c = int(code[:2])
        except ValueError:
            c = 0
    if c == 1:
        d.ellipse((x + 4, y + 3, x + 16, y + 15), fill=YELLOW)
        for i in range(8):
            a = i * math.pi / 4
            d.line((x + 10, y + 9, x + 10 + 9 * math.cos(a), y + 9 + 9 * math.sin(a)),
                   fill=YELLOW, width=2)
    else:
        d.ellipse((x, y + 4, x + 12, y + 16), fill=GRAY)
        d.ellipse((x + 7, y + 2, x + 19, y + 14), fill=GRAY)
        d.ellipse((x + 15, y + 6, x + 23, y + 14), fill=GRAY)
        d.rectangle((x - 1, y + 10, x + 20, y + 15), fill=GRAY)
        if c in (9, 10):
            for dx in (4, 9, 14):
                d.line((x + dx, y + 16, x + dx - 2, y + 19), fill=CYAN, width=2)
        elif c == 11:
            d.polygon([(x + 5, y + 16), (x + 11, y + 16), (x + 8, y + 20)], fill=YELLOW)
            d.polygon([(x + 10, y + 16), (x + 15, y + 16), (x + 13, y + 20)], fill=YELLOW)
        elif c == 13:
            for dx in (4, 9, 14):
                d.ellipse((x + dx, y + 16, x + dx + 2, y + 18), fill=WHITE)


def draw_glyph(d, x, y, kind, color):
    if kind == "coin":
        d.ellipse((x, y, x + 7, y + 7), outline=color, width=1)
        d.ellipse((x + 2, y + 2, x + 5, y + 5), fill=color)
    elif kind == "warn":
        d.polygon([(x + 4, y), (x + 8, y + 7), (x, y + 7)], outline=color)
        d.line((x + 4, y + 2, x + 4, y + 5), fill=color)
        d.point((x + 4, y + 6), fill=color)
    elif kind == "key":
        d.ellipse((x + 2, y, x + 7, y + 5), outline=color)
        d.rectangle((x, y + 4, x + 7, y + 8), fill=color)
        d.rectangle((x + 4, y + 4, x + 5, y + 7), fill=BG)
    else:  # erro: X
        d.line((x, y, x + 7, y + 7), fill=color)
        d.line((x + 7, y, x, y + 7), fill=color)


STATE_META = {
    "saldo":     ("SALDO", GREEN, GREEN_DIM, 900, 300),
    "sem_saldo": ("ESGOTADO", RED, RED_DIM, 250, 250),
    "sem_chave": ("SEM CHAVE", YELLOW, YELLOW, 500, 500),
    "erro":      ("ERRO", YELLOW, YELLOW, 250, 250),
}


def draw_card(d, img, prov, y, frame, fword, fdate, fbig, fsmall):
    st = state_of(prov)
    word, color, dim, on_ms, off_ms = STATE_META.get(st, STATE_META["erro"])
    pct = prov.get("percent")

    period = (on_ms // 250) + (off_ms // 250)
    led_on = (frame % period) < (on_ms // 250)
    border = RED if st == "sem_saldo" else CARD_BORDER

    d.rounded_rectangle((8, y, 231, y + CARD_H - 1), radius=6, fill=CARD, outline=border)
    d.rectangle((8, y + 8, 10, y + 27), fill=color)

    # logo sobre badge com a cor da marca
    lpath = os.path.join(ROOT, "assets", "logos", prov["id"] + ".png")
    if os.path.exists(lpath):
        badge = BADGE_COLORS.get(prov["id"], (32, 32, 32))
        d.rounded_rectangle((12, y + 1, 12 + LOGO_SIZE - 1, y + LOGO_SIZE), radius=8, fill=badge)
        logo = process_logo(lpath, size=LOGO_SIZE, pid=prov["id"])
        img.paste(logo, (12, y + 1))
    else:
        letters = {"claude": "CL", "deepseek": "DS", "ollama_cloud": "OC"}.get(prov["id"], "AI")[:2]
        d.rounded_rectangle((12, y + 4, 40, y + 32), radius=7, fill=DS_BLUE)
        d.text((26, y + 18), letters, font=fword, fill=BLACK, anchor="mm")

    # linhas de informação (limites primeiro, senão estado/saldo)
    limits = prov.get("limits") or []
    rows = []
    if limits:
        for l in limits[:2]:
            lb = str(l.get("label", "?"))[:2]
            lp = l.get("percent")
            row = lb + (f" {lp:.0f}%" if lp is not None else "")
            rs = str(l.get("reset", ""))[:8]
            if rs:
                row += " " + rs
            rows.append((row[:13], usage_color(lp) if lp is not None else GRAY))
        while len(rows) < 2:
            rows.append(("-", GRAY))
    else:
        is_key = st in ("sem_chave", "erro")
        if is_key:
            rows = [(word[:13], color), ((prov.get("label") or "")[:13], GRAY)]
        else:
            r1 = (prov.get("label") or word)[:13]
            c1 = usage_color(pct) if pct is not None and pct >= 0 else color
            r2 = (prov.get("reset") or "")[:13]
            if not r2 and pct is not None and pct >= 0:
                r2 = f"usado {pct:.0f}%"
            rows = [(r1, c1), (r2, GRAY)]

    for k, (txt, c) in enumerate(rows):
        d.text((52, y + 2 + k * 17), txt, font=fword, fill=c, anchor="la")


    # LED de status (anel + dot)
    d.ellipse((219, y + 18, 231, y + 30), outline=(19, 19, 19))
    if led_on:
        d.ellipse((222, y + 21, 228, y + 27), fill=color)
    else:
        d.ellipse((222, y + 21, 228, y + 27), fill=dim)


def render(data, ip, hhmm, ss, frame, out, demo=False):
    if demo:
        providers = [
            {"id": "deepseek", "percent": 40.0, "label": "saldo 18.00 CNY", "reset": "", "state": "saldo", "limits": []},
            {"id": "ollama_cloud", "percent": 100.0, "label": "manual · 100/100", "reset": "reseta: 10/08 00:00", "state": "sem_saldo",
             "limits": [{"label": "H", "percent": 45.0, "reset": "em 3h"}, {"label": "W", "percent": 100.0, "reset": "2d"}]},
            {"id": "qwen", "percent": 0.0, "label": "sem chave de API", "reset": "", "state": "sem_chave", "limits": []},
            {"id": "claude", "percent": 41.0, "label": "plano max", "reset": "", "state": "saldo",
             "limits": [{"label": "5h", "percent": 8.0, "reset": "05:40"}, {"label": "1w", "percent": 41.0, "reset": "15/09"}]},
        ]
    else:
        providers = data["providers"]
    img = Image.new("RGB", (SIZE, SIZE), BG)
    d = ImageDraw.Draw(img)
    fill_bg(d, 0, 0, 240, 240)

    fdate = load_font(16)
    fsmall = load_font(8)
    fword = load_font(8, bold=True)
    fbig = load_font(32, bold=True)

    # topo: data (curta) + IP + divisor + sublinhado fosforo
    d.text((6, 3), "07/08", font=fdate, fill=WHITE, anchor="la")
    d.text((234, 3), ip, font=fdate, fill=GRAY, anchor="ra")
    d.line((0, 21, 239, 21), fill=DIVIDER)
    d.line((6, 21, 50, 21), fill=PHOSPHOR)

    # clima
    w = data.get("weather") or {}
    if w.get("ok"):
        d.text((6, 24), ">", font=fdate, fill=PHOSPHOR, anchor="la")
        d.text((16, 24), f"{w['temp']:.0f}C {w['condition']}", font=fdate, fill=WHITE, anchor="la")
        draw_weather_icon(d, w.get("icon", ""), 214, 22)

    # relogio 7-seg fosforo com sombra + dois-pontos piscando + segundos
    draw_clock(d, hhmm, 120, 44, frame)
    d.text((210, 50), ss, font=fdate, fill=CYAN, anchor="la")

    # status: LED + texto (última atualização + intervalo)
    led = GREEN if frame % 4 < 3 else GREEN_DIM
    d.ellipse((8, 80, 14, 86), fill=led)
    d.text((20, 76), f"{hhmm}:{ss} (60s)", font=fdate, fill=led, anchor="la")
    d.line((0, 92, 239, 92), fill=DIVIDER)

    # cards (1..MAX_CARDS, centralizados na região 93..239)
    shown = providers[:MAX_CARDS]
    n = len(shown)
    gap = 1 if n >= MAX_CARDS else 4
    total = n * CARD_H + (n - 1) * gap
    start = CARDS_Y + (240 - CARDS_Y - total) // 2
    for i, prov in enumerate(shown):
        draw_card(d, img, prov, start + i * (CARD_H + gap), frame, fword, fdate, fbig, fsmall)

    img.save(out)
    print(f"ok: {out} ({n} cards, frame {frame})")


# ---------------- tela 2: SOLAR (fósforo âmbar) — espelha drawSolarPage() do firmware ----------------
AMBER = (255, 176, 0)        # 0xFD80
AMBER_DIM = (128, 80, 0)     # 0x8280
AMBER_DARK = (40, 24, 0)     # 0x28C0
SOLAR_SEGS = 25              # barra de potência: 25 segmentos de 7px + 2px

_GLCD = None


def _glcd():
    """Fonte GLCD 5x7 do TFT_eSPI (a mesma do relógio) — preview pixel a pixel."""
    global _GLCD
    if _GLCD is None:
        src = os.path.join(ROOT, "firmware", ".pio", "libdeps", "esp12e", "TFT_eSPI", "Fonts", "glcdfont.c")
        body = open(src, encoding="latin-1").read().split("{", 1)[1].split("}", 1)[0]
        _GLCD = [int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body)]
    return _GLCD


def gtext(d, x, y, text, color, size=2, datum="TL"):
    """drawString com a fonte GLCD (6x8 por char * size). datum: TL | TR | TC."""
    font = _glcd()
    w = len(text) * 6 * size
    if datum == "TR":
        x -= w
    elif datum == "TC":
        x -= w // 2
    for ci, ch in enumerate(text):
        o = ord(ch) * 5
        for col in range(5):
            bits = font[o + col]
            for row in range(8):
                if bits >> row & 1:
                    px = x + ci * 6 * size + col * size
                    py = y + row * size
                    d.rectangle((px, py, px + size - 1, py + size - 1), fill=color)
    return w


def draw_7seg_big(d, x, y, ch, color):
    """Dígito 7-seg grande (H=44 W=26 T=5) — mesma geometria de draw7segBig() no firmware."""
    if ch == ".":
        d.rectangle((x + 1, y + 44 - 6, x + 6, y + 43), fill=color)
        return
    if ch == "-":
        d.rectangle((x + 3, y + 22 - 2, x + 22, y + 22 + 2), fill=color)
        return
    draw_7seg(d, x, y, ch, color, h=44, w=26, th=5, ins=3)


def draw_sun(d, x, y, frame, on):
    c = AMBER if on else GRAY
    if not on:  # lua
        d.ellipse((x + 3, y + 2, x + 17, y + 16), fill=GRAY)
        d.ellipse((x + 8, y, x + 20, y + 12), fill=BG)
        return
    d.ellipse((x + 5, y + 4, x + 15, y + 14), fill=c)
    off = math.pi / 8 if frame % 4 >= 2 else 0     # raios "giram" a cada 500 ms
    for i in range(8):
        a = i * math.pi / 4 + off
        d.line((x + 10 + 7 * math.cos(a), y + 9 + 7 * math.sin(a),
                x + 10 + 10 * math.cos(a), y + 9 + 10 * math.sin(a)), fill=c, width=2)


def solar_power_text(sd):
    if not sd.get("ok"):
        return "-.--", "kW"
    w = sd.get("w", 0) if sd.get("online") else 0
    if w >= 1000:
        return f"{w / 1000:.2f}", "kW"
    return f"{w:d}", "W"


def render_solar(sd, date, hhmm, frame, out):
    img = Image.new("RGB", (SIZE, SIZE), BG)
    d = ImageDraw.Draw(img)
    fill_bg(d, 0, 0, 240, 240)
    online = sd.get("online", False)
    w = sd.get("w", 0) if online else 0
    hour = int(hhmm.split(":")[0])

    # topo: data | pontos do carrossel | hora
    gtext(d, 6, 3, date, WHITE)
    for i, cx in enumerate((114, 126)):
        if i == 1:
            d.ellipse((cx - 3, 8, cx + 3, 14), fill=AMBER)
        else:
            d.ellipse((cx - 3, 8, cx + 3, 14), outline=GRAY)
    gtext(d, 234, 3, hhmm, CYAN, datum="TR")
    d.line((0, 21, 239, 21), fill=DIVIDER)
    d.line((6, 21, 50, 21), fill=AMBER)

    # linha de título: > SOLAR ............ (LED) ESTADO  sol/lua
    gtext(d, 6, 24, ">", AMBER)
    gtext(d, 18, 24, "SOLAR", WHITE)
    if online and w > 0:
        word, col, dim = "GERANDO", GREEN, GREEN_DIM
    elif online:
        word, col, dim = "OCIOSO", GRAY, GRAY
    elif hour >= 18 or hour < 6:
        word, col, dim = "NOITE", GRAY, GRAY
    else:
        word, col, dim = "OFFLINE", RED, RED_DIM
    wx = 206 - gtext(d, 206, 24, word, col, datum="TR")
    led = col if frame % 4 < 3 else dim
    d.ellipse((wx - 11, 29, wx - 5, 35), fill=led)
    draw_sun(d, 212, 22, frame, online and w > 0)

    # herói: potência em 7-seg âmbar com sombra + unidade
    txt, unit = solar_power_text(sd)
    pitch, dpw = 32, 10
    width = sum(dpw if c == "." else pitch for c in txt) - (pitch - 26) + 6 + 24
    x = 120 - width // 2
    num_col = AMBER if online else AMBER_DIM
    for c in txt:
        draw_7seg_big(d, x + 1, 47, c, AMBER_DARK)
        draw_7seg_big(d, x, 46, c, num_col)
        x += dpw if c == "." else pitch
    x -= pitch - 26
    gtext(d, x + 6, 48, "AGORA", GRAY, size=1)
    gtext(d, x + 6, 74, unit, num_col)

    # barra de potência (% da nominal), ponta pisca
    pct = sd.get("pct", 0) if online else 0
    lit = round(pct * SOLAR_SEGS / 100)
    for i in range(SOLAR_SEGS):
        sx = 8 + i * 9
        if i < lit:
            c = YELLOW if (i == lit - 1 and frame % 2 == 0) else AMBER
        else:
            c = AMBER_DARK
        d.rectangle((sx, 97, sx + 6, 106), fill=c)
    gtext(d, 8, 110, f"{pct}%", AMBER, size=1)
    gtext(d, 232, 110, f"nominal {sd.get('rated', 5000) / 1000:.1f}kW", GRAY, size=1, datum="TR")

    # tiles: HOJE / TOTAL
    def tile(x0, label, val, unit, accent):
        d.rounded_rectangle((x0, 121, x0 + 107, 155), radius=6, fill=CARD, outline=CARD_BORDER)
        d.rectangle((x0, 129, x0 + 2, 147), fill=accent)
        gtext(d, x0 + 10, 126, label, GRAY, size=1)
        vw = gtext(d, x0 + 10, 136, val, WHITE)
        gtext(d, x0 + 14 + vw, 143, unit, accent, size=1)

    today = sd.get("today")
    total = sd.get("total")
    tile(8, "HOJE", f"{today:.1f}" if today is not None else "--", "kWh", AMBER)
    if total is None:
        tile(124, "TOTAL", "--", "kWh", CYAN)
    elif total >= 1000:
        tile(124, "TOTAL", f"{total / 1000:.2f}", "MWh", CYAN)
    else:
        tile(124, "TOTAL", f"{total:.0f}", "kWh", CYAN)

    # curva do dia (05h..19h, fatias de 20 min)
    d.rounded_rectangle((8, 159, 231, 216), radius=6, fill=CARD, outline=CARD_BORDER)
    gtext(d, 15, 164, "CURVA HOJE", GRAY, size=1)
    if sd.get("peak_w"):
        gtext(d, 225, 164, f"pico {sd['peak_w'] / 1000:.1f}kW {sd.get('peak_t', '')}", AMBER_DIM, size=1, datum="TR")
    hist = sd.get("hist") or []
    slot0, smin = sd.get("slot0", 300), sd.get("slot_min", 20)
    h_, m_ = (int(v) for v in hhmm.split(":"))
    cur = (h_ * 60 + m_ - slot0) // smin
    base = 203
    d.line((15, base + 1, 224, base + 1), fill=DIVIDER)
    for i, v in enumerate(hist):
        bx = 15 + i * 5
        if v is None or v < 0:
            continue
        bh = max(1, round(v * 28 / 100))
        if i == cur and online:
            c_fill = c_cap = YELLOW if frame % 2 == 0 else AMBER
        else:
            c_fill, c_cap = AMBER_DIM, AMBER
        d.rectangle((bx, base - bh + 1, bx + 3, base), fill=c_fill)
        d.rectangle((bx, base - bh + 1, bx + 3, base - bh + 2), fill=c_cap)
    for label, minute in (("5h", 300), ("12h", 720), ("19h", 1140)):
        lx = 15 + (minute - slot0) // smin * 5
        gtext(d, min(max(lx, 15), 225), 207, label, GRAY, size=1,
              datum="TL" if minute == 300 else ("TR" if minute == 1140 else "TC"))

    # rodapé: strings PV + rede, ou há quanto tempo o inversor está offline
    if not online:
        age = sd.get("age", -1)
        if not sd.get("ok"):
            msg = f"sem leitura: {sd.get('err', '')}"
        elif age < 3600:
            msg = f"inversor offline ha {age // 60}min"
        else:
            msg = f"inversor offline ha {age // 3600}h{age % 3600 // 60:02d}"
        gtext(d, 8, 226, msg[:38], GRAY, size=1)
        img.save(out)
        print(f"ok: {out} (solar, frame {frame})")
        return
    pv = sd.get("pv") or []
    for k, row in enumerate(pv[:2]):
        y = 221 + k * 10
        v, a, pw = row
        xx = 8 + gtext(d, 8, y, f"PV{k + 1}", AMBER, size=1)
        gtext(d, xx + 6, y, f"{v:.0f}V {a:.1f}A {pw / 1000:.2f}kW", WHITE if online else GRAY, size=1)
    if sd.get("vac") is not None:
        gtext(d, 232, 221, f"{sd['vac']:.0f}V {sd['hz']:.1f}Hz", GRAY, size=1, datum="TR")
        gtext(d, 232, 231, f"inv {sd['temp']:.0f}C", GRAY, size=1, datum="TR")

    img.save(out)
    print(f"ok: {out} (solar, frame {frame})")


def solar_demo():
    """Dia ensolarado sintético até 11:40 — curva em sino com nuvens."""
    hist = []
    for i in range(42):
        t = (300 + i * 20) / 60
        if t > 11.67:
            hist.append(-1)
            continue
        v = max(0.0, math.sin(math.pi * (t - 5.6) / 12.8)) * 92
        if i in (14, 15):
            v *= 0.55
        hist.append(round(v))
    return {"ok": True, "online": True, "age": 5, "rated": 5000, "peak_w": 4310, "peak_t": "11:20",
            "slot0": 300, "slot_min": 20, "hist": hist, "w": 4131, "today": 11.9, "total": 14050.1,
            "vac": 229.2, "hz": 60.03, "temp": 74.2,
            "pv": [[228.0, 12.4, 2827], [116.5, 12.7, 1480]], "pct": 83}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--usage-url", default="http://127.0.0.1:8787/usage")
    ap.add_argument("--ip", default="192.168.18.67")
    ap.add_argument("--time", default="12:34:56")
    ap.add_argument("--out", default="preview.png")
    ap.add_argument("--demo", action="store_true", help="usa 4 estados de exemplo")
    ap.add_argument("--gif", action="store_true", help="gera GIF animado")
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--frame", type=int, default=0, help="frame fixo para PNG estatico")
    ap.add_argument("--page", choices=("ai", "solar"), default="ai", help="tela do carrossel")
    ap.add_argument("--solar-url", default="http://127.0.0.1:8787/solar")
    ap.add_argument("--date", default="14/10")
    args = ap.parse_args()

    if args.page == "solar":
        if args.demo:
            sd = solar_demo()
        else:
            with urllib.request.urlopen(args.solar_url, timeout=10) as r:
                sd = json.loads(r.read().decode("utf-8"))
        hhmm = args.time[:5]
        if args.gif:
            frames = []
            for f in range(args.frames):
                tmp = f"/tmp/preview_solar_{f}.png"
                render_solar(sd, args.date, hhmm, f, tmp)
                frames.append(Image.open(tmp).convert("P", palette=Image.ADAPTIVE, colors=256))
            frames[0].save(args.out, save_all=True, append_images=frames[1:], duration=250, loop=0)
            print(f"ok: {args.out} (GIF solar, {args.frames} frames)")
        else:
            render_solar(sd, args.date, hhmm, args.frame, args.out)
        return

    data = {}
    if not args.demo:
        with urllib.request.urlopen(args.usage_url, timeout=10) as r:
            data = json.loads(r.read().decode("utf-8"))
    else:
        data = {"weather": {"temp": 30.8, "condition": "nublado", "icon": "04d", "ok": True}}

    h, m, s = args.time.split(":")
    hhmm, ss = f"{h}:{m}", s

    if args.gif:
        frames = []
        for f in range(args.frames):
            tmp = f"/tmp/preview_frame_{f}.png"
            render(data, args.ip, hhmm, ss, f, tmp, demo=args.demo)
            frames.append(Image.open(tmp).convert("P", palette=Image.ADAPTIVE, colors=256))
        frames[0].save(args.out, save_all=True, append_images=frames[1:],
                       duration=280, loop=0)
        print(f"ok: {args.out} (GIF, {args.frames} frames)")
    else:
        render(data, args.ip, hhmm, ss, args.frame, args.out, demo=args.demo)


if __name__ == "__main__":
    main()

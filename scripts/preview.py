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
"""
import argparse
import json
import math
import os
import urllib.request

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE = 240

# palette PHOSPHOR CONSOLE (RGB565 do designer -> RGB)
BG = (8, 8, 8)
SCANLINE = (0, 4, 0)
CARD = (32, 32, 32)          # 0x2104
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

INVERT_OVERRIDES = {"deepseek": "white", "ollama": "keep", "ollama_cloud": "keep"}
BADGE_COLORS = {"deepseek": DS_BLUE, "ollama": (255, 255, 255), "ollama_cloud": (255, 255, 255)}

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


def draw_7seg(d, x, y, digit, color, h=48, w=28, th=4):
    ins = 3
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


def draw_clock(d, hhmm, x_center, y, frame, h=48, w=28, gap=0):
    colon_on = frame % 2 == 0
    digits = [int(c) for c in hhmm if c.isdigit()]
    if len(digits) != 4:
        return
    total = 4 * w + 10  # colon ocupa ~10px
    x0 = x_center - total // 2
    xs = [x0, x0 + w, x0 + 2 * w + 10, x0 + 3 * w + 10]
    for i, dig in enumerate(digits):
        draw_7seg(d, xs[i] + 1, y + 1, dig, PHOSPHOR_DIM)  # sombra
        draw_7seg(d, xs[i], y, dig, PHOSPHOR)
    if colon_on:
        cx = x0 + 2 * w + 4
        d.ellipse((cx, y + h // 2 - 10, cx + 6, y + h // 2 - 4), fill=PHOSPHOR)
        d.ellipse((cx, y + h // 2 + 4, cx + 6, y + h // 2 + 10), fill=PHOSPHOR)


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

    d.rounded_rectangle((8, y, 232, y + 38), radius=6, fill=CARD, outline=border)
    d.rectangle((8, y + 9, 11, y + 33), fill=color)

    # logo sobre badge com a cor da marca
    lpath = os.path.join(ROOT, "assets", "logos", prov["id"] + ".png")
    if os.path.exists(lpath):
        badge = BADGE_COLORS.get(prov["id"], (32, 32, 32))
        d.rounded_rectangle((12, y + 1, 48, y + 37), radius=8, fill=badge)
        logo = process_logo(lpath, pid=prov["id"])
        img.paste(logo, (12, y + 1))
    else:
        letters = {"deepseek": "DS", "ollama_cloud": "OC"}.get(prov["id"], "AI")[:2]
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
        d.text((52, y + 3 + k * 18), txt, font=fword, fill=c, anchor="la")

    # % global (pior limite) no canto superior direito — 16px, só quando há limites
    if limits and pct is not None and pct >= 0:
        d.text((232, y + 4), f"{pct:.0f}%", font=fdate, fill=usage_color(pct), anchor="ra")

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
    d.line((0, 23, 239, 23), fill=DIVIDER)
    d.line((6, 23, 50, 23), fill=PHOSPHOR)

    # clima
    w = data.get("weather") or {}
    if w.get("ok"):
        d.text((6, 27), ">", font=fdate, fill=PHOSPHOR, anchor="la")
        d.text((16, 27), f"{w['temp']:.0f}C {w['condition']}", font=fdate, fill=WHITE, anchor="la")
        draw_weather_icon(d, w.get("icon", ""), 214, 24)

    # relogio 7-seg fosforo com sombra + dois-pontos piscando + segundos
    draw_clock(d, hhmm, 120, 46, frame)
    d.text((210, 66), ss, font=fdate, fill=CYAN, anchor="la")

    # status: LED + texto (última atualização + intervalo)
    led = GREEN if frame % 4 < 3 else GREEN_DIM
    d.ellipse((8, 106, 14, 112), fill=led)
    d.text((20, 102), f"{hhmm}:{ss} (60s)", font=fdate, fill=led, anchor="la")
    d.line((0, 122, 239, 122), fill=DIVIDER)

    # cards (1..3, centralizados)
    n = min(len(providers), 3)
    gap = 1 if n >= 3 else 4
    total = n * 38 + (n - 1) * gap
    start = 124 + (116 - total) // 2
    for i, prov in enumerate(providers[:3]):
        draw_card(d, img, prov, start + i * (38 + gap), frame, fword, fdate, fbig, fsmall)

    img.save(out)
    print(f"ok: {out} ({len(providers[:3])} cards, frame {frame})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--usage-url", default="http://127.0.0.1:8787/usage")
    ap.add_argument("--ip", default="192.168.18.67")
    ap.add_argument("--time", default="12:34:56")
    ap.add_argument("--out", default="preview.png")
    ap.add_argument("--demo", action="store_true", help="usa 3 estados de exemplo")
    ap.add_argument("--gif", action="store_true", help="gera GIF animado")
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--frame", type=int, default=0, help="frame fixo para PNG estatico")
    args = ap.parse_args()

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

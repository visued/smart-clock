#!/usr/bin/env python3
"""Converte logos PNG em arrays RGB565 para o firmware (firmware/src/logos.h).

Uso:
  python3 scripts/logo2header.py [--size 32]

Lê assets/logos/<id>.png e gera firmware/src/logos.h com um logo por provedor.
O fundo transparente é composto sobre a cor do card do firmware (0x18C3).
"""
import argparse
import os
import sys

from PIL import Image

SIZE_DEFAULT = 36

# Modo por logo: "white" = logo branco puro (badge colorido), "keep" = cor original
LOGO_MODE = {
    "deepseek": "white",     # baleia branca sobre badge azul da marca
    "ollama": "keep",        # llama preta sobre badge branco
    "ollama_cloud": "keep",
    "zai": "white",          # "Z" branco sobre badge preto
}

# cor do badge por provedor (fundo do logo no card)
BADGE_COLORS = {
    "deepseek": 0x4B5F,     # azul DeepSeek
    "ollama": 0xFFFF,       # branco (llama preta)
    "ollama_cloud": 0xFFFF,
    "zai": 0x0000,          # preto (fundo do logo oficial: "Z" branco)
}


def rgb565_to_rgb(v):
    return ((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=SIZE_DEFAULT)
    args = ap.parse_args()
    size = args.size
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    logos_dir = os.path.join(root, "assets", "logos")
    out_path = os.path.join(root, "firmware", "src", "logos.h")

    entries = []
    for name in sorted(os.listdir(logos_dir)):
        if not name.endswith(".png"):
            continue
        pid = name[:-4]
        img = Image.open(os.path.join(logos_dir, name)).convert("RGBA")
        # contém dentro de size×size, centralizado
        img.thumbnail((size, size), Image.LANCZOS)
        canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        x = (size - img.width) // 2
        y = (size - img.height) // 2
        canvas.paste(img, (x, y), img)
        px = canvas.load()

        # chroma-key: fundo branco sólido (ex.: avatar do GitHub) -> transparente
        for yy in range(size):
            for xx in range(size):
                r, g, b, a = px[xx, yy]
                if a > 0 and r >= 225 and g >= 225 and b >= 225:
                    px[xx, yy] = (0, 0, 0, 0)

        # modo por logo: "white" (branco puro p/ badge colorido) | "keep"
        mode = LOGO_MODE.get(pid, "keep")
        if mode == "white":
            for yy in range(size):
                for xx in range(size):
                    r, g, b, a = px[xx, yy]
                    if a > 0:
                        px[xx, yy] = (255, 255, 255, a)

        # composição alfa sobre a cor do badge (opaco — encaixa sobre o badge no card)
        badge = BADGE_COLORS.get(pid, 0x2104)
        bg = rgb565_to_rgb(badge)
        data = []
        for yy in range(size):
            for xx in range(size):
                r, g, b, a = px[xx, yy]
                alpha = a / 255.0
                cr = int(r * alpha + bg[0] * (1 - alpha))
                cg = int(g * alpha + bg[1] * (1 - alpha))
                cb = int(b * alpha + bg[2] * (1 - alpha))
                data.append(rgb565(cr, cg, cb))

        rows = []
        for i in range(0, len(data), 12):
            rows.append("  " + ", ".join(f"0x{v:04X}" for v in data[i:i + 12]) + ",")
        entries.append(
            f"// logo {pid} ({img.width}x{img.height} -> {size}x{size})\n"
            f"static const uint16_t LOGO_{pid.upper()}[{size * size}] = {{\n"
            + "\n".join(rows)
            + f"\n}};\n"
        )

    if not entries:
        sys.exit("nenhum logo em assets/logos/")

    header = (
        "// Gerado por scripts/logo2header.py — não edite manualmente.\n"
        f"#define LOGO_SIZE {size}\n"
        "#define LOGO_COUNT " + str(len(entries)) + "\n\n"
        + "\n".join(entries)
        + "\nstruct LogoMeta { const char* id; const uint16_t* data; uint16_t badge; };\n"
        + "static const LogoMeta LOGOS[] = {\n"
    )
    for name in sorted(os.listdir(logos_dir)):
        if not name.endswith(".png"):
            continue
        pid = name[:-4]
        badge = BADGE_COLORS.get(pid, 0x2104)
        header += f'  {{"{pid}", LOGO_{pid.upper()}, 0x{badge:04X}}},\n'
    header += "  {nullptr, nullptr, 0}\n};\n"
    header += (
        "\nstatic const uint16_t* logoFor(const String& id) {\n"
        "  for (int i = 0; LOGOS[i].id; i++) if (id == LOGOS[i].id) return LOGOS[i].data;\n"
        "  return nullptr;\n"
        "}\n"
        "\nstatic uint16_t badgeFor(const String& id) {\n"
        "  for (int i = 0; LOGOS[i].id; i++) if (id == LOGOS[i].id) return LOGOS[i].badge;\n"
        "  return 0x2104;\n"
        "}\n"
    )

    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write(header)
    print(f"ok: {out_path} ({len(entries)} logos, {size}x{size})")


if __name__ == "__main__":
    main()

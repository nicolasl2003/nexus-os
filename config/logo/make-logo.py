#!/usr/bin/env python3
"""Génère le logo de démarrage de Nexus OS pour le noyau Linux.

Sortie :
  nexus_logo_clut224.ppm  format attendu par le noyau (PPM ASCII, <= 224 couleurs)
  nexus_logo_preview.png  aperçu agrandi, pour le regarder sur le Mac

Le dessin : un carré aux coins arrondis en dégradé cyan -> bleu, avec un
« N » blanc. Les bords sont lissés par sur-échantillonnage, puis les
couleurs sont quantifiées pour rester sous la limite de 224 couleurs.

Usage : python3 make-logo.py   (sans dépendance externe)
"""
import os
import struct
import zlib

SIZE = 96          # taille du logo en pixels
SS = 4             # sur-échantillonnage (SS x SS points par pixel)
LEVELS = 8         # niveaux de transparence des bords (anti-aliasing)
GRADIENT = 16      # nombre de teintes du dégradé

TOP = (0, 210, 235)      # cyan
BOTTOM = (40, 80, 225)   # bleu
WHITE = (255, 255, 255)
HERE = os.path.dirname(os.path.abspath(__file__))


def in_rounded_square(x, y, margin=3, radius=20):
    lo, hi = margin, SIZE - margin
    if not (lo <= x <= hi and lo <= y <= hi):
        return False
    cx = min(max(x, lo + radius), hi - radius)
    cy = min(max(y, lo + radius), hi - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def in_polygon(x, y, pts):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


# Le « N » : deux barres verticales et une diagonale
TOP_Y, BOT_Y = 24, 72
LEFT_BAR = [(26, TOP_Y), (38, TOP_Y), (38, BOT_Y), (26, BOT_Y)]
RIGHT_BAR = [(58, TOP_Y), (70, TOP_Y), (70, BOT_Y), (58, BOT_Y)]
DIAGONAL = [(26, TOP_Y), (39, TOP_Y), (70, BOT_Y), (57, BOT_Y)]


def in_n(x, y):
    return any(in_polygon(x, y, p) for p in (LEFT_BAR, RIGHT_BAR, DIAGONAL))


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render():
    pixels = []
    for py in range(SIZE):
        # Teinte du dégradé pour cette ligne, quantifiée
        g = round(py / (SIZE - 1) * (GRADIENT - 1)) / (GRADIENT - 1)
        base = mix(TOP, BOTTOM, g)
        row = []
        for px in range(SIZE):
            sq = n = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if in_rounded_square(x, y):
                        sq += 1
                        if in_n(x, y):
                            n += 1
            total = SS * SS
            a_sq = round(sq / total * LEVELS) / LEVELS
            a_n = round(n / total * LEVELS) / LEVELS
            color = mix((0, 0, 0), base, a_sq)      # carré sur fond noir
            color = mix(color, WHITE, a_n)          # « N » blanc par-dessus
            row.append(color)
        pixels.append(row)
    return pixels


def write_ppm(pixels, path):
    lines = ["P3", "# Logo Nexus OS (genere par make-logo.py)", f"{SIZE} {SIZE}", "255"]
    for row in pixels:
        lines.append(" ".join(f"{r} {g} {b}" for r, g, b in row))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def write_png(pixels, path, scale=4):
    raw = b""
    for row in pixels:
        line = b"".join(bytes(c) * scale for c in row)
        raw += (b"\x00" + line) * scale
    w = h = SIZE * scale

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    img = render()
    colors = {c for row in img for c in row}
    assert len(colors) <= 224, f"{len(colors)} couleurs : le noyau en accepte 224 au maximum"
    write_ppm(img, os.path.join(HERE, "nexus_logo_clut224.ppm"))
    write_png(img, os.path.join(HERE, "nexus_logo_preview.png"))
    print(f"Logo {SIZE}x{SIZE} généré ({len(colors)} couleurs)")

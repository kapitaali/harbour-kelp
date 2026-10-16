"""Generate the Kelp launcher icon (placeholder mark).

A rounded-square deep-sea gradient with three swaying kelp blades. Drawn at
512x512 and downsampled so the small sizes stay clean. Rerun to regenerate:

    python3 scripts/make-icons.py
"""

import math
import os

from PIL import Image, ImageDraw

S = 512
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def make_master():
    top, bottom = (10, 46, 58), (3, 18, 27)

    grad = Image.new("RGB", (1, S))
    for y in range(S):
        grad.putpixel((0, y), lerp(top, bottom, y / (S - 1)))
    grad = grad.resize((S, S))

    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1],
                                           radius=int(S * 0.22), fill=255)

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)

    blades = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(blades)

    # (base x, amplitude, phase, stroke width, colour, top)
    specs = [
        (0.32, 0.055, 0.0, 0.075, (150, 233, 178, 255), 0.16),
        (0.52, 0.075, 1.6, 0.090, (110, 214, 150, 255), 0.08),
        (0.70, 0.050, 3.1, 0.065, (86, 190, 128, 255), 0.22),
    ]

    for base_x, amp, phase, wfrac, col, top in specs:
        x0, amp = base_x * S, amp * S
        w = wfrac * S
        # Stamp overlapping discs along the path: a polyline leaves visible
        # notches at its joins, stamped discs come out as one smooth blade.
        n = 200
        for i in range(n):
            t = i / (n - 1)
            y = (0.96 - t * (0.96 - top)) * S
            sway = math.sin(2.1 * math.pi * t + phase) * (0.35 + 0.65 * t)
            x = x0 + amp * sway + 0.5 * amp * t * t
            r = (w / 2) * (1.0 - 0.35 * t)   # taper toward the tip
            d.ellipse([x - r, y - r, x + r, y + r], fill=col)

    img = Image.alpha_composite(img, blades)

    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


def main():
    master = make_master()
    master.save(os.path.join(ROOT, "docs", "icon-512.png"))
    for size in (86, 108, 128, 172, 256):
        dest = os.path.join(ROOT, "icons", f"{size}x{size}", "harbour-kelp.png")
        master.resize((size, size), Image.LANCZOS).save(dest)
        print("wrote", dest)


if __name__ == "__main__":
    main()

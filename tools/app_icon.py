#!/usr/bin/env python3
"""Draws tennessine's app icon and writes every file made from it.

The icon is element 117's outer electron shell: a nucleus and a tilted shell holding 7
electrons, with room for an 8th. oganesson's icon is element 118's, with the shell full.
The glyph is one flat shape, so the same drawing serves the colour icons, Android's themed
icon and a 1-bit icon.

Needs skia-pathops for the boolean operations. Run it in a container:

    podman run --rm -v "$PWD:/w:Z" -w /w docker.io/library/python:3.12-slim \\
        sh -c 'pip install -q skia-pathops==0.8.0.post2 && python tools/app_icon.py'

Writes:
  res/app_icon/*.svg              icon.sh renders the PNGs Chromium keeps from these
  res/drawable/themed_app_icon.xml  Android's themed (monochrome) icon
  src/chrome/browser/app_icon/android/java/res/drawable/
                                  the glyph and one adaptive icon per colour in Settings
  --og DIR                        oganesson's glyph, as og.svg, in DIR instead of the above
"""
import math, sys
from pathlib import Path
from pathops import Path as SkPath, PathOp, op

ROOT = Path(__file__).resolve().parent.parent

# The design, on a 512 grid centred on C. The shell is an ellipse tilted down to the right;
# t is the angle along it, and sin(t) > 0 is the half in front of the nucleus.
C = 256
SCALE = 0.84               # the whole drawing, about C, to sit inside the icon's safe zone
A, B, TILT = 212, 106, 28  # shell radii and tilt, degrees
NUCLEUS = 92
GAP = 14                   # space cut around a front part where it crosses the nucleus
LINE_FRONT, LINE_BACK = 16, 7.2
SLOTS = [-112.5 + 45 * i for i in range(8)]  # the 8th, on the back left, is the empty one


def electron_radius(depth):
    return 30 + 10 * depth


# Settings → App icon. The first is the default. Android draws an adaptive icon's background
# opaque, so there is no transparent one: a transparent background shows as black.
COLOURS = [
    ('indigo', '#2D2D6B', 'white'),
    ('teal', '#1F5C63', 'white'),
    ('crimson', '#7A1F3D', 'white'),
    ('black', '#000000', 'white'),
    ('white', '#FFFFFF', 'black'),
]


def shell_point(t):
    t, tilt = math.radians(t), math.radians(TILT)
    x, y = A * math.cos(t), B * math.sin(t)
    # outward normal of the ellipse before the tilt
    nx, ny = B * math.cos(t), A * math.sin(t)
    n = math.hypot(nx, ny)
    rot = lambda u, v: (u * math.cos(tilt) - v * math.sin(tilt), u * math.sin(tilt) + v * math.cos(tilt))
    (px, py), (qx, qy) = rot(x, y), rot(nx / n, ny / n)
    return C + px, C + py, qx, qy, math.sin(t)


def line_width(depth):
    """Thin behind the nucleus, thick in front, blending across the shell's ends."""
    k = min(1, max(0, (depth + 0.25) / 0.5))
    k = k * k * (3 - 2 * k)
    return LINE_BACK + (LINE_FRONT - LINE_BACK) * k


def polygon(points):
    path = SkPath()
    pen = path.getPen()
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()
    return path


def circle(x, y, r):
    k = 0.5522847498 * r
    path = SkPath()
    pen = path.getPen()
    pen.moveTo((x + r, y))
    pen.curveTo((x + r, y + k), (x + k, y + r), (x, y + r))
    pen.curveTo((x - k, y + r), (x - r, y + k), (x - r, y))
    pen.curveTo((x - r, y - k), (x - k, y - r), (x, y - r))
    pen.curveTo((x + k, y - r), (x + r, y - k), (x + r, y))
    pen.closePath()
    return path


def band(t0, t1, grow=0.0, steps=240):
    """The shell line from angle t0 to t1, widened by `grow` on each side."""
    outer, inner = [], []
    for i in range(steps + 1):
        x, y, nx, ny, depth = shell_point(t0 + (t1 - t0) * i / steps)
        h = line_width(depth) / 2 + grow
        outer.append((x + nx * h, y + ny * h))
        inner.append((x - nx * h, y - ny * h))
    return polygon(outer + inner[::-1])


def union(paths):
    out = paths[0]
    for p in paths[1:]:
        out = op(out, p, PathOp.UNION)
    return out


def glyph(electrons):
    """The icon as one path: `electrons` of the 8 slots filled."""
    dots = []
    for t in SLOTS[:electrons]:
        x, y, _, _, depth = shell_point(t)
        dots.append((x, y, electron_radius(depth), depth))
    front = [d for d in dots if d[3] >= 0]
    back = [d for d in dots if d[3] < 0]

    # Each half overlaps the other by a degree, so the union has no seam.
    front_line, back_line = band(-1, 181), band(179, 361)
    hidden = circle(C, C, NUCLEUS + GAP)
    behind = op(union([back_line] + [circle(x, y, r) for x, y, r, _ in back]), hidden, PathOp.DIFFERENCE)
    nucleus = circle(C, C, NUCLEUS)
    for cut in [band(-1, 181, GAP)] + [circle(x, y, r + GAP) for x, y, r, _ in front]:
        nucleus = op(nucleus, cut, PathOp.DIFFERENCE)
    return union([behind, nucleus, front_line] + [circle(x, y, r) for x, y, r, _ in front])


class SvgPen:
    """Collects a path as SVG path data, mapping each point through `f`."""

    def __init__(self, f):
        self.f, self.d = f, []

    def _p(self, *pts):
        return ' '.join('%s,%s' % tuple(fmt(c) for c in self.f(p)) for p in pts)

    def moveTo(self, p):
        self.d.append('M' + self._p(p))

    def lineTo(self, p):
        self.d.append('L' + self._p(p))

    def curveTo(self, *pts):
        self.d.append('C' + self._p(*pts))

    def qCurveTo(self, *pts):
        self.d.append('Q' + self._p(*pts))

    def closePath(self):
        self.d.append('Z')

    def endPath(self):
        pass


def fmt(v):
    s = '%.2f' % v
    return s.rstrip('0').rstrip('.') if '.' in s else s


def path_data(path, size, inset=0.0):
    """Path data in a size × size box; the 512 grid fills it minus `inset` on each side."""
    k = (size - 2 * inset) / 512

    def f(p):
        x, y = (C + (c - C) * SCALE for c in p)
        return inset + x * k, inset + y * k

    pen = SvgPen(f)
    path.draw(pen)
    return ''.join(pen.d)


def svg(size, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d">\n%s\n</svg>\n'
            % (size, size, body))


def vector(d, colour):
    """An adaptive icon layer: 108dp, of which the middle 72dp shows, holding the 512 grid."""
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- Generated by tools/app_icon.py. -->\n'
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            '    android:width="108dp"\n'
            '    android:height="108dp"\n'
            '    android:viewportWidth="108"\n'
            '    android:viewportHeight="108">\n'
            '    <path\n'
            '        android:fillColor="%s"\n'
            '        android:pathData="%s"/>\n'
            '</vector>\n' % (colour, d))


def adaptive(background, glyph_colour):
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- Generated by tools/app_icon.py. -->\n'
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background>\n'
            '        <shape><solid android:color="%s"/></shape>\n'
            '    </background>\n'
            '    <foreground android:drawable="@drawable/app_icon_glyph_%s"/>\n'
            '    <monochrome android:drawable="@drawable/themed_app_icon"/>\n'
            '</adaptive-icon>\n' % (background, glyph_colour))


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    print(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path)


def main():
    if sys.argv[1:2] == ['--og']:
        write(Path(sys.argv[2]).resolve() / 'og.svg', svg(512, '<path d="%s"/>' % path_data(glyph(8), 512)))
        return

    ts = glyph(7)
    _, default_bg, _ = COLOURS[0]
    res = ROOT / 'res/app_icon'
    d512, d108 = path_data(ts, 512), path_data(ts, 108, 18)
    write(res / 'glyph.svg', svg(512, '<path d="%s"/>' % d512))
    # Chromium's PNGs: the adaptive icon's layers, and the round icon older launchers show.
    write(res / 'layered_app_icon_background.svg', svg(108, '<rect width="108" height="108" fill="%s"/>' % default_bg))
    write(res / 'layered_app_icon.svg', svg(108, '<rect width="108" height="108" fill="%s"/>\n<path fill="#fff" d="%s"/>' % (default_bg, d108)))
    write(res / 'app_icon.svg', svg(512, '<circle cx="256" cy="256" r="240" fill="%s"/>\n<path fill="#fff" d="%s"/>'
                                    % (default_bg, path_data(ts, 512, 16))))

    write(ROOT / 'res/drawable/themed_app_icon.xml', vector(d108, '#000000'))
    drawables = ROOT / 'src/chrome/browser/app_icon/android/java/res/drawable'
    for colour in ('white', 'black'):
        write(drawables / ('app_icon_glyph_%s.xml' % colour), vector(d108, '#FFFFFF' if colour == 'white' else '#000000'))
    for name, background, glyph_colour in COLOURS:
        write(drawables / ('app_icon_%s.xml' % name), adaptive(background, glyph_colour))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Draws tennessine's app icon and writes every file made from it.

The icon is element 117's outer electron shell seen from above: a ring with 8 places, 7 of them
holding an electron. The 8th, at the top left, is empty, and the ring stops at the electrons on
either side of it. oganesson's icon is element 118's: all 8 places filled, the ring closed. A gap
is cut round each electron. The glyph is one flat shape, so the same drawing serves the colour
icons, Android's themed icon and the 1-bit icons.

Needs skia-pathops for the boolean operations. Run it in a container:

    podman run --rm -v "$PWD:/w:Z" -w /w docker.io/library/python:3.12-slim \\
        sh -c 'pip install -q skia-pathops==0.8.0.post2 && python tools/app_icon.py'

Writes:
  res/app_icon/*.svg              icon.sh renders the PNGs Chromium keeps from these
  res/drawable/themed_app_icon.xml  Android's themed (monochrome) icon
  src/chrome/browser/app_icon/android/java/res/drawable/
                                  the glyph, one adaptive icon per colour in Settings, and
                                  the white and black glyph alone (app_icon_clear.xml, app_icon_clearblack.xml)
  --og DIR                        oganesson's glyph, as og.svg, in DIR instead of the above
  --dubna DIR                     dubna's, as dubna.svg and dubna-icon.svg (on its plate), in DIR:
                                  a nucleus between two interlaced orbits, after the atom sign
                                  on Dubna's arms. Rendered to PNG, dubna-icon is the F-Droid
                                  repository's icon (see tennessine.py publish).
"""
import math, sys
from pathlib import Path
from pathops import Path as SkPath, PathOp, op

ROOT = Path(__file__).resolve().parent.parent

# The design, on a 512 grid centred on C.
C = 256
SCALE = 0.84      # the whole drawing, about C, to sit inside the icon's safe zone
RING = 174        # radius of the ring's centre line
LINE = 32         # the ring's width
ELECTRON = 42     # an electron's radius
GAP = 12          # space cut into the ring round each electron
# The 8 places, clockwise from the top, in degrees (y points down). The last is the top left.
PLACES = [-90 + 45 * i for i in range(8)]

# Settings → App icon: name, background, glyph colour. The first is the default. Android draws an
# adaptive icon's background opaque, a transparent one as black, so the icons with no background
# (app_icon_clear, app_icon_clearblack) are not adaptive: the glyph alone, which launchers that
# put every icon on a plate show on one.
COLOURS = [
    ('indigo', '#2D2D6B', 'white'),
    ('teal', '#1F5C63', 'white'),
    ('crimson', '#7A1F3D', 'white'),
    ('black', '#000000', 'white'),
    ('white', '#FFFFFF', 'black'),
]


def place(angle):
    return C + RING * math.cos(math.radians(angle)), C + RING * math.sin(math.radians(angle))


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


def arc(start, end, steps=180):
    """The ring from angle `start` clockwise to `end`."""
    outer, inner = [], []
    for i in range(steps + 1):
        a = math.radians(start + (end - start) * i / steps)
        outer.append((C + (RING + LINE / 2) * math.cos(a), C + (RING + LINE / 2) * math.sin(a)))
        inner.append((C + (RING - LINE / 2) * math.cos(a), C + (RING - LINE / 2) * math.sin(a)))
    return polygon(outer + inner[::-1])


# dubna: two orbits tilted either way, each passing over the other at alternate crossings.
DUBNA_ORBIT = (200, 96, 24)   # radii and tilt, degrees
DUBNA_LINE = 28
DUBNA_NUCLEUS = 54
DUBNA_PLATE = '#1E4E8C'       # the blue of Dubna's flag


def ellipse(rx, ry, tilt, steps=360):
    t = math.radians(tilt)
    pts = [(rx * math.cos(math.radians(360 * i / steps)), ry * math.sin(math.radians(360 * i / steps)))
           for i in range(steps)]
    return polygon([(C + x * math.cos(t) - y * math.sin(t), C + x * math.sin(t) + y * math.cos(t)) for x, y in pts])


def orbit(tilt, grow=0.0):
    rx, ry, _ = DUBNA_ORBIT
    h = DUBNA_LINE / 2 + grow
    return op(ellipse(rx + h, ry + h, tilt), ellipse(rx - h, ry - h, tilt), PathOp.DIFFERENCE)


def dubna():
    rx, ry, tilt = DUBNA_ORBIT
    a, b = orbit(-tilt), orbit(tilt)
    # The crossings lie on the axes: right and left, where `a` passes over, and top and bottom,
    # where `b` does. Under each crossing, the orbit below loses the other one widened by GAP.
    over_a = op(polygon([(C + 60, 0), (512, 0), (512, 512), (C + 60, 512)]),
                polygon([(0, 0), (C - 60, 0), (C - 60, 512), (0, 512)]), PathOp.UNION)
    over_b = op(polygon([(0, 0), (512, 0), (512, C - 30), (0, C - 30)]),
                polygon([(0, C + 30), (512, C + 30), (512, 512), (0, 512)]), PathOp.UNION)
    b = op(b, op(orbit(-tilt, GAP), over_a, PathOp.INTERSECTION), PathOp.DIFFERENCE)
    a = op(a, op(orbit(tilt, GAP), op(over_b, over_a, PathOp.DIFFERENCE), PathOp.INTERSECTION), PathOp.DIFFERENCE)
    links = op(op(a, b, PathOp.UNION), circle(C, C, DUBNA_NUCLEUS + GAP), PathOp.DIFFERENCE)
    return op(links, circle(C, C, DUBNA_NUCLEUS), PathOp.UNION)


def glyph(electrons):
    """The icon as one path: the first `electrons` of the 8 places filled."""
    filled = PLACES[:electrons]
    if electrons == len(PLACES):
        ring = op(circle(C, C, RING + LINE / 2), circle(C, C, RING - LINE / 2), PathOp.DIFFERENCE)
    else:
        ring = arc(filled[0], filled[-1])
    for angle in filled:
        ring = op(ring, circle(*place(angle), ELECTRON + GAP), PathOp.DIFFERENCE)
    for angle in filled:
        ring = op(ring, circle(*place(angle), ELECTRON), PathOp.UNION)
    return ring


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


def vector(d, colour, size=108):
    """An adaptive icon layer: 108dp, of which the middle 72dp shows, holding the 512 grid. Or,
    with another size, a plain icon whose whole box holds the grid."""
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- Generated by tools/app_icon.py. -->\n'
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            '    android:width="%ddp"\n'
            '    android:height="%ddp"\n'
            '    android:viewportWidth="%d"\n'
            '    android:viewportHeight="%d">\n'
            '    <path\n'
            '        android:fillColor="%s"\n'
            '        android:pathData="%s"/>\n'
            '</vector>\n' % (size, size, size, size, colour, d))


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
    if sys.argv[1:2] == ['--dubna']:
        out, d = Path(sys.argv[2]).resolve(), dubna()
        write(out / 'dubna.svg', svg(512, '<path d="%s"/>' % path_data(d, 512)))
        write(out / 'dubna-icon.svg', svg(512, '<circle cx="256" cy="256" r="240" fill="%s"/>\n<path fill="#fff" d="%s"/>'
                                          % (DUBNA_PLATE, path_data(d, 512, 16))))
        return

    ts = glyph(7)
    _, default_bg, _ = COLOURS[0]
    res = ROOT / 'res/app_icon'
    d512, d108 = path_data(ts, 512), path_data(ts, 108, 18)
    write(res / 'glyph.svg', svg(512, '<path d="%s"/>' % d512))
    # Chromium's PNGs. Each layered one is the whole icon, glyph included: the application's icon
    # draws layered_app_icon as its foreground, and its round icon draws layered_app_icon_background
    # under a transparent foreground. Some phones hand apps such as F-Droid the round one.
    layered = svg(108, '<rect width="108" height="108" fill="%s"/>\n<path fill="#fff" d="%s"/>' % (default_bg, d108))
    write(res / 'layered_app_icon_background.svg', layered)
    write(res / 'layered_app_icon.svg', layered)
    # The round icon older launchers show.
    write(res / 'app_icon.svg', svg(512, '<circle cx="256" cy="256" r="240" fill="%s"/>\n<path fill="#fff" d="%s"/>'
                                    % (default_bg, path_data(ts, 512, 16))))

    write(ROOT / 'res/drawable/themed_app_icon.xml', vector(d108, '#000000'))
    drawables = ROOT / 'src/chrome/browser/app_icon/android/java/res/drawable'
    for colour in ('white', 'black'):
        write(drawables / ('app_icon_glyph_%s.xml' % colour), vector(d108, '#FFFFFF' if colour == 'white' else '#000000'))
    for name, background, glyph_colour in COLOURS:
        write(drawables / ('app_icon_%s.xml' % name), adaptive(background, glyph_colour))
    write(drawables / 'app_icon_clear.xml', vector(path_data(ts, 48), '#FFFFFF', 48))
    write(drawables / 'app_icon_clearblack.xml', vector(path_data(ts, 48), '#000000', 48))


if __name__ == '__main__':
    main()

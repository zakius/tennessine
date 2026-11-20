#!/usr/bin/env python3
"""End-to-end test of a page's width with page zoom following Android's font
size (zakius/tennessine#52), on the emulator or an attached device, with the
browser installed there.

The page zoom is Android's font scale, rounded so that a page's width is a
whole number of CSS pixels (page-zoom-follows-font-size.patch). Without the
rounding, a page on a 1080 px screen at density 374 and font scale 0.85 is
543.567 px wide, and matches neither `width <= 543px` nor `width >= 544px`.

A local page, reached through `adb reverse`, has those two media queries and
shows which one matches. For each setting the test reads, through DevTools,
the width of document.documentElement and visualViewport.width, which must be
whole numbers, and checks that exactly one of the two queries matches and
that `(width: <the width>px)` matches. Blink divides the window's width by
the zoom in single precision, so a whole width can read as 483.99997: the
test allows 0.001 px, which the media queries' own tolerance (1/64 px)
covers. The settings: density 374 with font scale 0.85 (the maintainer's
phone), density 420 with font scale 0.85 and 1.15, then in landscape and in
split screen (landscape, side by side) without reloading the page, which
must round again for the new width. At font scale 1.0 the width must stay
the window's width in DIPs, unrounded. It sets the density, the font scale
and the rotation back at the end, also after a failure.

Split screen goes through SystemUI's WM Shell command
(`dumpsys activity service SystemUIService WMShell splitscreen
moveToSideStage`), with Settings in the other half. When the page doesn't get
narrower, that case is reported as skipped, not as passed.

With APK, it first installs that build over the installed one (an x86_64
build for the emulator, AVD `tennessine`: ./tennessine.py build x64).

Usage: tools/page_width_test.py [APK]
"""
import argparse
import functools
import http.server
import json
import re
import sys
import tempfile
import threading
import time
from pathlib import Path

from screenshots import PACKAGE, Devtools, adb, shell
from xpi_install_test import targets, wait_for

PAGE = '''<!doctype html>
<meta name="viewport" content="width=device-width">
<style>
  p { display: none; font: 24px sans-serif }
  @media screen and (width <= 543px) { #narrow { display: block } }
  @media screen and (width >= 544px) { #wide { display: block } }
</style>
<p id="narrow">width &lt;= 543px matches</p>
<p id="wide">width &gt;= 544px matches</p>
'''
# What the test reads from the page.
MEASURE = '''(() => {
  const width = document.documentElement.getBoundingClientRect().width;
  return {
    width: width,
    viewport: visualViewport.width,
    innerWidth: innerWidth,
    devicePixelRatio: devicePixelRatio,
    narrow: matchMedia('screen and (width <= 543px)').matches,
    wide: matchMedia('screen and (width >= 544px)').matches,
    exact: matchMedia('(width: ' + Math.round(width) + 'px)').matches,
  };
})()'''
# Single precision at these widths is about 0.00006 px.
WHOLE = 0.001
SETTINGS = 'com.android.settings'
SPLIT_SCREEN = 'dumpsys activity service SystemUIService WMShell splitscreen'


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    handler = functools.partial(QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def saved_settings():
    """The density override, font scale and rotation, to put back."""
    density = re.search(r'Override density: (\d+)', shell('wm density'))
    saved = {'density': density.group(1) if density else None}
    for name in ('font_scale', 'accelerometer_rotation', 'user_rotation'):
        value = shell(f'settings get system {name}').strip()
        saved[name] = None if value == 'null' else value
    return saved


def restore(saved):
    if saved['density']:
        shell(f'wm density {saved["density"]}')
    else:
        shell('wm density reset')
    for name in ('font_scale', 'accelerometer_rotation', 'user_rotation'):
        if saved[name] is None:
            shell(f'settings delete system {name}')
        else:
            shell(f'settings put system {name} {saved[name]}')


def rotate(landscape):
    shell('settings put system accelerometer_rotation 0')
    shell(f'settings put system user_rotation {1 if landscape else 0}')


def open_page(url):
    """Starts the browser on URL; returns a DevTools session with the page."""
    shell(f'am start -a android.intent.action.VIEW -d {url} {PACKAGE}')
    target = wait_for(lambda: next((t for t in targets()
                                    if t['type'] == 'page' and t['url'] == url), None),
                      60, url)
    session = Devtools(target['webSocketDebuggerUrl'])
    wait_for(lambda: session.call('Runtime.evaluate', expression='document.readyState')
             ['result']['value'] == 'complete', 30, f'{url} to load')
    time.sleep(2)  # the browser's toolbar and the first layout
    return session


def measure(session):
    result = session.call('Runtime.evaluate', expression=MEASURE, returnByValue=True)
    return result['result']['value']


def is_whole(value):
    return abs(value - round(value)) < WHOLE


def check_rounded(what, values):
    """Returns a failure, or None."""
    print(f'{what}: {json.dumps(values)}')
    if not is_whole(values['width']):
        return f'{what}: the page is {values["width"]} px wide'
    if not is_whole(values['viewport']):
        return f'{what}: visualViewport.width is {values["viewport"]}'
    if values['narrow'] == values['wide']:
        return f'{what}: width <= 543px is {values["narrow"]}, width >= 544px too'
    if not values['exact']:
        return f'{what}: (width: {round(values["width"])}px) does not match'
    return None


def check_unrounded(what, values, density):
    """At font scale 1.0, the page is the window's width in DIPs."""
    print(f'{what}: {json.dumps(values)}')
    screen_width = int(re.search(r'(\d+)x\d+', shell('wm size')).group(1))
    expected = screen_width / (density / 160)
    if abs(values['width'] - expected) > WHOLE:
        return f'{what}: the page is {values["width"]} px wide, not {expected:.3f}'
    return None


def browser_task():
    """The browser's task ID."""
    activities = shell('dumpsys activity activities')
    found = re.search(r'#(\d+) type=standard A=\d+:' + re.escape(PACKAGE), activities)
    return found.group(1) if found else None


def enter_split_screen():
    """Puts Settings and the browser side by side; False if the browser's task
    isn't found. Whether the page got narrower tells if it worked."""
    task = browser_task()
    if not task:
        return False
    shell(f'am start -n {SETTINGS}/.Settings')
    time.sleep(2)
    # 1: the browser on the right.
    shell(f'{SPLIT_SCREEN} moveToSideStage {task} 1')
    time.sleep(3)
    return True


def leave_split_screen():
    shell(f'am force-stop {SETTINGS}')
    shell('input keyevent KEYCODE_HOME')


def run(base_url, failures):
    def record(failure):
        if failure:
            print(f'FAIL: {failure}')
            failures.append(failure)
        else:
            print('pass')

    def restart(density, font_scale, name):
        shell(f'am force-stop {PACKAGE}')
        shell(f'wm density {density}')
        shell(f'settings put system font_scale {font_scale}')
        time.sleep(2)
        return open_page(f'{base_url}?{name}')

    rotate(landscape=False)
    for density, font_scale in ((374, 0.85), (420, 0.85), (420, 1.15)):
        what = f'density {density}, font scale {font_scale}'
        session = restart(density, font_scale, f'{density}-{font_scale}')
        record(check_rounded(what, measure(session)))
        session.close()

    session = restart(420, 1.0, '420-1.0')
    record(check_unrounded('density 420, font scale 1.0', measure(session), 420))
    session.close()

    # The same page, resized: it must round again for the new width.
    session = restart(420, 1.15, 'resized')
    try:
        rotate(landscape=True)
        time.sleep(4)
        landscape = measure(session)
        record(check_rounded('density 420, font scale 1.15, landscape', landscape))
        if not enter_split_screen():
            print("SKIP: split screen: the browser's task wasn't found")
            return
        split = measure(session)
    finally:
        session.close()
    if split['width'] >= landscape['width'] - 1:
        print(f'split screen: {json.dumps(split)}')
        print('SKIP: split screen: the page did not get narrower, so the WM Shell'
              ' command did not put it in split screen')
        return
    record(check_rounded('density 420, font scale 1.15, split screen', split))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('apk', nargs='?')
    args = parser.parse_args()
    if args.apk:
        adb('install', '-r', args.apk)
    saved = saved_settings()
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, 'index.html').write_text(PAGE)
        server = serve(tmp)
        port = server.server_address[1]
        adb('reverse', f'tcp:{port}', f'tcp:{port}')
        try:
            run(f'http://localhost:{port}/', failures)
        except AssertionError as error:
            print(f'FAIL: {error}')
            failures.append(str(error))
        finally:
            leave_split_screen()
            shell(f'am force-stop {PACKAGE}')
            restore(saved)
            adb('reverse', '--remove', f'tcp:{port}')
            server.shutdown()
    print(f'{len(failures)} failed' if failures else 'all passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()

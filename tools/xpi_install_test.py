#!/usr/bin/env python3
"""End-to-end test of a downloaded .xpi on Android (zakius/tennessine#47), on
the emulator or an attached device, with the browser installed there.

A local server, reached through `adb reverse`, serves a page whose link to a
test .xpi fills the screen. The test taps it, accepts the install prompt, and
finds the add-on running with the ID its Gecko ID gives (dubna's
firefox_gecko_id.cc), then uninstalls it. Its icons are an SVG, as uBlock
Origin's and Pouch's are, which a packed install doesn't decode
(zakius/dubna#37). The first run downloads the .xpi
to a free name; each later one over the file of the run before, where
Android asks whether to overwrite it. Each checks that its download took a
new name. Then a .xpi whose manifest has no name, which can't install: the
test finds the message that says why (zakius/dubna#206), and so for a .xpi
without a Gecko ID. With APK, it first
installs that
build over the installed one (an x86_64 build for the emulator: ./tennessine.py
build x64).

Usage: tools/xpi_install_test.py [--repeat N] [APK]
  N: the runs, 2 by default, so that one meets a file of the .xpi's name
"""
import argparse
import functools
import hashlib
import html
import http.server
import io
import json
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

from screenshots import DEVTOOLS_PORT, PACKAGE, Devtools, adb, shell

GECKO_ID = 'xpi-install-test@tennessine.test'
NAME = 'XPI install test'
# The manifest error of an add-on without a name (manifest_constants.h).
BROKEN_ERROR = "Required value 'name' is missing or invalid."
# What firefox_xpi_installer.cc says of a .xpi without a Gecko ID.
NO_GECKO_ID_ERROR = 'This add-on has no Gecko ID'
DOWNLOADS = '/sdcard/Download'


def extension_id(gecko_id):
    """crx_file::id_util::GenerateId(): the SHA-256's first 16 bytes in a to p."""
    digest = hashlib.sha256(gecko_id.encode()).hexdigest()[:32]
    return digest.translate(str.maketrans('0123456789abcdef', 'abcdefghijklmnop'))


def xpi(broken=False, gecko_id=GECKO_ID):
    manifest = {
        'manifest_version': 2,
        'name': NAME,
        'version': '1.0',
        'browser_specific_settings': {'gecko': {'id': gecko_id}},
        'background': {'scripts': ['background.js']},
        'icons': {'48': 'icon.svg', '96': 'icon.svg'},
        'browser_action': {'default_icon': {'48': 'icon.svg'}},
    }
    if broken:
        del manifest['name']
    if not gecko_id:
        del manifest['browser_specific_settings']
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w') as archive:
        archive.writestr('manifest.json', json.dumps(manifest))
        archive.writestr('background.js', 'window.started = true;\n')
        archive.writestr('icon.svg', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">'
                         '<rect width="16" height="16" fill="green"/></svg>')
    return data.getvalue()


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    handler = functools.partial(QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def targets():
    adb('forward', f'tcp:{DEVTOOLS_PORT}', 'localabstract:chrome_devtools_remote')
    url = f'http://127.0.0.1:{DEVTOOLS_PORT}/json/list'
    try:
        return json.loads(urllib.request.urlopen(url, timeout=10).read())
    except OSError:
        return []


def wait_for(check, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = check()
        if result:
            return result
        time.sleep(1)
    raise AssertionError(f'timed out after {timeout}s waiting for {what}')


def background(addon_id):
    prefix = f'chrome-extension://{addon_id}/'
    return next((t for t in targets() if t['url'].startswith(prefix)), None)


def uninstall(addon_id):
    """Uninstalls the add-on from its background page, without a prompt."""
    target = background(addon_id)
    if not target:
        return
    session = Devtools(target['webSocketDebuggerUrl'])
    # Once DevTools has answered: the page closes with the add-on.
    session.call('Runtime.evaluate',
                 expression='setTimeout(() => chrome.management.uninstallSelf(), 100)')
    session.close()
    wait_for(lambda: not background(addon_id), 30, 'the add-on to go')


def tap_text(pattern):
    """Taps the view whose text matches PATTERN; False if none shows."""
    shell('uiautomator dump /sdcard/ui.xml')
    xml = shell('cat /sdcard/ui.xml')
    for node in re.finditer(r'<node [^>]*>', xml):
        text = re.search(r' text="([^"]*)"', node.group())
        bounds = re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', node.group())
        if text and bounds and re.fullmatch(pattern, text.group(1)):
            x1, y1, x2, y2 = map(int, bounds.groups())
            shell(f'input tap {(x1 + x2) // 2} {(y1 + y2) // 2}')
            return True
    return False


def shows_text(text):
    """Whether a view on screen has TEXT in its text."""
    try:
        shell('uiautomator dump /sdcard/ui.xml')
    except subprocess.CalledProcessError:
        return False  # uiautomator, killed under a high load: try again
    xml = shell('cat /sdcard/ui.xml')
    # The dump escapes quotes: ' is &apos;.
    return any(text in html.unescape(found)
               for found in re.findall(r' text="([^"]*)"', xml))


def downloaded():
    # A taken name gets a number: "test (1).xpi".
    return {name for name in shell(f'ls -1 {DOWNLOADS}').splitlines()
            if name.startswith('test') and name.endswith('.xpi')}


def tap_link(url):
    """Opens the page at URL and taps its link, which fills the page."""
    shell(f'am start -a android.intent.action.VIEW -d {url} {PACKAGE}')
    wait_for(lambda: any(t['type'] == 'page' and t['url'] == url for t in targets()),
             30, 'the test page')
    time.sleep(2)  # its first paint
    # The link fills the page, so the middle of the screen is on it.
    width, height = map(int, re.search(r'(\d+)x(\d+)', shell('wm size')).groups())
    shell(f'input tap {width // 2} {height // 2}')


def run_broken(port, page, error):
    """A .xpi that can't install says why in a message on the tab. It may wait
    behind the download's own message."""
    tap_link(f'http://localhost:{port}/{page}')
    wait_for(lambda: shows_text(error), 60, 'the install error')


def run_once(port, addon_id, again):
    uninstall(addon_id)
    before = downloaded()
    assert bool(before) == again, f'earlier downloads: {sorted(before)}'
    tap_link(f'http://localhost:{port}/')
    wait_for(lambda: tap_text(r'Add extension'), 30, 'the install prompt')
    target = wait_for(lambda: background(addon_id), 30, 'the add-on to start')
    # The page is listed before its script runs.
    session = Devtools(target['webSocketDebuggerUrl'])
    wait_for(lambda: session.call('Runtime.evaluate',
                                  expression='window.started === true')['result']['value'],
             30, 'its background script')
    session.close()
    # The post-install dialog, on the tab: while one waits, even on a tab in
    # the background, Android shows no message, such as an install error.
    wait_for(lambda: tap_text(r'Got it'), 30, 'the post-install dialog')
    new = downloaded() - before
    assert len(new) == 1, f'new downloads: {sorted(new)}'
    uninstall(addon_id)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('apk', nargs='?')
    parser.add_argument('--repeat', type=int, default=2)
    args = parser.parse_args()
    if args.apk:
        adb('install', '-r', args.apk)
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, 'test.xpi').write_bytes(xpi())
        # Not test*.xpi, which downloaded() counts.
        Path(tmp, 'broken.xpi').write_bytes(xpi(broken=True))
        Path(tmp, 'no-id.xpi').write_bytes(xpi(gecko_id=None))
        for page, link in (('index.html', 'test.xpi'), ('broken.html', 'broken.xpi'),
                           ('no-id.html', 'no-id.xpi')):
            Path(tmp, page).write_text(
                '<!doctype html><meta name="viewport" content="width=device-width">'
                f'<a href="{link}" style="position:fixed;inset:0">Install {link}</a>')
        server = serve(tmp)
        port = server.server_address[1]
        adb('reverse', f'tcp:{port}', f'tcp:{port}')
        addon_id = extension_id(GECKO_ID)
        # Once: each later run downloads the .xpi again, over a file of its
        # name, which takes a new one.
        shell(f'rm -f {DOWNLOADS}/test*.xpi {DOWNLOADS}/broken*.xpi {DOWNLOADS}/no-id*.xpi')
        failures = 0
        try:
            for run in range(1, args.repeat + 1):
                try:
                    run_once(port, addon_id, again=run > 1)
                    print(f'{run}/{args.repeat} pass')
                except AssertionError as error:
                    failures += 1
                    print(f'{run}/{args.repeat} FAIL: {error}')
            for name, page, text in (('broken', 'broken.html', BROKEN_ERROR),
                                     ('no Gecko ID', 'no-id.html', NO_GECKO_ID_ERROR)):
                try:
                    run_broken(port, page, text)
                    print(f'{name} .xpi pass')
                except AssertionError as error:
                    failures += 1
                    print(f'{name} .xpi FAIL: {error}')
        finally:
            adb('reverse', '--remove', f'tcp:{port}')
            server.shutdown()
    print(f'{args.repeat + 2 - failures} of {args.repeat + 2} passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()

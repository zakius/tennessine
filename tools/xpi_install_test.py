#!/usr/bin/env python3
"""End-to-end test of a downloaded .xpi on Android (zakius/tennessine#47), on
the emulator or an attached device, with the browser installed there.

A local server, reached through `adb reverse`, serves a page whose link to a
test .xpi fills the screen. The test taps it, accepts the install prompt, and
finds the add-on running with the ID its Gecko ID gives (dubna's
firefox_gecko_id.cc), then uninstalls it. With APK, it first installs that
build over the installed one (an x86_64 build for the emulator: ./tennessine.py
build x64).

Usage: tools/xpi_install_test.py [--repeat N] [APK]
"""
import argparse
import functools
import hashlib
import http.server
import io
import json
import re
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
DOWNLOADS = '/sdcard/Download'


def extension_id(gecko_id):
    """crx_file::id_util::GenerateId(): the SHA-256's first 16 bytes in a to p."""
    digest = hashlib.sha256(gecko_id.encode()).hexdigest()[:32]
    return digest.translate(str.maketrans('0123456789abcdef', 'abcdefghijklmnop'))


def xpi():
    manifest = {
        'manifest_version': 2,
        'name': NAME,
        'version': '1.0',
        'browser_specific_settings': {'gecko': {'id': GECKO_ID}},
        'background': {'scripts': ['background.js']},
    }
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w') as archive:
        archive.writestr('manifest.json', json.dumps(manifest))
        archive.writestr('background.js', 'window.started = true;\n')
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


def run_once(port, addon_id):
    uninstall(addon_id)
    shell(f'rm -f {DOWNLOADS}/test*.xpi')
    url = f'http://localhost:{port}/'
    shell(f'am start -a android.intent.action.VIEW -d {url} {PACKAGE}')
    wait_for(lambda: any(t['type'] == 'page' and t['url'] == url for t in targets()),
             30, 'the test page')
    time.sleep(2)  # its first paint
    # The link fills the page, so the middle of the screen is on it.
    width, height = map(int, re.search(r'(\d+)x(\d+)', shell('wm size')).groups())
    shell(f'input tap {width // 2} {height // 2}')
    wait_for(lambda: tap_text(r'Add extension'), 30, 'the install prompt')
    target = wait_for(lambda: background(addon_id), 30, 'the add-on to start')
    # The page is listed before its script runs.
    session = Devtools(target['webSocketDebuggerUrl'])
    wait_for(lambda: session.call('Runtime.evaluate',
                                  expression='window.started === true')['result']['value'],
             30, 'its background script')
    session.close()
    uninstall(addon_id)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('apk', nargs='?')
    parser.add_argument('--repeat', type=int, default=1)
    args = parser.parse_args()
    if args.apk:
        adb('install', '-r', args.apk)
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, 'test.xpi').write_bytes(xpi())
        Path(tmp, 'index.html').write_text(
            '<!doctype html><meta name="viewport" content="width=device-width">'
            '<a href="test.xpi" style="position:fixed;inset:0">Install test.xpi</a>')
        server = serve(tmp)
        port = server.server_address[1]
        adb('reverse', f'tcp:{port}', f'tcp:{port}')
        addon_id = extension_id(GECKO_ID)
        failures = 0
        try:
            for run in range(1, args.repeat + 1):
                try:
                    run_once(port, addon_id)
                    print(f'{run}/{args.repeat} pass')
                except AssertionError as error:
                    failures += 1
                    print(f'{run}/{args.repeat} FAIL: {error}')
        finally:
            adb('reverse', '--remove', f'tcp:{port}')
            server.shutdown()
    print(f'{args.repeat - failures} of {args.repeat} passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()

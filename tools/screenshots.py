#!/usr/bin/env python3
"""Screenshots of tennessine for dubna's site, on the Android emulator (AVD
`tennessine`, x86_64; started here when no device is attached, and stopped
again) or an attached device with root. The APK is installed on a cleared
profile, run once past its first run, then set up to show its features: the
address bar at the bottom, where the menu, the extensions menu and the tab
list follow it, and a rearranged toolbar with its settings screen. Pages come
from Wikipedia, in its dark theme, so run it online. Saves OUT/<scene>.webp.

Usage: tools/screenshots.py APK OUT
  APK: an x86_64 build for the emulator (./tennessine.py build x64)
"""
import base64
import json
import os
import re
import socket
import struct
import urllib.request
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SDK = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk'))
ADB = str(SDK / 'platform-tools/adb')
PACKAGE = 'io.github.zakius.tennessine'
DATA = f'/data/data/{PACKAGE}'
WIKI = 'https://en.m.wikipedia.org/wiki/'
# On theme, as oganesson's: the elements the browsers are named after, and the
# town. The first run opens the first; each page once (a repeat counts as an
# inactive duplicate), Tennessine last, so it's the one shown.
PAGES = ['Oganesson', 'Dubna', 'Helium', 'Tennessine']
# The toolbar's items, left to right ("-" hides one): the extensions button
# moved left of the address bar, Home and New tab hidden.
TOOLBAR = 'extensions,location,tabs,menu,-home,-shortcut'
SETTINGS = 'org.chromium.chrome.browser.settings.SettingsActivity'
TOOLBAR_SETTINGS = 'org.chromium.chrome.browser.toolbar_items.ToolbarItemsSettings'


DEVTOOLS_PORT = 9333


class Devtools:
    """The browser's DevTools protocol over adb, for what the pages need before a
    screenshot (cookies, a banner to close): a minimal WebSocket client."""

    def __init__(self, ws_url):
        host, path = ws_url.removeprefix('ws://').split('/', 1)
        self.sock = socket.create_connection(('127.0.0.1', DEVTOOLS_PORT), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET /{path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n'
                           f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                           'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        response = b''
        while b'\r\n\r\n' not in response:
            response += self._recv(1)
        self.next_id = 0

    def _recv(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                sys.exit('screenshots: DevTools closed the connection')
            data += chunk
        return data

    def call(self, method, **params):
        self.next_id += 1
        payload = json.dumps({'id': self.next_id, 'method': method, 'params': params}).encode()
        mask = os.urandom(4)
        size = len(payload)
        head = struct.pack('!BB', 0x81, 0x80 | (size if size < 126 else 126 if size < 65536 else 127))
        head += b'' if size < 126 else struct.pack('!H' if size < 65536 else '!Q', size)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        while True:
            first, second = self._recv(2)
            size = second & 0x7f
            if size == 126:
                size = struct.unpack('!H', self._recv(2))[0]
            elif size == 127:
                size = struct.unpack('!Q', self._recv(8))[0]
            message = json.loads(self._recv(size)) if first & 0x0f == 1 else {}
            if message.get('id') == self.next_id:
                if 'error' in message:
                    sys.exit(f'screenshots: {method}: {message["error"]}')
                return message.get('result', {})

    def close(self):
        self.sock.close()


def devtools(url_part=None):
    """A DevTools session with the browser, or with its page whose URL has URL_PART."""
    adb('forward', f'tcp:{DEVTOOLS_PORT}', 'localabstract:chrome_devtools_remote')
    get = lambda path: json.loads(urllib.request.urlopen(f'http://127.0.0.1:{DEVTOOLS_PORT}{path}', timeout=10).read())
    if url_part is None:
        return Devtools(get('/json/version')['webSocketDebuggerUrl'])
    page = next(t for t in get('/json/list') if t['type'] == 'page' and url_part in t['url'])
    return Devtools(page['webSocketDebuggerUrl'])


def adb(*args, **kwargs):
    return subprocess.run([ADB, *args], check=True, capture_output=True, **kwargs).stdout


def shell(command):
    return adb('shell', command).decode()


def wait(check, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if check():
            return
        time.sleep(2)
    sys.exit(f'screenshots: timed out waiting for {what}')


def device():
    """Starts the emulator unless a device is attached; returns its process or None."""
    if b'\tdevice' in adb('devices'):
        return None
    emulator = subprocess.Popen(
        [str(SDK / 'emulator/emulator'), '-avd', 'tennessine', '-no-window', '-no-audio',
         '-no-boot-anim', '-gpu', 'swiftshader_indirect', '-no-snapshot-save'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait(lambda: subprocess.run([ADB, 'shell', 'getprop', 'sys.boot_completed'],
                                capture_output=True).stdout.strip() == b'1', 300, 'the emulator')
    return emulator


def open_pages():
    for page in PAGES[1:]:
        shell(f'am start -a android.intent.action.VIEW -d {WIKI}{page} {PACKAGE}')
        time.sleep(5)
    # A changed toolbar takes a restart or two (the tabs come back) to show,
    # at the bottom.
    for _ in range(3):
        shell(f'am force-stop {PACKAGE}')
        shell(f'monkey -p {PACKAGE} -c android.intent.category.LAUNCHER 1')
        time.sleep(8)
        if toolbar_at_bottom():
            return
    sys.exit('screenshots: the toolbar is not at the bottom')


def toolbar_at_bottom():
    shell('uiautomator dump /sdcard/ui.xml')
    xml = shell('cat /sdcard/ui.xml')
    match = re.search(re.escape(f'{PACKAGE}:id/toolbar"') + r'[^>]*bounds="\[\d+,(\d+)\]', xml)
    height = int(re.search(r'(\d+)x(\d+)', shell('wm size')).group(2))
    return bool(match) and int(match.group(1)) > height // 2


def tap(resource_id=None, text=None):
    """Taps the view with RESOURCE_ID or TEXT in the current screen; False if none."""
    shell('uiautomator dump /sdcard/ui.xml')
    xml = shell('cat /sdcard/ui.xml')
    attr = f'resource-id="{PACKAGE}:id/{resource_id}"' if resource_id else f'text="{text}"'
    match = re.search(re.escape(attr) + r'[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml)
    if not match:
        return False
    x1, y1, x2, y2 = map(int, match.groups())
    shell(f'input tap {(x1 + x2) // 2} {(y1 + y2) // 2}')
    return True


def capture(out):
    png = adb('exec-out', 'screencap', '-p')
    subprocess.run(['magick', 'png:-', '-resize', '540x', '-quality', '86', str(out)],
                   input=png, check=True)
    print(out)


def set_up(apk):
    adb('root')
    wait(lambda: b'\tdevice' in adb('devices'), 30, 'adb as root')
    # A fresh install: an older one may be signed with another key.
    subprocess.run([ADB, 'uninstall', PACKAGE], capture_output=True)
    adb('install', str(apk))
    shell(f'pm grant {PACKAGE} android.permission.POST_NOTIFICATIONS')
    shell('cmd uimode night yes')
    # Past the first run once; the command line only for that, as the browser
    # warns about it while it's there. A userdebug build reads it from
    # /data/local/tmp; SELinux keeps apps out of /data/local itself.
    shell('echo "_ --disable-fre --no-default-browser-check --no-first-run"'
          ' > /data/local/tmp/chrome-command-line')
    shell(f'am start -a android.intent.action.VIEW -d {WIKI}{PAGES[0]} {PACKAGE}')
    time.sleep(25)  # first run, uBlock Origin's install
    shell(f'am force-stop {PACKAGE}')
    shell('rm -f /data/local/tmp/chrome-command-line')
    # The address bar at the bottom: Local State's pref and the toolbar's own,
    # and the rearranged toolbar.
    uid = shell(f'stat -c %u {DATA}').strip()
    local_state = f'{DATA}/app_chrome/Local State'
    prefs = f'{DATA}/shared_prefs/{PACKAGE}_preferences.xml'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        adb('pull', local_state, str(tmp / 'state'))
        adb('pull', prefs, str(tmp / 'prefs.xml'))
        state = json.loads((tmp / 'state').read_text())
        state.setdefault('omnibox', {})['is_in_bottom_position'] = True
        (tmp / 'state').write_text(json.dumps(state))
        xml = (tmp / 'prefs.xml').read_text()
        # TopAnchored is what startup reads, before Local State: 3 is "bottom, set
        # in Settings" (ToolbarPositionAndSource.BOTTOM_SETTINGS).
        xml = re.sub(r'\s*<int name="Chrome.Toolbar.TopAnchored"[^>]*/>', '', xml)
        xml = xml.replace('</map>', '    <int name="Chrome.Toolbar.TopAnchored" value="3" />\n'
                          f'    <string name="tennessine.toolbar.items">{TOOLBAR}</string>\n</map>')
        (tmp / 'prefs.xml').write_text(xml)
        adb('push', str(tmp / 'state'), local_state)
        adb('push', str(tmp / 'prefs.xml'), prefs)
    shell(f"chown {uid}:{uid} '{local_state}' {prefs}; restorecon '{local_state}' {prefs}")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    apk, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    emulator = device()
    try:
        set_up(apk)
        open_pages()
        # Wikipedia's own dark theme, to go with the browser's. Set now, as the
        # restarts above would lose a cookie the browser hadn't saved yet.
        browser = devtools()
        browser.call('Storage.setCookies', cookies=[
            {'name': 'enwikimwclientpreferences', 'value': 'skin-theme-clientpref-night',
             'domain': '.wikipedia.org', 'path': '/'}])
        browser.close()
        page = devtools(PAGES[-1])
        page.call('Page.reload')
        time.sleep(6)
        # Wikipedia's banner over the article, which a reader would close.
        page.call('Runtime.evaluate', expression="document.getElementById('cbnnr')?.remove()")
        page.close()
        time.sleep(1)
        capture(out / 'android-bottom.webp')
        scenes = [('android-menu.webp', 'menu_button'),
                  ('android-extensions.webp', 'extensions_menu_button'),
                  ('android-tabs.webp', 'tab_switcher_button')]
        for name, button in scenes:
            if not tap(resource_id=button):
                sys.exit(f'screenshots: no {button}')
            time.sleep(3)
            if tap(text='Got it'):  # a one-time tip over the tab list
                time.sleep(2)
            capture(out / name)
            shell('input keyevent KEYCODE_BACK')
            time.sleep(2)
        # Settings > Toolbar layout.
        shell(f'am start -n {PACKAGE}/{SETTINGS} --es show_fragment {TOOLBAR_SETTINGS}')
        time.sleep(3)
        capture(out / 'android-toolbar.webp')
    finally:
        if emulator:
            adb('emu', 'kill')
            emulator.wait(timeout=60)


if __name__ == '__main__':
    main()

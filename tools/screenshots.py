#!/usr/bin/env python3
"""Screenshots of tennessine for dubna's site, on the Android emulator (AVD
`tennessine`, x86_64; started here when no device is attached, and stopped
again) or an attached device with root. The APK is installed on a cleared
profile, run once past its first run, then set up to show a feature: the
address bar at the bottom, where the menu and tab list follow it. Pages come
from dubna's site, so run it online. Saves OUT/<scene>.webp.

Usage: tools/screenshots.py APK OUT
  APK: an x86_64 build for the emulator (./tennessine.py build x64)
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SDK = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk'))
ADB = str(SDK / 'platform-tools/adb')
PACKAGE = 'io.github.zakius.tennessine'
DATA = f'/data/data/{PACKAGE}'
SITE = 'https://dubna.zakius.xyz/'
# The first run opens install.html; each page once (a repeat counts as an
# inactive duplicate), the home page last.
PAGES = ['everything.html', '']


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
    for page in PAGES:
        shell(f'am start -a android.intent.action.VIEW -d {SITE}{page} {PACKAGE}')
        time.sleep(4)
    # The first start after the prefs change comes up without its toolbar; a
    # restart (the tabs come back) shows it at the bottom.
    shell(f'am force-stop {PACKAGE}')
    shell(f'monkey -p {PACKAGE} -c android.intent.category.LAUNCHER 1')
    time.sleep(8)


def visible(resource_id):
    shell('uiautomator dump /sdcard/ui.xml')
    return f'{PACKAGE}:id/{resource_id}' in shell('cat /sdcard/ui.xml')


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
    # warns about it while it's there.
    shell('echo "_ --disable-fre --no-default-browser-check --no-first-run"'
          ' > /data/local/chrome-command-line')
    shell(f'am start -a android.intent.action.VIEW -d {SITE}install.html {PACKAGE}')
    time.sleep(25)  # first run, uBlock Origin's install
    shell(f'am force-stop {PACKAGE}')
    shell('rm -f /data/local/chrome-command-line')
    # The address bar at the bottom: Local State's pref and the toolbar's own.
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
        if 'Chrome.Toolbar.TopAnchored' not in xml:
            xml = xml.replace('</map>',
                              '    <boolean name="Chrome.Toolbar.TopAnchored" value="false" />\n</map>')
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
        if not visible('tab_switcher_button'):
            sys.exit('screenshots: the toolbar is missing')
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
    finally:
        if emulator:
            adb('emu', 'kill')
            emulator.wait(timeout=60)


if __name__ == '__main__':
    main()

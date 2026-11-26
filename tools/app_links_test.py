#!/usr/bin/env python3
"""End-to-end test of links in a new window of an installed web app
(zakius/tennessine#55, after zakius/dubna#223), on the emulator or an attached
device, with the browser installed there.

A local server on localhost:8123, reached through `adb reverse`, serves the
app AppLinksTest (/app/, with a manifest). The test installs it through the
browser's menu (Save and share > Install and create shortcut > Install) and
the launcher's prompt, unless a shortcut of that name is pinned already
(`dumpsys shortcut`), and keeps it installed. A home screen app's scope is
its whole site, so the test gives each such app the rule
http://localhost:8123/app/* (Settings > Web apps > AppLinksTest > Pages that
stay in the app), which narrows its scope on localhost to /app/. /out.html is
then out of it. It writes the rule into the profile's Preferences with the
browser stopped, as tools/screenshots.py sets prefs, and starts the app with
its shortcut's intent through the browser's own launcher activity, which
isn't exported: both take adb as root (adb root). The app's page is one big
link that opens /app/window.html with target=_blank, which the rule keeps in
the app: a new window, shown as a tab of the app's activity. That page is one big link to
/out.html?plain, above two screens of page to scroll.

The cases, each in a new window opened by a tap in the app's page:

1. A real touch scroll (`input swipe`) in the new window, then a script's
   pushState() out of scope: the window keeps the pushed address, and the
   default browser opens nothing. A touch that becomes a scroll gives the page
   no activation, so the push isn't from input. Then a tap on the link, which
   is input, opens the default browser: that shows the test sees it.
2. A script's a.click() on the out-of-scope link: the page loads in the
   window, as a script navigation does, and the default browser opens nothing.

On desktop, a new app window has the activation of a click in another window
for its first 5 s, as the activation of script run in it, which made both
cases leave the app before dubna#223. On Android the new window had none in
158.0.8127.92 (a popup neither), and without activation the browser's checks
kept both in the app before dubna#223 too: there the cases check the outcome
on Android, not that fix. The test prints whether the window has activation
as the push or the click runs; when it does, the same cases check the fix.

The default browser must be another app (the emulator's is Chrome). Its
launch is the system's `START` line in logcat for a VIEW intent from the
browser's uid to another app. With APK, it first installs that build over the
installed one (an x86_64 build for the emulator, AVD `tennessine`:
./tennessine.py build x64).

Usage: tools/app_links_test.py [APK]
"""
import argparse
import functools
import http.server
import json
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from screenshots import ADB, DATA, PACKAGE, Devtools, adb, shell, tap
from xpi_install_test import tap_text, targets, wait_for

PORT = 8123
ORIGIN = f'http://localhost:{PORT}'
NAME = 'AppLinksTest'
RULE = f'{ORIGIN}/app/*'
PREFERENCES = f'{DATA}/app_chrome/Default/Preferences'
MANIFEST = {'name': NAME, 'short_name': NAME, 'start_url': '/app/', 'scope': '/app/',
            'display': 'standalone',
            'icons': [{'src': '/icon.png', 'sizes': '192x192', 'type': 'image/png'}]}
APP = '''<!doctype html>
<meta name="viewport" content="width=device-width">
<link rel="manifest" href="/manifest.json">
<title>AppLinksTest</title>
<style>body { margin: 0 } a { display: block; height: 100vh; font: 32px sans-serif }</style>
<a target="_blank" href="/app/window.html">Open a window</a>
'''
# The page listens for touches, as most do, so the browser sends them.
WINDOW = '''<!doctype html>
<meta name="viewport" content="width=device-width">
<title>window</title>
<style>body { margin: 0; height: 300vh } a { display: block; height: 100vh; font: 32px sans-serif }</style>
<a id="plain" href="/out.html?plain">Out of the app</a>
<script>addEventListener('touchstart', () => {}, {passive: true});</script>
'''
OUT = '<!doctype html><title>out</title>Out of the app\n'


def icon():
    """A 192 px PNG of one colour: the manifest's icon, which installing needs."""
    return subprocess.run(['magick', '-size', '192x192', 'xc:#3080c0', 'png:-'],
                          check=True, capture_output=True).stdout


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    handler = functools.partial(QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', PORT), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def home():
    shell('input keyevent KEYCODE_HOME')
    time.sleep(2)


def app_ids():
    """The web app ids of the browser's pinned shortcuts named NAME."""
    ids = []
    for block in shell('dumpsys shortcut').split('ShortcutInfo {id=')[1:]:
        app_id, flags = block.split(', ', 1)
        if (re.match(r'flags=0x[0-9a-f]+ \[Pin', flags)
                and f'packageName={PACKAGE}\n' in block
                and f'shortLabel={NAME},' in block):
            ids.append(app_id)
    return ids


def install_app():
    """Installs the app unless a shortcut of its name is pinned; returns the
    ids of the installed copies."""
    if not app_ids():
        shell(f'am start -a android.intent.action.VIEW -d {ORIGIN}/app/ {PACKAGE}')
        time.sleep(5)
        for step in ('menu', 'Save and share', 'Install and create shortcut', 'Install',
                     'positive', 'Add to home screen'):
            if step == 'menu':
                found = tap(resource_id='menu_button')
            elif step == 'positive':
                found = tap(resource_id='positive_button')
            else:
                found = tap_text(step)
            assert found, f'installing the app: no "{step}"'
            time.sleep(3)
        wait_for(app_ids, 10, 'the app\'s shortcut')
    home()
    return app_ids()


def set_rule(ids):
    """Narrows the scope of each app in IDS with RULE, in the profile's prefs."""
    shell(f'am force-stop {PACKAGE}')
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp, 'Preferences')
        adb('pull', PREFERENCES, str(local))
        prefs = json.loads(local.read_text())
        rules = (prefs.setdefault('oganesson', {}).setdefault('web_apps', {})
                 .setdefault('user_scope_rules', {}))
        if all(rules.get(app_id) == [RULE] for app_id in ids):
            return
        for app_id in ids:
            rules[app_id] = [RULE]
        local.write_text(json.dumps(prefs))
        uid = shell(f'stat -c %u {DATA}').strip()
        adb('push', str(local), PREFERENCES)
        shell(f"chown {uid}:{uid} '{PREFERENCES}'; restorecon '{PREFERENCES}'")


def browser_uid():
    listed = shell(f'cmd package list packages -U {PACKAGE}')
    return next(part.split(':')[1] for line in listed.splitlines()
                for part in line.split() if part.startswith('uid:'))


def opened(uid):
    """The browser's VIEW intents to other apps since the log was cleared."""
    log = adb('logcat', '-d', '-s', 'ActivityTaskManager:I').decode()
    return [line for line in log.splitlines()
            if 'START u0 {act=android.intent.action.VIEW' in line
            and f'from uid {uid} ' in line and f'cmp={PACKAGE}/' not in line]


def open_window(app_id):
    """Starts the app and taps its page, which opens a new window; returns a
    DevTools session with the new window, and its target id."""
    shell(f'am force-stop {PACKAGE}')
    # As its shortcut does, through the launcher that only the browser itself
    # may start, which takes no MAC (WebappLauncherActivity).
    extra = 'org.chromium.chrome.browser.'
    shell(f'am start -n {PACKAGE}/{extra}webapps.SecureWebAppLauncher'
          f' -a {extra}webapps.WebappManager.ACTION_START_SECURE_WEBAPP'
          f' --es {extra}webapp_id {app_id} --es {extra}webapp_url {ORIGIN}/app/'
          f' --es {extra}webapp_scope {ORIGIN}/app/ --es {extra}webapp_name {NAME}'
          f' --es {extra}webapp_short_name {NAME} --ei {extra}webapp_display_mode 3'
          f' --ei {extra}webapp_source 7 --ei {extra}webapp_shortcut_version 3')
    wait_for(lambda: any(t['type'] == 'page' and t['url'] == f'{ORIGIN}/app/'
                         for t in targets()), 30, 'the app')
    time.sleep(3)  # its first paint
    width, height = map(int, shell('wm size').split()[-1].split('x'))
    shell(f'input tap {width // 2} {height // 2}')
    url = f'{ORIGIN}/app/window.html'
    target = wait_for(lambda: next((t for t in targets()
                                    if t['type'] == 'page' and t['url'] == url), None),
                      30, 'the new window')
    window = Devtools(target['webSocketDebuggerUrl'])
    wait_for(lambda: evaluate(window, 'document.readyState') == 'complete', 30,
             'the new window to load')
    return window, target['id']


def run_script(window, script):
    """Runs SCRIPT in the window, printing whether the window had activation
    as it ran."""
    active = evaluate(window, f'[navigator.userActivation.isActive, {script}][0]')
    print('activation at the script:', active)


def evaluate(session, expression):
    result = session.call('Runtime.evaluate', expression=expression, returnByValue=True)
    return result['result'].get('value')


def touch_scroll(window):
    """A real touch that scrolls the window's page."""
    width, height = map(int, shell('wm size').split()[-1].split('x'))
    x = width // 2
    shell(f'input swipe {x} {height * 3 // 4} {x} {height // 4} 300')
    wait_for(lambda: evaluate(window, 'scrollY') > 0, 10, 'the page to scroll')
    time.sleep(1)  # its fling


def run_push(app_id, uid, failures):
    window, _ = open_window(app_id)
    try:
        touch_scroll(window)
        run_script(window, 'history.pushState(null, "", "/out.html?script")')
        time.sleep(3)
        href = evaluate(window, 'location.href')
        if href != f'{ORIGIN}/out.html?script':
            failures.append(f'push after a touch scroll: the window is at {href}')
        if opened(uid):
            failures.append(f'push after a touch scroll: the default browser opened {opened(uid)}')
        # A tap on the link is input: it leaves the app.
        evaluate(window, 'scrollTo(0, 0)')
        time.sleep(1)
        width, height = map(int, shell('wm size').split()[-1].split('x'))
        shell(f'input tap {width // 2} {height // 2}')
        try:
            wait_for(lambda: opened(uid), 10, 'the default browser')
        except AssertionError:
            failures.append('a tap on the out-of-scope link opened nothing in the default'
                            ' browser, so the test can\'t see it open')
    finally:
        window.close()


def run_click(app_id, uid, failures):
    window, window_id = open_window(app_id)
    try:
        run_script(window, 'document.getElementById("plain").click()')
        # From the target list: the page's own context goes away as it navigates.
        url = f'{ORIGIN}/out.html?plain'
        try:
            wait_for(lambda: any(t['id'] == window_id and t['url'] == url for t in targets()),
                     10, 'the link in the window')
        except AssertionError:
            where = [t['url'] for t in targets() if t['id'] == window_id]
            failures.append(f'script link click: the window is at {where}, not {url}')
        if opened(uid):
            failures.append(f'script link click: the default browser opened {opened(uid)}')
    finally:
        window.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('apk', nargs='?')
    args = parser.parse_args()
    if args.apk:
        adb('install', '-r', args.apk)
    adb('root')
    # adbd restarts as root: the device can still be listed just before it goes.
    subprocess.run([ADB, 'wait-for-device'], check=True, timeout=60)
    assert shell('id -u').strip() == '0', 'adb root did not take: the test needs a userdebug build'
    uid = browser_uid()
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, 'app').mkdir()
        Path(tmp, 'app', 'index.html').write_text(APP)
        Path(tmp, 'app', 'window.html').write_text(WINDOW)
        Path(tmp, 'out.html').write_text(OUT)
        Path(tmp, 'manifest.json').write_text(json.dumps(MANIFEST))
        Path(tmp, 'icon.png').write_bytes(icon())
        server = serve(tmp)
        adb('reverse', f'tcp:{PORT}', f'tcp:{PORT}')
        try:
            ids = install_app()
            set_rule(ids)
            for name, case in (('push after a touch scroll', run_push),
                               ('script link click', run_click)):
                print(name)
                before = len(failures)
                shell('am force-stop com.android.chrome')
                subprocess.run([ADB, 'logcat', '-c'], check=True)
                case(ids[0], uid, failures)
                print('pass' if len(failures) == before
                      else '\n'.join(f'FAIL: {f}' for f in failures[before:]))
        except AssertionError as error:
            print(f'FAIL: {error}')
            failures.append(str(error))
        finally:
            shell(f'am force-stop {PACKAGE}')
            shell('am force-stop com.android.chrome')
            home()
            adb('reverse', '--remove', f'tcp:{PORT}')
            server.shutdown()
    print(f'{len(failures)} failed' if failures else 'all passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()

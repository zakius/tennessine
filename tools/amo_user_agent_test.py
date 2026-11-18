#!/usr/bin/env python3
"""End-to-end test of the user agent on addons.mozilla.org (AMO) on Android
(zakius/tennessine#48), on the emulator or a rooted device, with the browser
installed there.

AMO shows its "Add to Firefox" button only to Firefox, so the browser gives
AMO's pages Firefox for Android's user agent (dubna's
firefox_amo_user_agent.cc) and other hosts its own. A local HTTPS server,
which --host-resolver-rules maps addons.mozilla.org and example.com to,
records each page's User-Agent header. With "Desktop site" off, then on, the
test opens AMO from a link and from the address bar, and leaves it the same
two ways. On each page it compares the header and navigator.userAgent with
what the host should get: Firefox for Android's on AMO, and on example.com
the browser's own, or with "Desktop site" on the desktop site's. It puts the
"Desktop site" setting back and removes the command line at the end. With
APK, it first installs that build over the installed one (an x86_64 build for
the emulator: ./tennessine.py build x64).

Usage: tools/amo_user_agent_test.py [APK]
"""
import argparse
import base64
import hashlib
import http.server
import json
import re
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

from screenshots import DATA, DEVTOOLS_PORT, PACKAGE, Devtools, adb, shell, tap
from xpi_install_test import targets, wait_for

AMO = 'addons.mozilla.org'
OTHER = 'example.com'
# The browser reads it on a userdebug build (AndroidInfo.isDebugAndroid());
# SELinux keeps apps out of /data/local itself.
COMMAND_LINE = '/data/local/tmp/chrome-command-line'
PREFERENCES = f'{DATA}/app_chrome/Default/Preferences'
# "Desktop site" for a host, as the app menu's checkbox sets it: an exception
# of this content setting, 1 to allow. The global default in Preferences
# doesn't switch it on this build.
DESKTOP_SITE = 'request_desktop_site'
SITES = [f'[*.]{host},*' for host in (AMO, OTHER)]
# What ContentUtils.setUserAgentOverride() gives "Desktop site".
DESKTOP_UA = re.compile(r'Mozilla/5\.0 \(X11; [^)]+\) AppleWebKit/.+ Chrome/.+')


class Server(http.server.ThreadingHTTPServer):
    links = {}  # a page's URL: the URL that its link goes to
    user_agents = {}  # a page's URL: the User-Agent header of its last load

    def handle_error(self, request, client_address):
        pass  # a connection that the browser opened and closed unused


class Handler(http.server.BaseHTTPRequestHandler):
    def setup(self):
        self.request.do_handshake()  # in this connection's own thread
        super().setup()

    def do_GET(self):
        url = f'https://{self.headers["Host"]}{self.path}'
        self.server.user_agents[url] = self.headers['User-Agent']
        body = '<!doctype html><meta name="viewport" content="width=device-width">'
        if url in self.server.links:
            # The link fills the page, so the middle of the screen is on it.
            body += f'<a href="{self.server.links[url]}" style="position:fixed;inset:0">next</a>'
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def certificate(directory):
    """A self-signed certificate for both hosts; returns its SPKI hash, which
    --ignore-certificate-errors-spki-list takes."""
    cert, key = directory / 'cert.pem', directory / 'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
                    '-keyout', key, '-out', cert, '-subj', f'/CN={AMO}',
                    '-addext', f'subjectAltName=DNS:{AMO},DNS:{OTHER}'],
                   check=True, capture_output=True)
    public_key = subprocess.run(['openssl', 'x509', '-in', cert, '-pubkey', '-noout'],
                                check=True, capture_output=True).stdout
    der = subprocess.run(['openssl', 'pkey', '-pubin', '-outform', 'der'],
                         input=public_key, check=True, capture_output=True).stdout
    return base64.b64encode(hashlib.sha256(der).digest()).decode()


def serve(directory):
    server = Server(('127.0.0.1', 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(directory / 'cert.pem', directory / 'key.pem')
    # Handler.setup() does each handshake, so that a slow client doesn't hold
    # up the others.
    server.socket = context.wrap_socket(server.socket, server_side=True,
                                        do_handshake_on_connect=False)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def edit_preferences(edit):
    """Calls EDIT with the stopped browser's "Desktop site" exceptions, then
    saves them."""
    shell(f'am force-stop {PACKAGE}')
    uid = shell(f'stat -c %u {DATA}').strip()
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp, 'Preferences')
        adb('pull', PREFERENCES, str(local))
        prefs = json.loads(local.read_text())
        exceptions = prefs.setdefault('profile', {}).setdefault('content_settings', {})
        edit(exceptions.setdefault('exceptions', {}).setdefault(DESKTOP_SITE, {}))
        local.write_text(json.dumps(prefs))
        adb('push', str(local), PREFERENCES)
    shell(f'chown {uid}:{uid} {PREFERENCES}; restorecon {PREFERENCES}')


def set_desktop_site(values):
    """Sets each of SITES' exception to VALUES' (a dict), or removes it."""
    def edit(exceptions):
        for site in SITES:
            if values.get(site) is None:
                exceptions.pop(site, None)
            else:
                exceptions[site] = values[site]
    edit_preferences(edit)


def browser_user_agent():
    """The browser's own user agent, once DevTools answers; None before."""
    if not targets():
        return None
    url = f'http://127.0.0.1:{DEVTOOLS_PORT}/json/version'
    return json.loads(urllib.request.urlopen(url, timeout=10).read())['User-Agent']


def page(url):
    return next((t for t in targets() if t['type'] == 'page' and t['url'] == url), None)


def check(server, url, expected, what):
    """Waits for the tab to show URL; returns a failure, or None if its header
    and navigator.userAgent match EXPECTED, a string or a pattern."""
    target = wait_for(lambda: page(url), 30, url)
    session = Devtools(target['webSocketDebuggerUrl'])
    wait_for(lambda: session.call('Runtime.evaluate', expression='document.readyState')
             ['result']['value'] == 'complete', 30, f'{url} to load')
    script = session.call('Runtime.evaluate', expression='navigator.userAgent')['result']['value']
    session.close()
    header = server.user_agents.get(url)
    for name, user_agent in (('header', header), ('navigator.userAgent', script)):
        matches = (user_agent == expected if isinstance(expected, str)
                   else bool(user_agent and expected.fullmatch(user_agent)))
        if not matches:
            return f'{what}: {name} on {url} is {user_agent!r}'
    print(f'pass: {what}')
    return None


def tap_link():
    width, height = map(int, re.search(r'(\d+)x(\d+)', shell('wm size')).groups())
    shell(f'input tap {width // 2} {height // 2}')


def tap_url_bar():
    try:
        return tap(resource_id='url_bar')
    except subprocess.CalledProcessError:
        return False  # uiautomator, killed under a high load: try again


def type_url(url):
    wait_for(tap_url_bar, 30, 'the address bar')
    time.sleep(1)
    shell(f"input text '{url}'")
    shell('input keyevent KEYCODE_ENTER')


def run_mode(server, desktop):
    """Opens AMO and leaves it, from a link and from the address bar; returns
    the failures."""
    mode = 'desktop' if desktop else 'mobile'
    base = f'/{int(time.time())}/{mode}'
    start, linked = f'https://{OTHER}{base}/start', f'https://{AMO}{base}/link'
    left_link, typed = f'https://{OTHER}{base}/left-link', f'https://{AMO}{base}/typed'
    left_typed = f'https://{OTHER}{base}/left-typed'
    server.links.update({start: linked, linked: left_link})
    set_desktop_site({site: {'setting': 1} for site in SITES} if desktop else {})
    shell(f'am start -a android.intent.action.VIEW -d {start} {PACKAGE}')
    own_ua = wait_for(browser_user_agent, 30, 'DevTools')
    major = re.search(r'Chrome/(\d+)\.', own_ua).group(1)
    firefox_ua = f'Mozilla/5.0 (Android 10; Mobile; rv:{major}.0) Gecko/{major}.0 Firefox/{major}.0'
    other_ua = DESKTOP_UA if desktop else own_ua
    failures = []
    steps = [
        (start, other_ua, None, f'{mode}: {OTHER} before AMO'),
        (linked, firefox_ua, tap_link, f'{mode}: AMO from a link'),
        (left_link, other_ua, tap_link, f'{mode}: {OTHER} from a link on AMO'),
        (typed, firefox_ua, lambda: type_url(typed), f'{mode}: AMO from the address bar'),
        (left_typed, other_ua, lambda: type_url(left_typed),
         f'{mode}: {OTHER} from the address bar on AMO'),
    ]
    for url, expected, go, what in steps:
        try:
            if go:
                time.sleep(2)  # the page before's first paint
                go()
            failure = check(server, url, expected, what)
        except AssertionError as error:
            failure = f'{what}: {error}'
        if failure:
            print(f'FAIL: {failure}')
            failures.append(failure)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('apk', nargs='?')
    args = parser.parse_args()
    adb('root')
    wait_for(lambda: b'\tdevice' in adb('devices'), 30, 'adb as root')
    if args.apk:
        adb('install', '-r', args.apk)
    saved = {}
    edit_preferences(lambda exceptions: saved.update({site: exceptions.get(site) for site in SITES}))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        spki = certificate(tmp)
        server = serve(tmp)
        port = server.server_address[1]
        adb('reverse', f'tcp:{port}', f'tcp:{port}')
        # Read at each start of the browser.
        rules = f'MAP {AMO} 127.0.0.1:{port},MAP {OTHER} 127.0.0.1:{port}'
        (tmp / 'command-line').write_text(f'_ --host-resolver-rules="{rules}"'
                                          f' --ignore-certificate-errors-spki-list={spki}\n')
        adb('push', str(tmp / 'command-line'), COMMAND_LINE)
        shell(f'chmod 644 {COMMAND_LINE}')
        failures = []
        try:
            for desktop in (False, True):
                failures += run_mode(server, desktop)
        finally:
            shell(f'rm -f {COMMAND_LINE}')
            set_desktop_site(saved)
            adb('reverse', '--remove', f'tcp:{port}')
            server.shutdown()
    print(f'{len(failures)} failed' if failures else 'all passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()

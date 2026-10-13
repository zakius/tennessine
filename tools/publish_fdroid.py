#!/usr/bin/env python3
"""Publishes an APK in tennessine's F-Droid repository, https://packages.zakius.xyz/fdroid/repo.

Usage: tools/publish_fdroid.py [APK]   (default: the local build's arm64 APK)

The repository lives in ~/.local/share/tennessine-fdroid: config.yml, the index signing key
(keystore.p12, which exists only there: back the folder up), metadata/ and repo/. fdroid update,
from F-Droid's fdroidserver image, renames the APK to <package>_<versionCode>.apk, keeps the
newest 3 in repo/ and moves older ones to archive/, and signs the index. repo/ and archive/ then
go to oracle:fdroid/www, served by nginx (tools/fdroid-compose.yaml) behind the host's
Traefik. An APK the repository already has is not added again.
"""
import hashlib, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = Path.home() / '.local/share/tennessine-fdroid'
TARGET = 'oracle:fdroid'
IMAGE = 'registry.gitlab.com/fdroid/docker-executable-fdroidserver:master'
DEFAULT_APK = ROOT / 'work/build/tennessine/chromium/src/out/release'


def run(*args):
    subprocess.run(args, check=True)


def main():
    apks = [Path(sys.argv[1])] if len(sys.argv) > 1 else sorted(DEFAULT_APK.glob('*-arm64-v8a.apk'))
    if len(apks) != 1 or not apks[0].is_file():
        sys.exit(f'publish_fdroid.py: expected one APK, found {[str(a) for a in apks]}')
    if not (REPO / 'keystore.p12').is_file():
        sys.exit(f'publish_fdroid.py: no signing key in {REPO}')
    digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
    have = {digest(f) for d in ('repo', 'archive') for f in (REPO / d).glob('*.apk')}
    if digest(apks[0]) not in have:
        shutil.copy2(apks[0], REPO / 'repo' / apks[0].name)
    run('docker', 'run', '--rm', '-u', f'{os.getuid()}:{os.getgid()}', '-e', 'HOME=/tmp',
        # The image checks its own fdroidserver checkout, owned by another user.
        '-e', 'GIT_CONFIG_COUNT=1', '-e', 'GIT_CONFIG_KEY_0=safe.directory',
        '-e', 'GIT_CONFIG_VALUE_0=*', '-v', f'{REPO}:/repo', '-w', '/repo', IMAGE,
        'update', '--rename-apks')
    host, path = TARGET.split(':', 1)
    run('ssh', host, f'mkdir -p {path}/www')
    run('rsync', '-a', '--delete', f'{REPO}/repo', f'{REPO}/archive', f'{TARGET}/www/')
    run('rsync', '-a', str(ROOT / 'tools/fdroid-compose.yaml'), f'{TARGET}/compose.yaml')
    run('ssh', host, f'cd {path} && docker compose up -d --quiet-pull')


if __name__ == '__main__':
    main()

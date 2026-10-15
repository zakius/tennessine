#!/usr/bin/env python3
"""tennessine's commands. Stdlib only, Python 3.10.

apply [--stop]       prepares the Chromium tree in the current directory:
                     1. dubna/src/ (Firefox Sync, no saved history: shared with desktop),
                        then src/, copied over the tree.
                     2. Vanadium's patches, straight from the vanadium submodule, renamed to
                        tennessine, minus patches/vanadium-drops.txt.
                     3. patches/series: paths from the repo root, so dubna/ patches come
                        from that submodule. A series entry from a GPL-3.0 source fails:
                        Vanadium's patches are GPL-2.0-only (see LICENSING below).
                     4. dubna's Polish translations of its strings.
                     --stop stops at the first failure, for refresh.
check [WORKDIR] [--stop]
                     apply on a Chromium checkout at Vanadium's version, zero fuzz. The
                     checkout (default work/chromium/src) is shallow, one tag per version,
                     and reused; fix a failed patch in place there and refresh it.
refresh PATCH [FILE...]
                     in the tree: applies a patch that no longer applies cleanly with fuzz,
                     then rewrites it from the result, keeping its header comment. The tree
                     must hold everything before the patch and nothing after (check --stop).
                     Rejected hunks are left as *.rej. Fuzz can land a hunk in the wrong
                     place: read every hunk patch reports with fuzz, fix the tree, then take
                     the fix with FILE: rewrites FILE's part of PATCH from the tree's git
                     diff (only when no other patch edits FILE).
upstream             lists newer upstream tags (vanadium, titanium), a newer uBlock Origin
                     release than the installer's pin, and dubna commits; exits 1 when
                     there is one.
build [resume|x64]   arm64 APK in a rootless Ubuntu container on a scratch copy in
                     work/build (tools/container-build.sh), signed with the key in
                     ~/.local/share/tennessine-signing.
                     Output: work/build/tennessine/chromium/src/out/release/*.apk.
                     resume: copies src/ over the prepared tree, then compiles and signs;
                     enough when only src/ changed. x64: the same for an x86_64 APK in
                     out/x64, for the emulator, whose ARM translation crashes arm64 Chromium.
publish [APK]        puts the APK (default: the local build's arm64 one) in the F-Droid
                     repository, https://packages.zakius.xyz/fdroid/repo.

LICENSING: Vanadium's patches are GPL-2.0-only and allow no GPL-3.0 code in the same
program, so nothing GPL-3.0 is built in: not Helium's or Cromite's patches. dubna is used
under its GPL-2.0 option. uBlock Origin (GPL-3.0, Helium's build) is a separate program
that the browser downloads and installs on first run (src/chrome/browser/ublock_installer).
"""
import fnmatch, hashlib, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VERSION_RE = r'\d+(?:\.\d+){3}'


def run(*args, **kw):
    return subprocess.run(args, check=True, **kw)


def vanadium_version():
    return re.search(VERSION_RE, (ROOT / 'vanadium/args.gn').read_text())[0]


# Series sources whose code is GPL-3.0, which Vanadium's GPL-2.0-only license excludes.
GPL3_SOURCES = ('helium/', 'patches/cromite/')
# The uBlock Origin build the installer downloads: its pin is in this file.
UBLOCK_INSTALLER = ROOT / 'src/chrome/browser/ublock_installer/ublock_installer.cc'


def apply(stop=False):
    tree = Path.cwd()
    for top in (ROOT / 'dubna/src', ROOT / 'src'):
        shutil.copytree(top, tree, dirs_exist_ok=True)

    drops = [line.split('\t')[0] for line in (ROOT / 'patches/vanadium-drops.txt').read_text()
             .splitlines() if line and not line.startswith('#')]
    vanadium = sorted((ROOT / 'vanadium/patches').glob('*.patch'))
    for glob in drops:
        if not any(fnmatch.fnmatch(p.name, glob) for p in vanadium):
            print(f'WARN  no Vanadium patch matches drop {glob}')
    rename = (('VANADIUM', 'TENNESSINE'), ('Vanadium', 'Tennessine'), ('vanadium', 'tennessine'))
    patches = []
    for p in vanadium:
        if not any(fnmatch.fnmatch(p.name, glob) for glob in drops):
            text = p.read_text()
            for old, new in rename:
                text = text.replace(old, new)
            patches.append((f'vanadium/{p.name}', text))
    for line in (ROOT / 'patches/series').read_text().splitlines():
        if line and not line.startswith('#'):
            if line.startswith(GPL3_SOURCES):
                sys.exit(f'tennessine: {line} is GPL-3.0 code, which Vanadium\'s GPL-2.0-only '
                         'license excludes (LICENSING in tennessine.py)')
            patches.append((line, (ROOT / line).read_text()))

    failed = False
    for name, text in patches:
        result = subprocess.run(['git', 'apply', '--whitespace=nowarn'], input=text, text=True,
                                capture_output=True)
        if result.returncode == 0:
            print(f'ok    {name}')
            continue
        print(f'FAIL  {name}')
        print(''.join(f'      {line}\n' for line in (result.stdout + result.stderr).splitlines()),
              end='')
        failed = True
        if stop:
            return 1
    # dubna's translations of its strings, after the patches add the grdp.
    sys.stdout.flush()
    if subprocess.run([sys.executable, str(ROOT / 'dubna/tools/translate.py'), str(tree),
                       str(ROOT / 'dubna')]).returncode:
        failed = True
    return 1 if failed else 0


def check(workdir, stop):
    version = vanadium_version()
    src = Path(workdir or ROOT / 'work/chromium') / 'src'
    src.mkdir(parents=True, exist_ok=True)
    git = lambda *args, **kw: subprocess.run(['git', '-C', str(src), *args], **kw)
    if not (src / '.git').is_dir():
        git('init', '-q', check=True)
        git('remote', 'add', 'origin', 'https://chromium.googlesource.com/chromium/src.git',
            check=True)
    if git('rev-parse', '-q', '--verify', f'refs/tags/{version}', capture_output=True).returncode:
        git('fetch', '-q', '--depth=1', 'origin', f'refs/tags/{version}:refs/tags/{version}',
            check=True)
    git('checkout', '-q', '-f', f'refs/tags/{version}', check=True)
    git('clean', '-qfd', check=True)
    os.chdir(src)
    code = apply(stop)
    milestone = (ROOT / 'dubna/milestone.txt').read_text().strip()
    if version.split('.')[0] != milestone:
        print(f'WARN  Chromium {version} is not on milestone {milestone} of dubna/milestone.txt; '
              'only an exploit in the wild allows that')
    return code


def section_span(text, name):
    """(start, end) of name's section in a patch."""
    f = re.escape(name)
    start = re.compile(rf'^(diff --git a/{f} b/{f}\n(index .*\n)?)?--- (a/{f}|/dev/null)\n',
                       re.M).search(text)
    end = re.compile(r'^(diff --git |--- (a/|/dev/null))', re.M).search(text, start.end() + 1)
    return start.start(), end.start() if end else len(text)


def refresh(patch, files):
    patch = Path(patch).resolve()
    if files:
        for name in files:
            diff = run('git', 'diff', '--src-prefix=a/', '--dst-prefix=b/', '--', name,
                       capture_output=True, text=True).stdout
            part = diff[diff.index('\n--- ') + 1:] if '\n--- ' in diff else ''
            text = patch.read_text()
            start, end = section_span(text, name)
            patch.write_text(text[:start] + part + text[end:])
            print(f'refreshed {name} in {patch}')
        return 0
    text = patch.read_text()
    names = re.findall(r'^\+\+\+ b/(\S+)', text, re.M)
    with tempfile.TemporaryDirectory() as before:
        for name in names:
            if Path(name).is_file():
                (Path(before) / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(name, Path(before) / name)
        subprocess.run(['patch', '-p1', '-F3', '--no-backup-if-mismatch'], input=text, text=True)
        header = re.split(r'(?m)^(?:---|diff) ', text, maxsplit=1)[0]
        parts = []
        for name in names:
            old = Path(before) / name
            old, label = (old, f'a/{name}') if old.is_file() else ('/dev/null', '/dev/null')
            parts.append(subprocess.run(['diff', '-u', '--label', label, '--label', f'b/{name}',
                                         str(old), name], capture_output=True, text=True).stdout)
    patch.write_text(header + ''.join(parts))
    print(f'refreshed {patch}')
    return 0


def upstream():
    def remote(*args):
        return run('git', 'ls-remote', *args, capture_output=True, text=True).stdout
    url = lambda m: run('git', '-C', str(ROOT), 'config', '-f', '.gitmodules', f'submodule.{m}.url',
                        capture_output=True, text=True).stdout.strip()
    key = lambda tag: [int(n) if n.isdigit() else n for n in re.split(r'(\d+)', tag)]
    new = 0
    for m in ('vanadium', 'titanium'):
        tags = re.findall(r'refs/tags/(\S+)', remote('--tags', '--refs', url(m)))
        want = max(tags, key=key)
        have = subprocess.run(['git', '-C', str(ROOT / m), 'describe', '--tags', '--exact-match'],
                              capture_output=True, text=True).stdout.strip() or '?'
        if have != want:
            print(f'{m}: {have} -> {want}')
            new = 1
    pin = re.search(r'kVersion\[\] = "([^"]+)"', UBLOCK_INSTALLER.read_text())[1]
    tags = re.findall(r'refs/tags/(\S+)', remote('--tags', '--refs',
                                                  'https://github.com/imputnet/uBlock.git'))
    latest = max(tags, key=key)
    if key(latest) > key(pin):
        print(f'uBlock Origin: {pin} -> {latest} (pin in {UBLOCK_INSTALLER.relative_to(ROOT)})')
        new = 1
    have = run('git', '-C', str(ROOT / 'dubna'), 'rev-parse', 'HEAD', capture_output=True,
               text=True).stdout.strip()
    want = remote(url('dubna'), 'refs/heads/main').split()[0]
    if have != want:
        print(f'dubna: {have[:7]} -> {want[:7]}')
        new = 1
    return new


def build(mode):
    work = ROOT / 'work'
    (work / 'build/tennessine').mkdir(parents=True, exist_ok=True)
    if not (KEYS / 'test.jks').is_file():
        sys.exit(f'tennessine: no APK signing key in {KEYS}')
    run('rsync', '-a', '--delete', '--exclude=/work', '--exclude=/dubna/rust/oganesson_sync/target',
        '--exclude=/chromium', '--exclude=/keys', '--exclude=/depot_tools', f'{ROOT}/',
        f'{work}/build/tennessine/')
    # --name: one build at a time in the shared tree.
    os.execvp('podman', ['podman', 'run', '--rm', '-i', '--pids-limit=-1',
                         '--name', 'tennessine-build',
                         '-v', f'{work}/build:/build', '-v', f'{KEYS}:/keys',
                         '-w', '/build/tennessine', '-e', 'LOCAL_BUILD=1', '-e', f'MODE={mode}',
                         '-e', f'JOBS={os.environ.get("JOBS", "28")}',
                         # The container's first process ignores SIGTERM unless it traps it:
                         # this one exits on it, which ends the whole build, so stopping podman
                         # stops the build too.
                         'docker.io/library/ubuntu:24.04', 'sh', '-c',
                         'trap "exit 143" TERM; bash tools/container-build.sh & wait $!'])


# The APK signing key (test.jks, local.properties): every update must be signed with it, or
# Android won't install it over the last one. It exists only there: back the folder up.
KEYS = Path.home() / '.local/share/tennessine-signing'
FDROID = Path.home() / '.local/share/tennessine-fdroid'
FDROID_IMAGE = 'registry.gitlab.com/fdroid/docker-executable-fdroidserver:master'


def env(name):
    """NAME from the environment, else from tennessine's .env, which Git ignores: where things
    are deployed stays out of the repository."""
    if name in os.environ:
        return os.environ[name]
    path = ROOT / '.env'
    for line in path.read_text().splitlines() if path.exists() else []:
        key, _, value = line.partition('=')
        if key.strip() == name:
            return value.strip()
    sys.exit(f'tennessine: set {name} in the environment or in {path}, e.g. {name}=myserver:fdroid')



def publish(apk):
    """The repository lives in ~/.local/share/tennessine-fdroid: config.yml, the index signing
    key (keystore.p12, which exists only there: back the folder up), metadata/ and repo/.
    fdroid update, from F-Droid's fdroidserver image, renames the APK to
    <package>_<versionCode>.apk, keeps the newest 3 in repo/ and moves older ones to archive/,
    and signs the index. repo/ and archive/ then go to FDROID_TARGET/www (HOST:DIR, from .env), served by nginx
    (tools/fdroid-compose.yaml) behind the host's Traefik. An APK the repository already has is
    not added again."""
    out = ROOT / 'work/build/tennessine/chromium/src/out/release'
    apks = [Path(apk)] if apk else sorted(out.glob('*-arm64-v8a.apk'))
    if len(apks) != 1 or not apks[0].is_file():
        sys.exit(f'tennessine: expected one APK, found {[str(a) for a in apks]}')
    if not (FDROID / 'keystore.p12').is_file():
        sys.exit(f'tennessine: no signing key in {FDROID}')
    digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
    have = {digest(f) for d in ('repo', 'archive') for f in (FDROID / d).glob('*.apk')}
    if digest(apks[0]) not in have:
        shutil.copy2(apks[0], FDROID / 'repo' / apks[0].name)
    run('docker', 'run', '--rm', '-u', f'{os.getuid()}:{os.getgid()}', '-e', 'HOME=/tmp',
        # The image checks its own fdroidserver checkout, owned by another user.
        '-e', 'GIT_CONFIG_COUNT=1', '-e', 'GIT_CONFIG_KEY_0=safe.directory',
        '-e', 'GIT_CONFIG_VALUE_0=*', '-v', f'{FDROID}:/repo', '-w', '/repo', FDROID_IMAGE,
        'update', '--rename-apks')
    target = env('FDROID_TARGET')
    host, path = target.split(':', 1)
    run('ssh', host, f'mkdir -p {path}/www')
    run('rsync', '-a', '--delete', f'{FDROID}/repo', f'{FDROID}/archive', f'{target}/www/')
    run('rsync', '-a', str(ROOT / 'tools/fdroid-compose.yaml'), f'{target}/compose.yaml')
    run('ssh', host, f'cd {path} && docker compose up -d --quiet-pull')
    return 0


def main():
    args = sys.argv[1:]
    command = args.pop(0) if args else ''
    stop = '--stop' in args
    args = [a for a in args if a != '--stop']
    if command == 'apply' and not args:
        return apply(stop)
    if command == 'check' and len(args) <= 1:
        return check(args[0] if args else None, stop)
    if command == 'refresh' and args:
        return refresh(args[0], args[1:])
    if command == 'upstream' and not args:
        return upstream()
    if command == 'build' and args in ([], ['resume'], ['x64']):
        return build(args[0] if args else '')
    if command == 'publish' and len(args) <= 1:
        return publish(args[0] if args else None)
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main())
